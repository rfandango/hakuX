/*
 * hakuX native core — the MCPX's nForce Ethernet controller ("nvnet") at
 * 0xFEF00000, IRQ 4, with its PHY at MII address 1.
 *
 * The title's network stack (XNet, linked into it) drives the controller:
 * descriptor rings in guest memory (8 bytes each: buffer, length - 1,
 * flags), TX started by a KICK write, RX buffers handed over by setting
 * NV_RX_AVAIL. Sent frames go to the network (native/net), received ones are
 * written into the next free RX buffer by the device's thread, which also
 * completes the PHY's auto-negotiation.
 *
 * The register model is xemu's (hw/xbox/mcpx/nvnet/nvnet.c, LGPL-2.1+):
 *   Copyright (c) 2013 espes
 *   Copyright (c) 2015-2025 Matt Borgerson
 * its definitions after forcedeth.c (cromwell, GPL-2+).
 */
#include "native/dev/dev.h"
#include "native/core/irq.h"
#include "native/core/mmio.h"
#include "native/core/nc.h"
#include "native/core/vm.h"
#include "native/net/net.h"
#include "hw/xbox/hle/host.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NVNET_BASE 0xFEF00000u
#define NVNET_SIZE 0x400u
#define NVNET_IRQ  4
#define PHY_ADDR   1
#define AUTONEG_NS 250000000ull

/* registers (xemu's nvnet_regs.h) */
#define IRQ_STATUS              0x000
#define  IRQ_STATUS_RX            0x00000002
#define  IRQ_STATUS_TX            0x00000010
#define  IRQ_STATUS_MIIEVENT      0x00000040
#define IRQ_MASK                0x004
#define TRANSMITTER_CONTROL     0x084
#define  TRANSMITTER_START        0x00000001
#define PACKET_FILTER           0x08C
#define  PACKET_FILTER_MYADDR     0x00000020
#define RECEIVER_CONTROL        0x094
#define  RECEIVER_START           0x00000001
#define MAC_ADDR_A              0x0A8
#define MAC_ADDR_B              0x0AC
#define MULTICAST_ADDR_A        0x0B0
#define MULTICAST_ADDR_B        0x0B4
#define MULTICAST_MASK_A        0x0B8
#define MULTICAST_MASK_B        0x0BC
#define TX_RING_PHYS_ADDR       0x100
#define RX_RING_PHYS_ADDR       0x104
#define RING_SIZE               0x108
#define TX_RING_CUR_DESC        0x11C
#define RX_RING_CUR_DESC        0x120
#define UNKNOWN_SETUP_REG5      0x130
#define  SETUP_REG5_BIT31         0x80000000u
#define TX_RING_NEXT_DESC       0x134
#define RX_RING_NEXT_DESC       0x138
#define TX_RX_CONTROL           0x144
#define  TXRX_KICK                0x00000001
#define  TXRX_BIT1                0x00000002
#define  TXRX_BIT2                0x00000004
#define  TXRX_IDLE                0x00000008
#define  TXRX_RESET               0x00000010
#define MII_STATUS              0x180
#define  MII_STATUS_LINKCHANGE    0x00000008
#define ADAPTER_CONTROL         0x188
#define  ADAPTER_CONTROL_LINKUP   0x00000004
#define MDIO_ADDR               0x190
#define  MDIO_ADDR_INUSE          0x00008000
#define  MDIO_ADDR_WRITE          0x00000400
#define MDIO_DATA               0x194

#define NV_TX_LASTPACKET      (1u << 0)
#define NV_TX_RETRYERROR      (1u << 3)
#define NV_TX_DEFERRED        (1u << 10)
#define NV_TX_CARRIERLOST     (1u << 11)
#define NV_TX_LATECOLLISION   (1u << 12)
#define NV_TX_UNDERFLOW       (1u << 13)
#define NV_TX_ERROR           (1u << 14)
#define NV_TX_VALID           (1u << 15)
#define NV_RX_DESCRIPTORVALID (1u << 0)
#define NV_RX_BIT4            (1u << 4)
#define NV_RX_AVAIL           (1u << 15)

#define TX_BUF 1628                     /* DEFAULT_MTU + 128 */

/* the PHY (MII) */
enum { MII_BMCR, MII_BMSR, MII_PHYID1, MII_PHYID2, MII_ANAR, MII_ANLPAR, PHY_REGS };
#define BMCR_RESET     0x8000
#define BMCR_AUTOEN    0x1000
#define BMCR_ANRESTART 0x0200
#define BMCR_FD        0x0100
#define BMSR_AN_COMP   0x0020
#define BMSR_AUTONEG   0x0008
#define BMSR_LINK_ST   0x0004
#define ANLPAR_ACK     0x4000
#define ANLPAR_ABILITY 0x03E0           /* T4, TX FD, TX, 10 FD, 10 */

typedef struct Nvnet {
    NcVm *vm;
    pthread_mutex_t lock;
    pthread_cond_t kick;                /* the guest acted: RX buffers may be back */
    uint8_t regs[NVNET_SIZE];
    uint32_t phy[PHY_REGS];
    uint32_t tx_off;                    /* bytes of a frame gathered over descriptors */
    uint8_t tx_buf[TX_BUF];
    uint64_t autoneg_at;                /* host monotonic ns; 0: none running */
    bool irq;
    uint64_t tx_frames, rx_frames, rx_dropped;
} Nvnet;

static Nvnet g_nv;
/* Outside the saved device state: joined before the guest memory is freed. */
static struct {
    pthread_t thread;
    bool initialized, started, quit;
} g_nv_thr;
static int g_trace;                     /* NC_TRACE_NVNET: register writes, frames (first 400) */

#define TRACE(...)                                     \
    do {                                               \
        if (g_trace > 0) {                             \
            g_trace--;                                 \
            nc_log("hakuX-native-nvnet", __VA_ARGS__); \
        }                                              \
    } while (0)

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t reg(const Nvnet *s, uint32_t off)
{
    return nc_rd32(s->regs + off);
}

static void set_reg(Nvnet *s, uint32_t off, uint32_t v)
{
    nc_wr32(s->regs + off, v);
}

static void or_reg(Nvnet *s, uint32_t off, uint32_t v)
{
    set_reg(s, off, reg(s, off) | v);
}

static void and_reg(Nvnet *s, uint32_t off, uint32_t v)
{
    set_reg(s, off, reg(s, off) & v);
}

static void update_irq(Nvnet *s)
{
    const bool level = (reg(s, IRQ_STATUS) & reg(s, IRQ_MASK)) != 0;
    if (level != s->irq) {
        TRACE("irq %d (status %08x mask %08x)", level, reg(s, IRQ_STATUS), reg(s, IRQ_MASK));
        s->irq = level;
        nc_irq_set(NVNET_IRQ, level);
    }
}

static void set_intr(Nvnet *s, uint32_t status)
{
    or_reg(s, IRQ_STATUS, status);
    update_irq(s);
}

/* ------------------------------------------------------------- the PHY */

static bool link_up(const Nvnet *s)
{
    return (s->phy[MII_BMSR] & BMSR_LINK_ST) != 0;
}

static void link_change(Nvnet *s)
{
    or_reg(s, MII_STATUS, MII_STATUS_LINKCHANGE);
    set_intr(s, IRQ_STATUS_MIIEVENT);
}

/* No link (networking off): auto-negotiation reads as done, without a
 * link, so nothing waits for it (xemu: the kernel's PhyInitialize waits for
 * AN complete before it looks at the link). */
static void regs_link_down(Nvnet *s)
{
    s->phy[MII_BMSR] = (s->phy[MII_BMSR] & ~BMSR_LINK_ST) | BMSR_AN_COMP;
    s->phy[MII_ANLPAR] &= ~ANLPAR_ACK;
    and_reg(s, ADAPTER_CONTROL, ~ADAPTER_CONTROL_LINKUP);
}

static void regs_link_up(Nvnet *s)
{
    s->phy[MII_BMSR] |= BMSR_LINK_ST;
    or_reg(s, ADAPTER_CONTROL, ADAPTER_CONTROL_LINKUP);
}

static void phy_reset(Nvnet *s)
{
    memset(s->phy, 0, sizeof(s->phy));
    s->phy[MII_BMCR] = BMCR_FD | BMCR_AUTOEN;
    s->phy[MII_BMSR] = BMSR_AUTONEG | BMSR_AN_COMP | BMSR_LINK_ST;
    s->phy[MII_ANAR] = ANLPAR_ABILITY;
    s->phy[MII_ANLPAR] = ANLPAR_ABILITY;
}

static void autoneg_restart(Nvnet *s)
{
    s->phy[MII_BMSR] &= ~BMSR_AN_COMP;
    s->autoneg_at = now_ns() + AUTONEG_NS;
    pthread_cond_signal(&s->kick);
}

static void autoneg_done(Nvnet *s)
{
    s->autoneg_at = 0;
    s->phy[MII_ANLPAR] |= ANLPAR_ACK;
    s->phy[MII_BMSR] |= BMSR_AN_COMP;
    regs_link_up(s);
    link_change(s);
    TRACE("link up (auto-negotiation done)");
}

static void phy_write(Nvnet *s, unsigned r, uint32_t v)
{
    if (r == MII_BMCR && (v & (BMCR_RESET | BMCR_ANRESTART))) {
        /* both self-clear; a reset restores the defaults */
        if (v & BMCR_RESET) {
            phy_reset(s);
        } else {
            s->phy[MII_BMCR] = v & ~BMCR_ANRESTART;
        }
        if (!nc_net_link()) {
            regs_link_down(s);
        } else if (s->phy[MII_BMCR] & BMCR_AUTOEN) {
            autoneg_restart(s);
        }
        return;
    }
    if (r < PHY_REGS) {
        s->phy[r] = v;
    }
}

static void mdio(Nvnet *s, uint32_t v)
{
    const unsigned addr = (v >> 5) & 0x1F, r = v & 0x1F;
    if (v & MDIO_ADDR_WRITE) {
        if (addr == PHY_ADDR) {
            phy_write(s, r, reg(s, MDIO_DATA) & 0xFFFF);
        }
    } else {
        set_reg(s, MDIO_DATA, addr == PHY_ADDR ? (r < PHY_REGS ? s->phy[r] : 0) : 0xFFFFFFFFu);
    }
    and_reg(s, MDIO_ADDR, ~MDIO_ADDR_INUSE);
}

/* ------------------------------------------------------------ the rings */

typedef struct Desc {
    uint32_t buf;
    uint16_t len, flags;
} Desc;

static bool desc_load(const Nvnet *s, uint32_t pa, Desc *d)
{
    if ((uint64_t)pa + 8 > s->vm->ram_size) {
        return false;
    }
    const uint8_t *p = s->vm->phys + pa;
    d->buf = nc_rd32(p);
    d->len = nc_rd16(p + 4);
    d->flags = nc_rd16(p + 6);
    return true;
}

static void desc_store(Nvnet *s, uint32_t pa, const Desc *d)
{
    if ((uint64_t)pa + 8 > s->vm->ram_size) {
        return;
    }
    uint8_t *p = s->vm->phys + pa;
    nc_wr32(p, d->buf);
    nc_wr16(p + 4, d->len);
    nc_wr16(p + 6, d->flags);
    hle_host_mem_set_dirty(pa, 8);
}

static unsigned ring_size(const Nvnet *s, bool rx)
{
    const uint32_t v = reg(s, RING_SIZE);
    return (rx ? v >> 16 : v & 0xFFFF) + 1;
}

/* The descriptor the ring's next pointer names (back at the base when it
 * is outside the ring), made current. */
static uint32_t cur_desc(Nvnet *s, bool rx)
{
    const uint32_t base = reg(s, rx ? RX_RING_PHYS_ADDR : TX_RING_PHYS_ADDR);
    const uint32_t end = base + ring_size(s, rx) * 8u;
    uint32_t cur = reg(s, rx ? RX_RING_NEXT_DESC : TX_RING_NEXT_DESC);
    if (cur < base || cur + 8 > end) {
        cur = base;
    }
    set_reg(s, rx ? RX_RING_CUR_DESC : TX_RING_CUR_DESC, cur);
    return cur;
}

static void advance(Nvnet *s, bool rx)
{
    const uint32_t base = reg(s, rx ? RX_RING_PHYS_ADDR : TX_RING_PHYS_ADDR);
    const uint32_t end = base + ring_size(s, rx) * 8u;
    uint32_t next = reg(s, rx ? RX_RING_CUR_DESC : TX_RING_CUR_DESC) + 8;
    set_reg(s, rx ? RX_RING_NEXT_DESC : TX_RING_NEXT_DESC, next >= end ? base : next);
}

static void reset_rings(Nvnet *s)
{
    uint32_t b = reg(s, TX_RING_PHYS_ADDR);
    set_reg(s, TX_RING_CUR_DESC, b);
    set_reg(s, TX_RING_NEXT_DESC, b);
    b = reg(s, RX_RING_PHYS_ADDR);
    set_reg(s, RX_RING_CUR_DESC, b);
    set_reg(s, RX_RING_NEXT_DESC, b);
}

static bool dma_on(const Nvnet *s)
{
    return (reg(s, TX_RX_CONTROL) & TXRX_BIT2) == 0;
}

static void set_idle(Nvnet *s, bool idle)
{
    if (idle) {
        or_reg(s, TX_RX_CONTROL, TXRX_IDLE);
    } else {
        and_reg(s, TX_RX_CONTROL, ~TXRX_IDLE);
    }
}

/* KICK: the valid TX descriptors from the current one, each buffer added
 * to the frame, the frame sent at the last one. */
static void transmit(Nvnet *s)
{
    if (!(reg(s, TRANSMITTER_CONTROL) & TRANSMITTER_START) || !dma_on(s) || !link_up(s)) {
        return;
    }
    set_idle(s, false);
    bool sent = false;
    for (unsigned i = 0, n = ring_size(s, false); i < n; i++) {
        const uint32_t pa = cur_desc(s, false);
        Desc d;
        const bool loaded = desc_load(s, pa, &d);
        TRACE("tx desc %08x: buffer %08x length %u flags %04x", pa, loaded ? d.buf : 0, loaded ? d.len + 1u : 0,
              loaded ? d.flags : 0);
        if (!loaded || !(d.flags & NV_TX_VALID)) {
            break;
        }
        const uint32_t len = (uint32_t)d.len + 1;
        if (s->tx_off + len <= TX_BUF && (uint64_t)d.buf + len <= s->vm->ram_size) {
            memcpy(s->tx_buf + s->tx_off, s->vm->phys + d.buf, len);
            s->tx_off += len;
        } else {
            s->tx_off = TX_BUF + 1;         /* too long: dropped at its last buffer */
        }
        const bool last = d.flags & NV_TX_LASTPACKET;
        if (last) {
            if (s->tx_off <= TX_BUF) {
                TRACE("tx %u bytes", s->tx_off);
                nc_net_send(s->tx_buf, s->tx_off);
                s->tx_frames++;
            }
            s->tx_off = 0;
            sent = true;
        }
        d.flags &= ~(NV_TX_VALID | NV_TX_RETRYERROR | NV_TX_DEFERRED | NV_TX_CARRIERLOST |
                     NV_TX_LATECOLLISION | NV_TX_UNDERFLOW | NV_TX_ERROR);
        desc_store(s, pa, &d);
        advance(s, false);
        if (last) {
            break;
        }
    }
    set_idle(s, true);
    if (sent) {
        set_intr(s, IRQ_STATUS_TX);
    }
}

static bool rx_on(const Nvnet *s)
{
    return (reg(s, RECEIVER_CONTROL) & RECEIVER_START) && dma_on(s) && link_up(s);
}

static bool rx_ready(Nvnet *s)
{
    if (!rx_on(s)) {
        return false;
    }
    Desc d;
    return desc_load(s, cur_desc(s, true), &d) && (d.flags & NV_RX_AVAIL);
}

/* The receive filter: broadcast always, everything unless MYADDR filtering
 * is on, then the multicast address under its mask, then our MAC. */
static bool accept(const Nvnet *s, const uint8_t *f, size_t len)
{
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (len < 14) {
        return false;
    }
    if (!memcmp(f, bcast, 6) || !(reg(s, PACKET_FILTER) & PACKET_FILTER_MYADDR)) {
        return true;
    }
    uint8_t a[8], m[8];
    nc_wr32(a, reg(s, MULTICAST_ADDR_A));
    nc_wr32(a + 4, reg(s, MULTICAST_ADDR_B));
    if (memcmp(a, bcast, 6)) {
        nc_wr32(m, reg(s, MULTICAST_MASK_A));
        nc_wr32(m + 4, reg(s, MULTICAST_MASK_B));
        uint8_t d[6];
        for (unsigned i = 0; i < 6; i++) {
            d[i] = f[i] & m[i];
        }
        if (!memcmp(d, a, 6)) {
            return true;
        }
    }
    nc_wr32(a, reg(s, MAC_ADDR_A));
    nc_wr32(a + 4, reg(s, MAC_ADDR_B));
    return !memcmp(f, a, 6);
}

/* The backend learns the station address the guest set (lan: its hellos). */
static void station_address(Nvnet *s)
{
    uint8_t a[8];
    nc_wr32(a, reg(s, MAC_ADDR_A));
    nc_wr32(a + 4, reg(s, MAC_ADDR_B));
    nc_net_set_mac(a);
}

/* The next received frame into the current RX buffer; false when the guest
 * has none free (the frame stays queued). With the receiver off it's lost,
 * as on the wire (a title starts it in its system link menu, other
 * consoles' broadcasts long before). */
static bool receive_one(Nvnet *s)
{
    static uint8_t buf[NC_NET_FRAME_MAX];
    if (!nc_net_peek()) {
        return false;
    }
    if (!rx_on(s)) {
        nc_net_recv(buf, sizeof(buf));
        s->rx_dropped++;
        return true;
    }
    if (!rx_ready(s)) {
        return false;
    }
    size_t len = nc_net_recv(buf, sizeof(buf));
    if (!len || !accept(s, buf, len)) {
        return true;                        /* taken, not for us */
    }
    if (len < 60) {                         /* padded to Ethernet's least, as the wire has it */
        memset(buf + len, 0, 60 - len);
        len = 60;
    }
    set_idle(s, false);
    const uint32_t pa = cur_desc(s, true);
    Desc d;
    desc_load(s, pa, &d);
    if (len > (size_t)d.len + 1 || (uint64_t)d.buf + len > s->vm->ram_size) {
        s->rx_dropped++;                    /* doesn't fit its buffer */
        set_idle(s, true);
        return true;
    }
    memcpy(s->vm->phys + d.buf, buf, len);
    hle_host_mem_set_dirty(d.buf, (uint32_t)len);
    d.len = (uint16_t)len;
    d.flags = NV_RX_BIT4 | NV_RX_DESCRIPTORVALID;
    __atomic_thread_fence(__ATOMIC_RELEASE);  /* the frame before its descriptor says so */
    desc_store(s, pa, &d);
    advance(s, true);
    set_idle(s, true);
    s->rx_frames++;
    TRACE("rx %zu bytes into %08x", len, d.buf);
    set_intr(s, IRQ_STATUS_RX);
    return true;
}

/* -------------------------------------------------------- the registers */

static uint64_t nv_read(void *opaque, uint32_t off, unsigned size)
{
    Nvnet *s = opaque;
    uint64_t v = 0;
    pthread_mutex_lock(&s->lock);
    if (off + size <= NVNET_SIZE) {
        memcpy(&v, s->regs + off, size > 4 ? 4 : size);
    }
    pthread_mutex_unlock(&s->lock);
    return v;
}

static void nv_write(void *opaque, uint32_t off, uint64_t value, unsigned size)
{
    Nvnet *s = opaque;
    const uint32_t v = (uint32_t)value;
    if (off + size > NVNET_SIZE) {
        return;
    }
    pthread_mutex_lock(&s->lock);
    TRACE("write %03x = %08x (%u)", off, v, size);
    switch (off) {
    case MDIO_ADDR:
        set_reg(s, off, v);
        mdio(s, v);
        break;
    case TX_RX_CONTROL:
        set_reg(s, off, (reg(s, off) & TXRX_IDLE) | (v & ~TXRX_IDLE));
        if (v & TXRX_KICK) {
            transmit(s);
        }
        if (v & TXRX_RESET) {
            reset_rings(s);
            s->tx_off = 0;
        }
        if (v & TXRX_BIT1) {
            set_reg(s, IRQ_STATUS, 0);
            update_irq(s);
        } else if (v == 0) {
            set_reg(s, UNKNOWN_SETUP_REG5, SETUP_REG5_BIT31);   /* forcedeth waits for it */
        }
        pthread_cond_signal(&s->kick);      /* RX may be possible now */
        break;
    case IRQ_STATUS:
    case MII_STATUS:
        set_reg(s, off, reg(s, off) & ~v);
        update_irq(s);
        break;
    case IRQ_MASK:
        set_reg(s, off, v);
        update_irq(s);
        break;
    case RECEIVER_CONTROL:
        set_reg(s, off, v);
        pthread_cond_signal(&s->kick);
        break;
    default:
        if (size >= 4) {
            set_reg(s, off, v);
        } else {
            memcpy(s->regs + off, &v, size);
        }
        if (off >= MAC_ADDR_A && off < MAC_ADDR_B + 4) {
            station_address(s);
        }
        break;
    }
    pthread_mutex_unlock(&s->lock);
}

/* ---------------------------------------------------- the device thread */

static void *nvnet_thread(void *arg)
{
    Nvnet *s = arg;
    nc_pause_register("nvnet");
    while (!__atomic_load_n(&g_nv_thr.quit, __ATOMIC_ACQUIRE)) {
        if (nc_paused()) {
            nc_pause_wait();
        }
        pthread_mutex_lock(&s->lock);
        if (s->autoneg_at && now_ns() >= s->autoneg_at) {
            autoneg_done(s);
        }
        bool more = false;
        for (unsigned n = 0; n < 64 && receive_one(s); n++) {
            more = true;
        }
        const bool waiting = nc_net_peek() != 0;   /* frames, no buffer for them */
        const bool negotiating = s->autoneg_at != 0;
        pthread_mutex_unlock(&s->lock);
        if (more) {
            continue;
        }
        /* nothing until a frame comes, the guest gives buffers back, or
         * auto-negotiation ends: idle meanwhile (a savestate needn't wait) */
        nc_pause_idle(true);
        if (waiting || negotiating) {
            struct timespec ts = { 0, 1000000 };    /* 1 ms */
            nanosleep(&ts, NULL);
        } else {
            nc_net_wait_rx(20000000);
        }
        nc_pause_idle(false);
    }
    return NULL;
}

static void nvnet_reset(Nvnet *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    or_reg(s, TX_RX_CONTROL, TXRX_IDLE);
    phy_reset(s);
    s->tx_off = 0;
    s->autoneg_at = 0;
    if (nc_net_link()) {
        regs_link_up(s);
    } else {
        regs_link_down(s);
    }
}

void nc_dev_nvnet_init(struct NcVm *vm)
{
    Nvnet *s = &g_nv;
    s->vm = vm;
    g_trace = getenv("NC_TRACE_NVNET") ? 400 : 0;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->kick, NULL);
    g_nv_thr.initialized = true;
    nvnet_reset(s);
    static NcMmioDev dev = {
        .name = "nvnet",
        .base = NVNET_BASE,
        .size = NVNET_SIZE,
        .read = nv_read,
        .write = nv_write,
        .opaque = &g_nv,
    };
    nc_mmio_register(&dev);
    __atomic_store_n(&g_nv_thr.quit, false, __ATOMIC_RELEASE);
    if (nc_net_link()) {
        g_nv_thr.started = pthread_create(&g_nv_thr.thread, NULL, nvnet_thread, s) == 0;
    }
}

void nc_dev_nvnet_stop(void)
{
    if (g_nv_thr.started) {
        __atomic_store_n(&g_nv_thr.quit, true, __ATOMIC_RELEASE);
        pthread_join(g_nv_thr.thread, NULL);   /* at most its 20 ms RX wait */
        g_nv_thr.started = false;
    }
}

void nc_dev_nvnet_free(void)
{
    if (g_nv_thr.initialized) {
        pthread_mutex_destroy(&g_nv.lock);
        pthread_cond_destroy(&g_nv.kick);
        memset(&g_nv, 0, sizeof(g_nv));
        g_nv_thr.initialized = false;
    }
}

void nc_dev_nvnet_phy_init(void)
{
    Nvnet *s = &g_nv;
    if (!nc_net_link()) {
        return;
    }
    pthread_mutex_lock(&s->lock);
    if (s->phy[MII_BMCR] & BMCR_AUTOEN) {
        autoneg_restart(s);
    } else {
        regs_link_up(s);
        link_change(s);
    }
    pthread_mutex_unlock(&s->lock);
}

uint32_t nc_dev_nvnet_link_state(void)
{
    /* XNET_ETHERNET_LINK_ACTIVE | 100MBPS | FULL_DUPLEX, or no link */
    return nc_net_link() ? 0x01 | 0x02 | 0x08 : 0;
}

/* ---------------------------------------------------------- savestates */

#define NC_DEF_SECTION NIC
#define NC_DEF_FILE "native/state/nic.def"
#include "native/state/def.h"
#include "native/state/sections.h"

bool nc_nic_state_save(NcBuf *b)
{
    Nvnet *s = &g_nv;
    pthread_mutex_lock(&s->lock);
    nc_rec_put(b, &nc_rec_Nvnet, s);
    pthread_mutex_unlock(&s->lock);
    return true;
}

bool nc_nic_state_load(NcRd *r, unsigned v)
{
    Nvnet *s = &g_nv;
    pthread_mutex_lock(&s->lock);
    nc_rec_get(r, &nc_rec_Nvnet, s, v);
    if (s->tx_off > TX_BUF + 1) {
        s->tx_off = TX_BUF + 1;             /* not a state's: the frame dropped */
    }
    /* the network isn't in the state: the link as this run's, negotiated
     * again if the title had it up */
    s->autoneg_at = 0;
    if (!nc_net_link()) {
        regs_link_down(s);
        link_change(s);
    } else if (s->phy[MII_BMCR] & BMCR_AUTOEN) {
        autoneg_restart(s);
    }
    s->irq = (reg(s, IRQ_STATUS) & reg(s, IRQ_MASK)) == 0;
    update_irq(s);                          /* the line as the registers say, set either way */
    station_address(s);
    pthread_mutex_unlock(&s->lock);
    return !r->fail;
}

/* A state from before the NIC: it as at power-on, so that nothing of this
 * run's (its rings' addresses, the receiver on) works on the state's RAM. */
void nc_nic_state_absent(void)
{
    Nvnet *s = &g_nv;
    pthread_mutex_lock(&s->lock);
    nvnet_reset(s);
    s->irq = true;
    update_irq(s);                          /* the line low, set either way */
    pthread_mutex_unlock(&s->lock);
}
