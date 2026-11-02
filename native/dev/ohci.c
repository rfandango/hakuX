/*
 * hakuX native core — the MCPX's first OHCI USB controller (OpenHCI 1.0a):
 * registers at 0xFED00000, IRQ 1, four root ports.
 *
 * The title's own USB stack (XAPI) drives it. A frame thread does each
 * 1 ms frame's work: the periodic list from the HCCA interrupt table, then
 * the control and bulk lists, then the done queue. Transfers complete at
 * once (the devices are in-process); a NAK leaves the TD for a later frame.
 *
 * Topology (the 1.0 console's): the front panel hub on root port 1, and on
 * its ports 3, 4, 1, 2 one controller per player — a 3-port hub with the
 * gamepad on port 1.
 */
#include "native/dev/usb.h"
#include "native/core/irq.h"
#include "native/core/mmio.h"
#include "native/core/vm.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#define OHCI_BASE  0xFED00000u
#define OHCI_SIZE  0x1000u
#define OHCI_IRQ   1
#define ROOT_PORTS 4

/* HcControl */
#define CTL_PLE      (1u << 2)
#define CTL_IE       (1u << 3)
#define CTL_CLE      (1u << 4)
#define CTL_BLE      (1u << 5)
#define CTL_HCFS     (3u << 6)
#define HCFS_OPERATIONAL (2u << 6)
#define HCFS_SUSPEND     (3u << 6)
#define CTL_IR       (1u << 8)

/* HcCommandStatus */
#define CS_HCR (1u << 0)
#define CS_CLF (1u << 1)
#define CS_BLF (1u << 2)
#define CS_OCR (1u << 3)

/* HcInterruptStatus / Enable */
#define INT_SO   (1u << 0)
#define INT_WDH  (1u << 1)
#define INT_SF   (1u << 2)
#define INT_RD   (1u << 3)
#define INT_UE   (1u << 4)
#define INT_FNO  (1u << 5)
#define INT_RHSC (1u << 6)
#define INT_OC   (1u << 30)
#define INT_MIE  (1u << 31)

/* HcRhDescriptorA */
#define RHA_NPS     (1u << 9)
#define RHA_RW_MASK (0x00001b00u | (0xffu << 24))

/* HcRhStatus */
#define RHS_LPS  (1u << 0)
#define RHS_OCI  (1u << 1)
#define RHS_DRWE (1u << 15)
#define RHS_LPSC (1u << 16)
#define RHS_OCIC (1u << 17)
#define RHS_CRWE (1u << 31)

/* HcRhPortStatus */
#define PORT_CCS  (1u << 0)
#define PORT_PES  (1u << 1)
#define PORT_PSS  (1u << 2)
#define PORT_POCI (1u << 3)
#define PORT_PRS  (1u << 4)
#define PORT_PPS  (1u << 8)
#define PORT_LSDA (1u << 9)
#define PORT_CSC  (1u << 16)
#define PORT_PESC (1u << 17)
#define PORT_PSSC (1u << 18)
#define PORT_OCIC (1u << 19)
#define PORT_PRSC (1u << 20)
#define PORT_WTC  (PORT_CSC | PORT_PESC | PORT_PSSC | PORT_OCIC | PORT_PRSC)

/* Endpoint descriptor */
#define ED_FA(x)  ((x) & 0x7f)
#define ED_EN(x)  (((x) >> 7) & 0xf)
#define ED_D(x)   (((x) >> 11) & 3)
#define ED_K      (1u << 14)
#define ED_F      (1u << 15)
#define ED_H      1u
#define ED_C      2u
#define PTR_MASK  0xfffffff0u

/* General transfer descriptor */
#define TD_R       (1u << 18)
#define TD_DP(x)   (((x) >> 19) & 3)
#define TD_DI(x)   (((x) >> 21) & 7)
#define TD_T0      (1u << 24)
#define TD_T1      (1u << 25)
#define TD_EC_MASK (3u << 26)
#define TD_CC_SHIFT 28

#define PID_SETUP 0
#define PID_OUT   1
#define PID_IN    2

#define CC_NOERROR           0
#define CC_STALL             4
#define CC_DEVICENOTRESPONDING 5
#define CC_DATAOVERRUN       8
#define CC_DATAUNDERRUN      9

/* Isochronous transfer descriptor (32 bytes): word 0 the starting frame, DI,
 * FrameCount - 1 and CC; 1 BufferPage0; 2 NextTD; 3 BufferEnd; 4-7 eight
 * Offset/PSW halfwords. */
#define ITD_SF(x)  ((x) & 0xffffu)
#define ITD_FC(x)  (((x) >> 24) & 7)
#define PSW_CC_SHIFT 12

typedef struct Ohci {
    NcVm *vm;
    pthread_mutex_t lock;
    pthread_cond_t run;

    uint32_t ctl, status, intr_status, intr;
    uint32_t hcca, ctrl_head, ctrl_cur, bulk_head, bulk_cur, per_cur;
    uint32_t done;
    int done_count;
    uint32_t fi, fsmps, fit, pstart, lst;
    uint16_t frame_number;
    uint64_t frame_t0_ns;
    uint32_t rhdesc_a, rhdesc_b, rhstatus;
    uint32_t port[ROOT_PORTS];
    NcUsbDev *dev[ROOT_PORTS];
    bool irq;
    uint64_t frames, tds;
} Ohci;

static Ohci g_ohci = {.lock = PTHREAD_MUTEX_INITIALIZER, .run = PTHREAD_COND_INITIALIZER};
static bool g_trace; /* NC_TRACE_USB: every transfer but NAKed polls */

/* Each player's controller (a hub with the gamepad on its port 1), made at
 * boot; its root port has it while the player has a controller
 * (nc_dev_ohci_set_pads). */
static const unsigned k_port_map[4] = {3, 4, 1, 2};
static NcUsbDev *g_pad_hub[4];
static bool g_front_hub;
static bool g_ready;                    /* nc_dev_ohci_init done */
static int g_early_mask = -1;           /* nc_dev_ohci_set_pads before it: the boot's */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Guest physical memory, bounds-checked. */
static uint8_t *phys(Ohci *o, uint32_t pa, uint32_t len)
{
    if ((uint64_t)pa + len > o->vm->ram_size) {
        return NULL;
    }
    return o->vm->phys + pa;
}

static bool rd_words(Ohci *o, uint32_t pa, uint32_t *w, unsigned n)
{
    const uint8_t *p = phys(o, pa, 4 * n);
    if (!p) {
        return false;
    }
    for (unsigned i = 0; i < n; i++) {
        w[i] = nc_rd32(p + 4 * i);
    }
    return true;
}

static void wr_words(Ohci *o, uint32_t pa, const uint32_t *w, unsigned n)
{
    uint8_t *p = phys(o, pa, 4 * n);
    if (p) {
        for (unsigned i = 0; i < n; i++) {
            nc_wr32(p + 4 * i, w[i]);
        }
    }
}

static void update_irq(Ohci *o)
{
    const bool level = (o->intr & INT_MIE) && (o->intr_status & o->intr & ~INT_MIE);
    if (level != o->irq) {
        o->irq = level;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        nc_irq_set(OHCI_IRQ, level);
    }
}

static void set_interrupt(Ohci *o, uint32_t bits)
{
    o->intr_status |= bits;
    update_irq(o);
}

/* ------------------------------------------------------------- root hub */

static void port_attach(Ohci *o, unsigned i)
{
    if (o->dev[i]) {
        o->port[i] |= PORT_CCS | PORT_CSC | (o->dev[i]->low_speed ? PORT_LSDA : 0);
        set_interrupt(o, INT_RHSC);
    }
}

static void roothub_reset(Ohci *o)
{
    o->rhdesc_a = RHA_NPS | ROOT_PORTS;
    o->rhdesc_b = 0;
    o->rhstatus = 0;
    for (unsigned i = 0; i < ROOT_PORTS; i++) {
        o->port[i] = PORT_PPS;
        if (o->dev[i]) {
            nc_usb_reset(o->dev[i]);
            port_attach(o, i);
        }
    }
}

static void soft_reset(Ohci *o)
{
    o->ctl = (o->ctl & CTL_IR) | HCFS_SUSPEND;
    o->status = 0;
    o->intr_status = 0;
    o->intr = INT_MIE;
    o->hcca = 0;
    o->ctrl_head = o->ctrl_cur = o->bulk_head = o->bulk_cur = o->per_cur = 0;
    o->done = 0;
    o->done_count = 7;
    o->fsmps = 0x2778;
    o->fi = 0x2edf;
    o->fit = 0;
    o->frame_number = 0;
    o->pstart = 0;
    o->lst = 0x628;
    update_irq(o);
}

/* SetPortEnable/Suspend/Reset on a port with nothing on it only sets CSC. */
static bool port_set_if_connected(Ohci *o, unsigned i, uint32_t bit)
{
    if (!bit) {
        return false;
    }
    if (!(o->port[i] & PORT_CCS)) {
        o->port[i] |= PORT_CSC;
        return false;
    }
    if (o->port[i] & bit) {
        return false;
    }
    o->port[i] |= bit;
    return true;
}

static void port_write(Ohci *o, unsigned i, uint32_t val)
{
    const uint32_t old = o->port[i];
    if (val & PORT_WTC) {
        o->port[i] &= ~(val & PORT_WTC);
    }
    if (val & PORT_CCS) { /* ClearPortEnable */
        o->port[i] &= ~PORT_PES;
    }
    port_set_if_connected(o, i, val & PORT_PES);
    port_set_if_connected(o, i, val & PORT_PSS);
    if (val & PORT_POCI) { /* ClearSuspendStatus */
        if (o->port[i] & PORT_PSS) {
            o->port[i] = (o->port[i] & ~PORT_PSS) | PORT_PSSC;
        }
    }
    if (port_set_if_connected(o, i, val & PORT_PRS)) {
        nc_usb_reset(o->dev[i]);
        o->port[i] &= ~PORT_PRS;
        o->port[i] |= PORT_PES | PORT_PRSC;
    }
    /* Power is never switched (NPS): the port stays powered. */
    if (old != o->port[i]) {
        set_interrupt(o, INT_RHSC);
    }
}

static NcUsbDev *find_device(Ohci *o, uint8_t addr)
{
    for (unsigned i = 0; i < ROOT_PORTS; i++) {
        if (o->dev[i] && (o->port[i] & PORT_PES) && !(o->port[i] & PORT_PSS)) {
            NcUsbDev *d = nc_usb_find(o->dev[i], addr);
            if (d) {
                return d;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ transfers */

/* A TD's buffer: CBP..BE inclusive, possibly crossing into BE's page. */
static uint32_t td_len(uint32_t cbp, uint32_t be)
{
    if (!cbp) {
        return 0;
    }
    if ((cbp ^ be) & ~0xfffu) {
        return (0x1000u - (cbp & 0xfffu)) + (be & 0xfffu) + 1;
    }
    return be - cbp + 1;
}

static void td_copy(Ohci *o, uint32_t cbp, uint32_t be, uint8_t *buf, uint32_t len, bool to_guest)
{
    uint32_t first = len;
    if ((cbp ^ be) & ~0xfffu) {
        first = MIN(len, 0x1000u - (cbp & 0xfffu));
    }
    uint8_t *p = phys(o, cbp, first);
    if (p) {
        to_guest ? memcpy(p, buf, first) : memcpy(buf, p, first);
    }
    if (len > first) {
        p = phys(o, be & ~0xfffu, len - first);
        if (p) {
            to_guest ? memcpy(p, buf + first, len - first) : memcpy(buf + first, p, len - first);
        }
    }
}

/* Service the head TD of an ED. Returns false if the ED is waiting (NAK). */
static bool service_td(Ohci *o, uint32_t ed_pa, uint32_t ed[4])
{
    const uint32_t td_pa = ed[2] & PTR_MASK;
    uint32_t td[4];
    if (!rd_words(o, td_pa, td, 4)) {
        ed[2] |= ED_H;
        return false;
    }
    int pid;
    switch (ED_D(ed[0])) {
    case 1:
        pid = PID_OUT;
        break;
    case 2:
        pid = PID_IN;
        break;
    default:
        pid = (int)TD_DP(td[0]);
        break;
    }
    const uint32_t len = td_len(td[1], td[3]);
    static uint8_t buf[8192];
    if (len > sizeof(buf)) {
        ed[2] |= ED_H;
        return false;
    }
    if (pid != PID_IN && len) {
        td_copy(o, td[1], td[3], buf, len, false);
    }

    NcUsbDev *d = find_device(o, (uint8_t)ED_FA(ed[0]));
    const int ep = (int)ED_EN(ed[0]);
    int r;
    if (!d) {
        r = -100;
    } else if (ep == 0) {
        r = pid == PID_SETUP ? (len == 8 ? nc_usb_setup(d, buf) : NC_USB_STALL)
          : pid == PID_IN    ? nc_usb_ep0_in(d, buf, (int)len)
                             : nc_usb_ep0_out(d, buf, (int)len);
    } else {
        r = d->data ? d->data(d, pid == PID_IN, ep, buf, (int)len) : NC_USB_STALL;
    }
    if (r == NC_USB_NAK) {
        return false;
    }
    o->tds++;
    if (g_trace) {
        static const char *const k_pid[] = {"SETUP", "OUT", "IN", "?"};
        char hex[3 * 8 + 1] = "";
        const int show = r > 0 ? MIN(r, 8) : 0;
        for (int i = 0; i < show; i++) {
            snprintf(hex + 3 * i, 4, " %02x", buf[i]);
        }
        nc_log("hakuX-native-usb", "td %08x addr %u ep %d %s len %u -> %d%s (mps %u%s)", td_pa,
               (unsigned)ED_FA(ed[0]), ep, k_pid[pid & 3], len, r, hex, (ed[0] >> 16) & 0x7ff,
               td[0] & TD_R ? ", rounding" : "");
    }

    /* Whatever moved, CBP moves past it: all of it clears CBP; a short
     * packet leaves CBP at the first byte that didn't arrive (the host
     * driver counts a short transfer from it, error or not). */
    if (r >= 0) {
        if (pid == PID_IN && r > 0) {
            td_copy(o, td[1], td[3], buf, (uint32_t)r, true);
        }
        if ((uint32_t)r == len) {
            td[1] = 0;
        } else {
            const uint32_t first = ((td[1] ^ td[3]) & ~0xfffu) ? 0x1000u - (td[1] & 0xfffu) : len;
            td[1] = (uint32_t)r < first ? td[1] + (uint32_t)r : (td[3] & ~0xfffu) + ((uint32_t)r - first);
        }
    }
    uint32_t cc;
    if (r >= 0 && ((uint32_t)r == len || (pid == PID_IN && (td[0] & TD_R)))) {
        td[0] |= TD_T1;
        td[0] ^= TD_T0;
        cc = CC_NOERROR;
        ed[2] &= ~ED_C;
        if (td[0] & TD_T0) {
            ed[2] |= ED_C;
        }
    } else {
        if (r >= 0) {
            cc = CC_DATAUNDERRUN;
        } else if (r == NC_USB_STALL) {
            cc = CC_STALL;
        } else {
            cc = CC_DEVICENOTRESPONDING;
        }
        ed[2] |= ED_H;
    }
    td[0] = (td[0] & ~(TD_EC_MASK | (0xfu << TD_CC_SHIFT))) | (cc << TD_CC_SHIFT);

    /* Retire: off the ED, onto the done queue. */
    ed[2] = (td[2] & PTR_MASK) | (ed[2] & (ED_H | ED_C));
    td[2] = o->done;
    o->done = td_pa;
    const int di = (int)TD_DI(td[0]);
    if (di < o->done_count) {
        o->done_count = di;
    }
    if (cc != CC_NOERROR) {
        o->done_count = 0; /* errors are reported at once */
    }
    wr_words(o, td_pa, td, 4);
    wr_words(o, ed_pa + 8, &ed[2], 1);
    return !(ed[2] & ED_H);
}

/* A packet of an isochronous TD: its first byte from Offset[n] (the page of
 * BufferPage0 or, bit 12 set, of BufferEnd); it ends before the next one's
 * first byte, the last at BufferEnd. */
static uint32_t itd_addr(const uint32_t td[8], unsigned n)
{
    const uint16_t off = ((const uint16_t *)&td[4])[n];
    return ((off & 0x1000) ? (td[3] & ~0xfffu) : (td[1] & ~0xfffu)) | (off & 0xfffu);
}

/* Service the head isochronous TD of an ED: the packet of this frame, if the
 * TD has one (OpenHCI 4.3.2; as QEMU's OHCI, which the Communicator's driver
 * runs on in xemu). One packet per frame; returns false (the ED is done for
 * this frame). */
static bool service_itd(Ohci *o, uint32_t ed_pa, uint32_t ed[4])
{
    const uint32_t td_pa = ed[2] & ~0x1fu & PTR_MASK;
    uint32_t td[8];
    if (!rd_words(o, td_pa, td, 8)) {
        ed[2] |= ED_H;
        wr_words(o, ed_pa + 8, &ed[2], 1);
        return false;
    }
    const int16_t rel = (int16_t)(o->frame_number - ITD_SF(td[0]));
    const unsigned fc = ITD_FC(td[0]);
    if (rel < 0) {
        return false;                   /* its first packet is for a later frame */
    }
    uint32_t cc = CC_NOERROR;
    if ((unsigned)rel > fc) {
        cc = CC_DATAOVERRUN;            /* its frames went by: retired as it is */
    } else {
        const uint32_t first = itd_addr(td, (unsigned)rel);
        const uint32_t last = (unsigned)rel < fc ? itd_addr(td, (unsigned)rel + 1) - 1 : td[3];
        const uint32_t len = last + 1 == first ? 0 : td_len(first, last);
        static uint8_t buf[1024];
        const bool in = ED_D(ed[0]) == 2;
        NcUsbDev *d = find_device(o, (uint8_t)ED_FA(ed[0]));
        uint16_t psw = (uint16_t)(CC_DEVICENOTRESPONDING << PSW_CC_SHIFT);
        if (d && d->data && len <= sizeof(buf)) {
            if (in) {
                int r = d->data(d, true, (int)ED_EN(ed[0]), buf, (int)len);
                r = r < 0 ? 0 : MIN(r, (int)len); /* nothing to send: an empty packet */
                if (r) {
                    td_copy(o, first, last, buf, (uint32_t)r, true);
                }
                psw = (uint16_t)((CC_NOERROR << PSW_CC_SHIFT) | (uint32_t)r);
            } else {
                if (len) {
                    td_copy(o, first, last, buf, len, false);
                }
                d->data(d, false, (int)ED_EN(ed[0]), buf, (int)len);
                psw = (uint16_t)(CC_NOERROR << PSW_CC_SHIFT);
            }
        }
        ((uint16_t *)&td[4])[rel] = psw;
        o->tds++;
        if ((unsigned)rel < fc) {
            wr_words(o, td_pa + 16, &td[4], 4);
            return false;
        }
    }

    /* Retire: off the ED, onto the done queue. */
    td[0] = (td[0] & ~(0xfu << TD_CC_SHIFT)) | (cc << TD_CC_SHIFT);
    ed[2] = (td[2] & PTR_MASK) | (ed[2] & (ED_H | ED_C));
    td[2] = o->done;
    o->done = td_pa;
    const int di = (int)TD_DI(td[0]);
    if (di < o->done_count) {
        o->done_count = di;
    }
    wr_words(o, td_pa, td, 8);
    wr_words(o, ed_pa + 8, &ed[2], 1);
    return false;
}

/* Walk an ED list; returns whether any ED had TDs queued. */
static bool service_list(Ohci *o, uint32_t head, bool periodic)
{
    bool active = false;
    unsigned guard = 0;
    for (uint32_t pa = head & PTR_MASK; pa && guard < 256; guard++) {
        uint32_t ed[4];
        if (!rd_words(o, pa, ed, 4)) {
            break;
        }
        if (periodic && (ed[0] & (ED_K | ED_F)) == ED_F) {
            /* isochronous (the Communicator's audio): this frame's packet */
            if (!(ed[2] & ED_H) && (ed[2] & PTR_MASK) != (ed[1] & PTR_MASK)) {
                active = true;
                service_itd(o, pa, ed);
            }
        } else if (!(ed[0] & (ED_K | ED_F))) {
            unsigned tds = 0;
            while (!(ed[2] & ED_H) && (ed[2] & PTR_MASK) != (ed[1] & PTR_MASK) && tds < 64) {
                active = true;
                tds++;
                if (!service_td(o, pa, ed)) {
                    break;
                }
                if (periodic) {
                    break; /* one packet per interrupt endpoint per frame */
                }
            }
        }
        pa = ed[3] & PTR_MASK;
    }
    return active;
}

static void frame(Ohci *o)
{
    o->frames++;
    if (o->frames % 10000 == 0) {
        /* the stack's health every 10 s: a title whose USB stops polling
         * (a dead controller, a stalled endpoint) reads no input */
        static uint64_t s_tds;
        nc_log("hakuX-native-usb", "ohci: %llu TDs in 10 s, ctl %08x (lists %s%s%s), hcca %08x, intr %08x/%08x",
               (unsigned long long)(o->tds - s_tds), o->ctl, o->ctl & CTL_PLE ? "P" : "-",
               o->ctl & CTL_CLE ? "C" : "-", o->ctl & CTL_BLE ? "B" : "-", o->hcca, o->intr_status, o->intr);
        s_tds = o->tds;
    }
    o->frame_number = (uint16_t)(o->frame_number + 1);
    uint8_t *hcca = phys(o, o->hcca, 256);
    if (hcca) {
        nc_wr16(hcca + 0x80, o->frame_number);
        nc_wr16(hcca + 0x82, 0);
    }
    if (!(o->frame_number & 0x7fff)) {
        o->intr_status |= INT_FNO;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    if ((o->ctl & CTL_PLE) && hcca) {
        service_list(o, nc_rd32(hcca + 4 * (o->frame_number & 31)), true);
    }
    if ((o->ctl & CTL_CLE) && (o->status & CS_CLF)) {
        if (!service_list(o, o->ctrl_head, false)) {
            o->ctrl_cur = 0;
            o->status &= ~CS_CLF;
        }
    }
    if ((o->ctl & CTL_BLE) && (o->status & CS_BLF)) {
        if (!service_list(o, o->bulk_head, false)) {
            o->bulk_cur = 0;
            o->status &= ~CS_BLF;
        }
    }

    /* The done queue goes out when its delay runs down and the last one was taken. */
    if (o->done && o->done_count == 0 && !(o->intr_status & INT_WDH) && hcca) {
        uint32_t head = o->done;
        if (o->intr_status & o->intr & ~INT_WDH) {
            head |= 1;
        }
        nc_wr32(hcca + 0x84, head);
        o->done = 0;
        o->done_count = 7;
        o->intr_status |= INT_WDH;
    }
    if (o->done_count != 7 && o->done_count != 0) {
        o->done_count--;
    }
    o->intr_status |= INT_SF;
    update_irq(o);
}

static void *frame_thread(void *arg)
{
    Ohci *o = arg;
    nc_pause_register("usb");
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    pthread_mutex_lock(&o->lock);
    for (;;) {
        while ((o->ctl & CTL_HCFS) != HCFS_OPERATIONAL) {
            nc_pause_idle(true);            /* nothing to do until the guest starts the controller */
            pthread_cond_wait(&o->run, &o->lock);
            nc_pause_idle(false);
            clock_gettime(CLOCK_MONOTONIC, &next);
        }
        frame(o);
        pthread_mutex_unlock(&o->lock);
        if (nc_paused()) {
            nc_pause_wait();                /* the emulation paused: no frames */
            clock_gettime(CLOCK_MONOTONIC, &next);
        }
        next.tv_nsec += 1000000;
        if (next.tv_nsec >= 1000000000) {
            next.tv_nsec -= 1000000000;
            next.tv_sec++;
        }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) != 0) {
        }
        pthread_mutex_lock(&o->lock);
    }
    return NULL;
}

/* ------------------------------------------------------------ registers */

static uint64_t ohci_read(void *opaque, uint32_t off, unsigned size)
{
    Ohci *o = opaque;
    uint32_t v = 0;
    pthread_mutex_lock(&o->lock);
    switch (off & ~3u) {
    case 0x00: v = 0x10; break;
    case 0x04: v = o->ctl; break;
    case 0x08: v = o->status; break;
    case 0x0C: v = o->intr_status; break;
    case 0x10:
    case 0x14: v = o->intr; break;
    case 0x18: v = o->hcca; break;
    case 0x1C: v = o->per_cur; break;
    case 0x20: v = o->ctrl_head; break;
    case 0x24: v = o->ctrl_cur; break;
    case 0x28: v = o->bulk_head; break;
    case 0x2C: v = o->bulk_cur; break;
    case 0x30: v = o->done; break;
    case 0x34: v = o->fit << 31 | o->fsmps << 16 | o->fi; break;
    case 0x38: {
        /* Time left in the current 1 ms frame, in bit times. */
        const uint64_t into = (now_ns() - o->frame_t0_ns) % 1000000u;
        v = (uint32_t)((o->fi * (1000000u - into)) / 1000000u) & 0x3fff;
        break;
    }
    case 0x3C: v = o->frame_number; break;
    case 0x40: v = o->pstart; break;
    case 0x44: v = o->lst; break;
    case 0x48: v = o->rhdesc_a; break;
    case 0x4C: v = o->rhdesc_b; break;
    case 0x50: v = o->rhstatus; break;
    default:
        if (off >= 0x54 && off < 0x54 + 4 * ROOT_PORTS) {
            v = o->port[(off - 0x54) / 4];
        }
        break;
    }
    pthread_mutex_unlock(&o->lock);
    return v >> (8 * (off & 3));
}

static void ohci_write(void *opaque, uint32_t off, uint64_t value, unsigned size)
{
    Ohci *o = opaque;
    const uint32_t v = (uint32_t)value;
    pthread_mutex_lock(&o->lock);
    switch (off) {
    case 0x04: {
        const uint32_t old = o->ctl & CTL_HCFS;
        o->ctl = v;
        if ((v & CTL_HCFS) != old) {
            nc_log("hakuX-native-usb", "ohci: state %u -> %u", old >> 6, (v & CTL_HCFS) >> 6);
            if ((v & CTL_HCFS) == HCFS_OPERATIONAL) {
                o->frame_t0_ns = now_ns();
                pthread_cond_signal(&o->run);
            }
        }
        break;
    }
    case 0x08:
        if (v & CS_HCR) {
            soft_reset(o);
        }
        o->status |= v & (CS_CLF | CS_BLF | CS_OCR);
        break;
    case 0x0C:
        o->intr_status &= ~v;
        update_irq(o);
        break;
    case 0x10:
        o->intr |= v;
        update_irq(o);
        break;
    case 0x14:
        o->intr &= ~v;
        update_irq(o);
        break;
    case 0x18: o->hcca = v & 0xffffff00u; break;
    case 0x20: o->ctrl_head = v & PTR_MASK; break;
    case 0x24: o->ctrl_cur = v & PTR_MASK; break;
    case 0x28: o->bulk_head = v & PTR_MASK; break;
    case 0x2C: o->bulk_cur = v & PTR_MASK; break;
    case 0x34:
        o->fi = v & 0x3fff;
        o->fsmps = (v >> 16) & 0x7fff;
        o->fit = v >> 31;
        break;
    case 0x40: o->pstart = v & 0x3fff; break;
    case 0x44: o->lst = v & 0xfff; break;
    case 0x48: o->rhdesc_a = (o->rhdesc_a & ~RHA_RW_MASK) | (v & RHA_RW_MASK); break;
    case 0x4C: o->rhdesc_b = v; break;
    case 0x50:
        if (v & RHS_OCIC) {
            o->rhstatus &= ~RHS_OCIC;
        }
        if (v & RHS_DRWE) {
            o->rhstatus |= RHS_DRWE;
        }
        if (v & RHS_CRWE) {
            o->rhstatus &= ~RHS_DRWE;
        }
        break;
    default:
        if (off >= 0x54 && off < 0x54 + 4 * ROOT_PORTS) {
            port_write(o, (off - 0x54) / 4, v);
        }
        break;
    }
    pthread_mutex_unlock(&o->lock);
}

/* NC_PAD_MASK: which of players 1-4 have a controller plugged in at boot (bit n:
 * player n + 1; hex: the app's, later ones nc_dev_ohci_set_pads); else NC_PADS, how many
 * from player 1 (default 1). */
void nc_dev_ohci_init(struct NcVm *vm)
{
    Ohci *o = &g_ohci;
    pthread_mutex_lock(&o->lock);
    o->vm = vm;

    g_trace = getenv("NC_TRACE_USB") != NULL;
    const char *m = getenv("NC_PAD_MASK");
    const char *e = getenv("NC_PADS");
    unsigned mask = m ? (unsigned)strtoul(m, NULL, 16) & 0xF : 0;
    if (!m) {
        const unsigned n = e ? (unsigned)atoi(e) : 1;
        mask = n >= 4 ? 0xF : (1u << n) - 1;
    }
    if (g_early_mask >= 0) {
        mask = (unsigned)g_early_mask;      /* the controllers changed while the core started */
    }
    unsigned players = 0;
    for (unsigned p = 0; p < 4; p++) {
        players += (mask >> p) & 1;
    }
    /* Controllers straight on the root ports (players 1-4 on ports 3, 4, 1,
     * 2), as the later consoles wire them. NC_USB_FRONT_HUB=1 puts the 1.0
     * console's front panel hub in between, which the titles' USB stack
     * doesn't enumerate past here yet. */
    const char *fh = getenv("NC_USB_FRONT_HUB");
    const bool front_hub = fh && atoi(fh) != 0;
    g_front_hub = front_hub;
    NcUsbDev *front = front_hub && players ? nc_usb_hub_new(4) : NULL;
    /* NC_VOICE_MASK (the app's voice=): the players with an Xbox Live
     * Communicator in their controller's expansion slot A (hub port 2, as
     * xemu's), for voice chat. */
    const char *vmask = getenv("NC_VOICE_MASK");
    const unsigned voice = vmask ? (unsigned)strtoul(vmask, NULL, 16) & 0xF : 0;
    for (unsigned p = 0; p < 4; p++) {
        g_pad_hub[p] = nc_usb_hub_new(3);
        nc_usb_hub_attach(g_pad_hub[p], 1, nc_xid_gamepad_new(p));
        if ((voice >> p) & 1) {
            nc_usb_hub_attach(g_pad_hub[p], 2, nc_xblc_new(p));
            nc_log("hakuX-native-usb", "ohci: player %u's controller has a Communicator", p + 1);
        }
        if (!((mask >> p) & 1)) {
            continue;
        }
        if (front) {
            nc_usb_hub_attach(front, k_port_map[p], g_pad_hub[p]);
        } else {
            o->dev[k_port_map[p] - 1] = g_pad_hub[p];
        }
    }
    if (front) {
        o->dev[0] = front;
    }
    soft_reset(o);
    o->ctl = 0;
    roothub_reset(o);
    g_ready = true;
    pthread_mutex_unlock(&o->lock);

    static NcMmioDev dev = {
        .name = "ohci0",
        .base = OHCI_BASE,
        .size = OHCI_SIZE,
        .read = ohci_read,
        .write = ohci_write,
        .opaque = &g_ohci,
    };
    nc_mmio_register(&dev);

    pthread_t t;
    pthread_create(&t, NULL, frame_thread, o);
    pthread_detach(t);
    nc_log("hakuX-native-usb", "ohci: %u controller(s) (players %s%s%s%s)%s", players, mask & 1 ? "1" : "",
           mask & 2 ? "2" : "", mask & 4 ? "3" : "", mask & 8 ? "4" : "",
           front_hub ? " behind the front panel hub" : "");
}

/* A controller plugged in or pulled out, as on the console: its root port
 * shows the connection change and the title's USB stack finds the gamepad,
 * or sees it go. Only the controllers the players have are plugged in; one
 * nobody holds was found like any other, and a title that listens to the
 * first controller it finds (AirForce Delta Storm, XDK 3911: the lowest
 * port ready as it boots, latched) took player 3's, an empty one on root
 * port 1, which enumerates first. Not behind NC_USB_FRONT_HUB's hub. */
void nc_dev_ohci_set_pads(unsigned mask)
{
    Ohci *o = &g_ohci;
    pthread_mutex_lock(&o->lock);
    if (!g_ready) {
        g_early_mask = (int)(mask & 0xF);   /* before the boot plugged any in: these */
        pthread_mutex_unlock(&o->lock);
        return;
    }
    if (g_front_hub) {
        pthread_mutex_unlock(&o->lock);
        return;
    }
    unsigned changed = 0;
    for (unsigned p = 0; p < 4; p++) {
        const unsigned i = k_port_map[p] - 1;
        const bool want = (mask >> p) & 1, have = o->dev[i] != NULL;
        if (want == have) {
            continue;
        }
        if (want) {
            o->dev[i] = g_pad_hub[p];
            nc_usb_reset(o->dev[i]);
            o->port[i] = (o->port[i] & PORT_PPS) | PORT_CCS | PORT_CSC;
        } else {
            o->port[i] = (o->port[i] & PORT_PPS) | PORT_CSC;
            o->dev[i] = NULL;
        }
        changed |= 1u << p;
    }
    if (changed) {
        set_interrupt(o, INT_RHSC);
        nc_log("hakuX-native-usb", "ohci: controllers plugged in for players %s%s%s%s",
               mask & 1 ? "1" : "", mask & 2 ? "2" : "", mask & 4 ? "3" : "", mask & 8 ? "4" : "");
    }
    pthread_mutex_unlock(&o->lock);
}

/* ---------------------------------------------------------- savestates */

#define NC_DEF_SECTION USB
#define NC_DEF_FILE "native/state/usb.def"
#include "native/state/def.h"
#include "native/state/sections.h"

bool nc_usb_state_save(NcBuf *b)
{
    Ohci *o = &g_ohci;
    pthread_mutex_lock(&o->lock);
    nc_rec_put(b, &nc_rec_Ohci, o);
    for (unsigned i = 0; i < ROOT_PORTS; i++) {
        NcUsbDev *hub = o->dev[i];
        nc_buf_u8(b, hub != NULL);
        if (!hub) {
            continue;
        }
        nc_rec_put(b, &nc_rec_NcUsbDev, hub);
        NcUsbHubState hs;
        nc_usb_hub_state(hub, &hs);
        nc_rec_put(b, &nc_rec_NcUsbHubState, &hs);
        for (unsigned p = 1; p <= hs.nports; p++) {
            NcUsbDev *c = nc_usb_hub_child(hub, p);
            nc_buf_u8(b, c != NULL);
            if (c) {
                nc_rec_put(b, &nc_rec_NcUsbDev, c);
            }
        }
    }
    pthread_mutex_unlock(&o->lock);
    return true;
}

bool nc_usb_state_load(NcRd *r, unsigned v)
{
    Ohci *o = &g_ohci;
    pthread_mutex_lock(&o->lock);
    uint32_t fresh_port[ROOT_PORTS];
    memcpy(fresh_port, o->port, sizeof(fresh_port));
    nc_rec_get(r, &nc_rec_Ohci, o, v);
    unsigned changes = 0;
    for (unsigned i = 0; i < ROOT_PORTS && !r->fail; i++) {
        NcUsbDev *hub = o->dev[i];
        if (!nc_rd_u8(r)) {
            if (hub) {
                /* plugged in since: a connection for the guest to see */
                o->port[i] = fresh_port[i] | PORT_CCS | PORT_CSC | (hub->low_speed ? PORT_LSDA : 0);
                changes++;
            }
            continue;
        }
        /* the state's devices are read into the ones plugged in now: their
         * stored fields only (descriptors and callbacks stay) */
        NcUsbDev scratch;
        NcUsbHubState hs = {0};
        nc_rec_get(r, &nc_rec_NcUsbDev, hub ? hub : &scratch, v);
        nc_rec_get(r, &nc_rec_NcUsbHubState, &hs, v);
        if (hs.nports > 4) {
            pthread_mutex_unlock(&o->lock);
            return false;
        }
        if (hub) {
            nc_usb_hub_state_set(hub, &hs);
        } else {
            /* unplugged since: the guest sees it go */
            o->port[i] = (o->port[i] & ~(PORT_CCS | PORT_PES | PORT_LSDA)) | PORT_CSC;
            changes++;
        }
        for (unsigned p = 1; p <= hs.nports; p++) {
            NcUsbDev *c = hub ? nc_usb_hub_child(hub, p) : NULL;
            const bool had = nc_rd_u8(r);
            if (had) {
                nc_rec_get(r, &nc_rec_NcUsbDev, c ? c : &scratch, v);
            }
            if (hub && had != (c != NULL)) {
                nc_usb_hub_port_changed(hub, p);    /* plugged in or out since */
                changes++;
            }
        }
    }
    if (changes) {
        set_interrupt(o, INT_RHSC);
        nc_log("hakuX-native-state", "usb: %u port(s) changed since the state (controllers plugged in or out)",
               changes);
    }
    o->irq = !((o->intr & INT_MIE) && (o->intr_status & o->intr & ~INT_MIE));
    update_irq(o);                          /* the line as the registers say, set either way */
    o->frame_t0_ns = now_ns();
    pthread_cond_broadcast(&o->run);
    pthread_mutex_unlock(&o->lock);
    return !r->fail;
}
