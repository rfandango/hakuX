/*
 * hakuX native core — the MCPX Audio Codec Interface: an AC'97 controller
 * (Intel ICH register set) at 0xFEC00000, IRQ 6, with a STAC9700 codec.
 * Mixer (NAM) registers at +0x000, bus master (NABM) at +0x100.
 *
 * DirectSound waits for codec 0 to report ready and runs PCM out through a
 * buffer descriptor list. The sound itself comes from the APU, so PCM out
 * is consumed at the real-time rate (48 kHz, 16-bit stereo) and dropped:
 * the list advances, completions set status and interrupt as on hardware.
 * Register behaviour follows the AC'97 model the Xbox titles run against
 * in xemu (QEMU's ac97 behind mcpx-aci); S/PDIF out does not advance there
 * and does not here.
 */
#include "native/dev/dev.h"
#include "native/core/irq.h"
#include "native/core/mmio.h"
#include "native/core/vm.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#define ACI_BASE 0xFEC00000u
#define ACI_SIZE 0x1000u
#define ACI_IRQ  6

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

enum { PI = 0, PO = 1, MC = 2, SO = 7, NCH = 8 };

/* Channel registers, at channel * 16. */
#define R_BDBAR 0x0
#define R_CIV   0x4
#define R_LVI   0x5
#define R_SR    0x6
#define R_PICB  0x8
#define R_PIV   0xA
#define R_CR    0xB
#define GLOB_CNT 0x2C
#define GLOB_STA 0x30
#define CAS      0x34

#define SR_DCH   0x01
#define SR_CELV  0x02
#define SR_LVBCI 0x04
#define SR_BCIS  0x08
#define SR_FIFOE 0x10
#define SR_RO_MASK     (SR_DCH | SR_CELV)
#define SR_WCLEAR_MASK (SR_FIFOE | SR_BCIS | SR_LVBCI)
#define SR_INT_MASK    (SR_FIFOE | SR_BCIS | SR_LVBCI)

#define CR_RPBM  0x01
#define CR_RR    0x02
#define CR_LVBIE 0x04
#define CR_FEIE  0x08
#define CR_IOCE  0x10
#define CR_VALID_MASK      0x1F
#define CR_DONT_CLEAR_MASK (CR_IOCE | CR_FEIE | CR_LVBIE)

#define GC_WR 4
#define GC_CR 2
#define GC_VALID_MASK 0x3F
#define GS_S0CR  (1u << 8)
#define GS_MINT  (1u << 7)
#define GS_POINT (1u << 6)
#define GS_PIINT (1u << 5)
#define GS_RO_MASK ((1u << 14) | (1u << 13) | (1u << 12) | (1u << 9) | (1u << 8) | 0xF6u)
#define GS_WCLEAR_MASK ((1u << 15) | (1u << 11) | (1u << 10) | 1u)
#define GS_VALID_MASK ((1u << 18) - 1)

#define BD_IOC (1u << 31)
#define BD_BUP (1u << 30)

/* Codec (mixer) registers. */
#define AC97_Reset                    0x00
#define AC97_Master_Volume_Mute       0x02
#define AC97_PCM_Out_Volume_Mute      0x18
#define AC97_Record_Select            0x1A
#define AC97_Record_Gain_Mute         0x1C
#define AC97_Powerdown_Ctrl_Stat      0x26
#define AC97_Extended_Audio_ID        0x28
#define AC97_Extended_Audio_Ctrl_Stat 0x2A
#define AC97_PCM_Front_DAC_Rate       0x2C
#define AC97_PCM_Surround_DAC_Rate    0x2E
#define AC97_PCM_LFE_DAC_Rate         0x30
#define AC97_PCM_LR_ADC_Rate          0x32
#define AC97_MIC_ADC_Rate             0x34
#define AC97_Vendor_ID1               0x7C
#define AC97_Vendor_ID2               0x7E
#define EACS_VRA 1
#define EACS_VRM 8

typedef struct Channel {
    uint32_t bdbar;
    uint8_t civ, lvi, piv, cr;
    uint16_t sr, picb;
    bool bd_valid;
    uint32_t bd_addr, bd_ctl;
} Channel;

typedef struct Aci {
    NcVm *vm;
    pthread_mutex_t lock;
    uint32_t glob_cnt, glob_sta, cas;
    Channel ch[NCH];
    uint8_t mixer[256];
    bool irq;
    uint64_t po_bytes;
} Aci;

static Aci g_aci;
static int g_trace; /* NC_TRACE_ACI: descriptor fetches, completions, interrupts (first 200) */

#define TRACE(...)                                                                                                         do {                                                                                                                       if (g_trace > 0) {                                                                                                         g_trace--;                                                                                                             nc_log("hakuX-native-aci", __VA_ARGS__);                                                                           }                                                                                                                  } while (0)

static void update_irq(Aci *a)
{
    const bool level = (a->glob_sta & (GS_PIINT | GS_POINT | GS_MINT)) != 0;
    if (level != a->irq) {
        TRACE("irq %d (glob_sta %08x)", level, a->glob_sta);
        a->irq = level;
        nc_irq_set(ACI_IRQ, level);
    }
}

static uint32_t channel_gs_bit(unsigned i)
{
    return i == PI ? GS_PIINT : i == PO ? GS_POINT : i == MC ? GS_MINT : 0;
}

static void fetch_bd(Aci *a, Channel *c)
{
    const uint32_t pa = c->bdbar + c->civ * 8u;
    if ((uint64_t)pa + 8 <= a->vm->ram_size) {
        const uint8_t *b = a->vm->phys + pa;
        c->bd_addr = nc_rd32(b) & ~3u;
        c->bd_ctl = nc_rd32(b + 4);
    } else {
        c->bd_addr = c->bd_ctl = 0;
    }
    c->bd_valid = true;
    c->picb = (uint16_t)c->bd_ctl;
    TRACE("ch %u bd %u at %08x: addr %08x ctl %08x (%u samples)", (unsigned)(c - a->ch), c->civ, pa, c->bd_addr,
          c->bd_ctl, c->picb);
}

/* The status register changes; the channel's interrupt follows its enables. */
static void update_sr(Aci *a, unsigned i, uint16_t new_sr)
{
    Channel *c = &a->ch[i];
    const uint16_t new_mask = new_sr & SR_INT_MASK, old_mask = c->sr & SR_INT_MASK;
    c->sr = new_sr;
    if (new_mask == old_mask) {
        return;
    }
    const uint32_t bit = channel_gs_bit(i);
    if (!new_mask) {
        a->glob_sta &= ~bit;
    } else if (((new_mask & SR_LVBCI) && (c->cr & CR_LVBIE)) || ((new_mask & SR_BCIS) && (c->cr & CR_IOCE))) {
        a->glob_sta |= bit;
    } else {
        return;
    }
    update_irq(a);
}

static void reset_channel(Aci *a, unsigned i)
{
    Channel *c = &a->ch[i];
    c->bdbar = 0;
    c->civ = c->lvi = c->piv = 0;
    update_sr(a, i, SR_DCH);
    c->picb = 0;
    c->cr &= CR_DONT_CLEAR_MASK;
    c->bd_valid = false;
}

static void mixer_store(Aci *a, uint32_t i, uint16_t v)
{
    if (i + 2 <= sizeof(a->mixer)) {
        nc_wr16(a->mixer + i, v);
    }
}

static uint16_t mixer_load(Aci *a, uint32_t i)
{
    return i + 2 <= sizeof(a->mixer) ? nc_rd16(a->mixer + i) : 0xFFFF;
}

static void mixer_reset(Aci *a)
{
    memset(a->mixer, 0, sizeof(a->mixer));
    mixer_store(a, AC97_Powerdown_Ctrl_Stat, 0x000F); /* ADC, DAC, analog, Vref ready */
    mixer_store(a, AC97_Vendor_ID1, 0x8384);          /* SigmaTel STAC9700 */
    mixer_store(a, AC97_Vendor_ID2, 0x7600);
    mixer_store(a, AC97_Extended_Audio_ID, 0x0809);
    mixer_store(a, AC97_Extended_Audio_Ctrl_Stat, 0x0009);
    mixer_store(a, AC97_PCM_Front_DAC_Rate, 0xBB80);
    mixer_store(a, AC97_PCM_Surround_DAC_Rate, 0xBB80);
    mixer_store(a, AC97_PCM_LFE_DAC_Rate, 0xBB80);
    mixer_store(a, AC97_PCM_LR_ADC_Rate, 0xBB80);
    mixer_store(a, AC97_MIC_ADC_Rate, 0xBB80);
    mixer_store(a, AC97_Master_Volume_Mute, 0x8000);
    mixer_store(a, AC97_PCM_Out_Volume_Mute, 0x8808);
    mixer_store(a, AC97_Record_Gain_Mute, 0x8808);
}

static void nam_write(Aci *a, uint32_t off, uint16_t v)
{
    switch (off) {
    case AC97_Reset:
        mixer_reset(a);
        break;
    case AC97_Powerdown_Ctrl_Stat:
        mixer_store(a, off, (uint16_t)((v & ~0x800Fu) | (mixer_load(a, off) & 0xF)));
        break;
    case AC97_Vendor_ID1:
    case AC97_Vendor_ID2:
    case AC97_Extended_Audio_ID:
        break; /* read only */
    case AC97_Extended_Audio_Ctrl_Stat:
        if (!(v & EACS_VRA)) {
            mixer_store(a, AC97_PCM_Front_DAC_Rate, 0xBB80);
            mixer_store(a, AC97_PCM_LR_ADC_Rate, 0xBB80);
        }
        if (!(v & EACS_VRM)) {
            mixer_store(a, AC97_MIC_ADC_Rate, 0xBB80);
        }
        mixer_store(a, off, v);
        break;
    case AC97_PCM_Front_DAC_Rate:
    case AC97_PCM_LR_ADC_Rate:
        if (mixer_load(a, AC97_Extended_Audio_Ctrl_Stat) & EACS_VRA) {
            mixer_store(a, off, v);
        }
        break;
    case AC97_MIC_ADC_Rate:
        if (mixer_load(a, AC97_Extended_Audio_Ctrl_Stat) & EACS_VRM) {
            mixer_store(a, off, v);
        }
        break;
    case AC97_Master_Volume_Mute:
    case AC97_PCM_Out_Volume_Mute:
    case AC97_Record_Gain_Mute:
    case AC97_Record_Select:
        mixer_store(a, off, v);
        break;
    default:
        break; /* the codec's other features are not emulated: read only */
    }
}

/* ------------------------------------------------------------ bus master */

static uint32_t nabm_readb(Aci *a, uint32_t off)
{
    if (off == CAS) {
        const uint32_t v = a->cas;
        a->cas = 1;
        return v;
    }
    const unsigned i = (off >> 4) & 7;
    const Channel *c = &a->ch[i];
    switch (off & 0xF) {
    case R_CIV: return c->civ;
    case R_LVI: return c->lvi;
    case R_PIV: return c->piv;
    case R_CR: return c->cr;
    case R_SR: return c->sr & 0xFF;
    default: return 0xFF;
    }
}

static uint32_t nabm_readw(Aci *a, uint32_t off)
{
    const unsigned i = (off >> 4) & 7;
    if (off < 0x80 && (off & 0xF) == R_SR) {
        return a->ch[i].sr;
    }
    if (off < 0x80 && (off & 0xF) == R_PICB) {
        return a->ch[i].picb;
    }
    return nabm_readb(a, off) | nabm_readb(a, off + 1) << 8;
}

static uint32_t nabm_readl(Aci *a, uint32_t off)
{
    if (off == GLOB_CNT) {
        return a->glob_cnt;
    }
    if (off == GLOB_STA) {
        return a->glob_sta | GS_S0CR; /* codec 0 always ready */
    }
    const unsigned i = (off >> 4) & 7;
    const Channel *c = &a->ch[i];
    switch (off & 0xF) {
    case R_BDBAR: return c->bdbar;
    case R_CIV: return c->civ | (uint32_t)c->lvi << 8 | (uint32_t)c->sr << 16;
    case R_PICB: return c->picb | (uint32_t)c->piv << 16 | (uint32_t)c->cr << 24;
    default: return nabm_readw(a, off) | nabm_readw(a, off + 2) << 16;
    }
}

static void nabm_writeb(Aci *a, uint32_t off, uint32_t v)
{
    if (off >= 0x80) {
        return;
    }
    const unsigned i = (off >> 4) & 7;
    Channel *c = &a->ch[i];
    switch (off & 0xF) {
    case R_LVI:
        if ((c->cr & CR_RPBM) && (c->sr & SR_DCH)) {
            /* A halted channel with new buffers resumes. */
            c->sr &= ~(SR_DCH | SR_CELV);
            c->civ = c->piv;
            c->piv = (c->piv + 1) % 32;
            fetch_bd(a, c);
        }
        c->lvi = v % 32;
        break;
    case R_CR:
        if (v & CR_RR) {
            reset_channel(a, i);
        } else {
            c->cr = v & CR_VALID_MASK;
            if (!(c->cr & CR_RPBM)) {
                c->sr |= SR_DCH;
            } else {
                c->civ = c->piv;
                c->piv = (c->piv + 1) % 32;
                fetch_bd(a, c);
                c->sr &= ~SR_DCH;
            }
        }
        break;
    case R_SR:
        c->sr |= v & ~(SR_RO_MASK | SR_WCLEAR_MASK);
        update_sr(a, i, c->sr & ~(v & SR_WCLEAR_MASK));
        break;
    default:
        break;
    }
}

static void nabm_writew(Aci *a, uint32_t off, uint32_t v)
{
    if (off < 0x80 && (off & 0xF) == R_SR) {
        Channel *c = &a->ch[(off >> 4) & 7];
        c->sr |= v & ~(SR_RO_MASK | SR_WCLEAR_MASK);
        update_sr(a, (off >> 4) & 7, c->sr & ~(v & SR_WCLEAR_MASK));
        return;
    }
    nabm_writeb(a, off, v & 0xFF);
    nabm_writeb(a, off + 1, (v >> 8) & 0xFF);
}

static void nabm_writel(Aci *a, uint32_t off, uint32_t v)
{
    if (off == GLOB_CNT) {
        if (!(v & (GC_WR | GC_CR))) {
            a->glob_cnt = v & GC_VALID_MASK;
        }
        return;
    }
    if (off == GLOB_STA) {
        a->glob_sta &= ~(v & GS_WCLEAR_MASK);
        a->glob_sta |= (v & ~(GS_WCLEAR_MASK | GS_RO_MASK)) & GS_VALID_MASK;
        return;
    }
    if (off < 0x80 && (off & 0xF) == R_BDBAR) {
        a->ch[(off >> 4) & 7].bdbar = v & ~3u;
        return;
    }
    nabm_writew(a, off, v & 0xFFFF);
    nabm_writew(a, off + 2, v >> 16);
}

/* ----------------------------------------------------------------- PCM out */

/* Consumes up to `bytes` of PCM out; completions as the hardware reports them. */
static void transfer_po(Aci *a, uint32_t bytes)
{
    Channel *c = &a->ch[PO];
    if ((c->sr & SR_DCH) || !(c->cr & CR_RPBM)) {
        return;
    }
    unsigned guard = 0;
    while (bytes >= 2 && guard++ < 64) {
        if (!c->bd_valid) {
            fetch_bd(a, c);
        }
        if (!c->picb) {
            if (c->civ == c->lvi) {
                c->sr |= SR_DCH;
                break;
            }
            c->sr &= ~SR_CELV;
            c->civ = c->piv;
            c->piv = (c->piv + 1) % 32;
            fetch_bd(a, c);
            continue;
        }
        const uint32_t take = MIN(bytes, (uint32_t)c->picb << 1);
        c->bd_addr += take;
        c->picb -= (uint16_t)(take >> 1);
        bytes -= take;
        a->po_bytes += take;
        if (!c->picb) {
            TRACE("ch %u bd %u done (ctl %08x, lvi %u)", (unsigned)PO, c->civ, c->bd_ctl, c->lvi);
            uint16_t new_sr = c->sr & ~SR_CELV;
            if (c->bd_ctl & BD_IOC) {
                new_sr |= SR_BCIS;
            }
            if (c->civ == c->lvi) {
                new_sr |= SR_LVBCI | SR_DCH | SR_CELV;
                update_sr(a, PO, new_sr);
                break;
            }
            c->civ = c->piv;
            c->piv = (c->piv + 1) % 32;
            fetch_bd(a, c);
            update_sr(a, PO, new_sr);
        }
    }
}

/* The PCM thread (outside Aci, whose layout the savestate keeps). */
static struct {
    pthread_t th;
    bool started;
    int quit;                   /* nc_dev_aci_stop */
} g_aci_thr;

static void *pcm_thread(void *arg)
{
    Aci *a = arg;
    nc_pause_register("ac97");
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!__atomic_load_n(&g_aci_thr.quit, __ATOMIC_ACQUIRE)) {
        if (nc_paused()) {
            nc_pause_wait();                /* the emulation paused: no samples played */
            clock_gettime(CLOCK_MONOTONIC, &next);
        }
        next.tv_nsec += 5000000; /* 5 ms: 240 frames */
        if (next.tv_nsec >= 1000000000) {
            next.tv_nsec -= 1000000000;
            next.tv_sec++;
        }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) != 0) {
        }
        pthread_mutex_lock(&a->lock);
        transfer_po(a, 240 * 4);
        pthread_mutex_unlock(&a->lock);
    }
    return NULL;
}

/* ------------------------------------------------------------- registers */

static uint64_t aci_read(void *opaque, uint32_t off, unsigned size)
{
    Aci *a = opaque;
    uint32_t v;
    pthread_mutex_lock(&a->lock);
    if (off < 0x100) {
        a->cas = 0;
        v = size == 2 ? mixer_load(a, off) : 0xFFFFFFFFu;
    } else {
        const uint32_t o = off - 0x100;
        v = size == 1 ? nabm_readb(a, o) : size == 2 ? nabm_readw(a, o) : nabm_readl(a, o);
    }
    pthread_mutex_unlock(&a->lock);
    return v;
}

static void aci_write(void *opaque, uint32_t off, uint64_t value, unsigned size)
{
    Aci *a = opaque;
    const uint32_t v = (uint32_t)value;
    pthread_mutex_lock(&a->lock);
    if (off < 0x100) {
        a->cas = 0;
        if (size == 2) {
            nam_write(a, off, (uint16_t)v);
        }
    } else {
        const uint32_t o = off - 0x100;
        if (size == 1) {
            nabm_writeb(a, o, v);
        } else if (size == 2) {
            nabm_writew(a, o, v);
        } else {
            nabm_writel(a, o, v);
        }
    }
    pthread_mutex_unlock(&a->lock);
}

void nc_dev_aci_init(struct NcVm *vm)
{
    Aci *a = &g_aci;
    a->vm = vm;
    g_trace = getenv("NC_TRACE_ACI") ? 200 : 0;
    pthread_mutex_init(&a->lock, NULL);
    mixer_reset(a);
    for (unsigned i = 0; i < NCH; i++) {
        a->ch[i].sr = SR_DCH;
    }
    static NcMmioDev dev = {
        .name = "aci",
        .base = ACI_BASE,
        .size = ACI_SIZE,
        .read = aci_read,
        .write = aci_write,
        .opaque = &g_aci,
    };
    nc_mmio_register(&dev);
    g_aci_thr.quit = 0;
    g_aci_thr.started = pthread_create(&g_aci_thr.th, NULL, pcm_thread, a) == 0;
}

void nc_dev_aci_stop(void)
{
    if (g_aci_thr.started) {
        __atomic_store_n(&g_aci_thr.quit, 1, __ATOMIC_RELEASE);
        pthread_join(g_aci_thr.th, NULL);   /* within its 5 ms period */
        g_aci_thr.started = false;
    }
}

void nc_dev_aci_free(void)
{
    pthread_mutex_destroy(&g_aci.lock);
    memset(&g_aci, 0, sizeof(g_aci));
}

/* ---------------------------------------------------------- savestates */

#define NC_DEF_SECTION ACI
#define NC_DEF_FILE "native/state/aci.def"
#include "native/state/def.h"
#include "native/state/sections.h"

bool nc_aci_state_save(NcBuf *b)
{
    Aci *a = &g_aci;
    pthread_mutex_lock(&a->lock);
    nc_rec_put(b, &nc_rec_Aci, a);
    pthread_mutex_unlock(&a->lock);
    return true;
}

bool nc_aci_state_load(NcRd *r, unsigned v)
{
    Aci *a = &g_aci;
    pthread_mutex_lock(&a->lock);
    nc_rec_get(r, &nc_rec_Aci, a, v);
    a->irq = (a->glob_sta & (GS_PIINT | GS_POINT | GS_MINT)) == 0;
    update_irq(a);                          /* the line as the registers say, set either way */
    pthread_mutex_unlock(&a->lock);
    return !r->fail;
}
