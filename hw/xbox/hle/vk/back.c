/*
 * hakuX HLE GPU — render thread: consumes the command ring, records clears
 * into lazily opened dynamic-rendering passes, submits at FENCE/SWAP/KICK/
 * READBACK with the front end's timeline value (spec §5.3, §5.10-5.12).
 */
#include "hw/xbox/hle/base.h"
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/prop.h"
#include "hw/xbox/hle/tex/xdk.h"
#include "hw/xbox/hle/tex/xdk_res.h"
#include "hw/xbox/hle/front/fe.h"
#include "hw/xbox/hle/mem/barrier.h"
#include "hw/xbox/hle/mem/guestmem.h"
#include "hw/xbox/hle/shader/glsl_compile.h"
#include "hw/xbox/hle/vk/back_int.h"
#include "hw/xbox/hle/vk/batch.h"
#include "hw/xbox/hle/vk/pcache.h"
#include "hw/xbox/hle/vk/pipeasync.h"
#include "hw/xbox/hle/hitch.h"
#include "hw/xbox/hle/prof.h"
#include "hw/xbox/hle/vk/display.h"
#include "hw/xbox/hle/vk/guest_ram.h"
#include "hw/xbox/hle/vk/quirks.h"

#define T (g_hvk.t)

typedef struct HleFrame {
    VkCommandPool pool;
    VkCommandBuffer cb;
    uint64_t value;
} HleFrame;

static struct {
    bool running;
    HleThread thread;
    HleEvent kick;
    int sleeping;                       /* the render thread is about to wait on kick (atomic) */
    HleFrame fr[HLE_FRAMES];
    unsigned cur;
    bool recording;
    uint32_t rt, ds;
    bool pass_open;
    uint64_t zeq_wgen;          /* the depth buffer's wgen at the open pass's last
                                   depth read barrier (hle_draw_zeq); 0 none yet */
    bool pend_clear;
    uint32_t pend_flags, pend_s;
    VkClearColorValue pend_color;
    float pend_z;
    HleCmdViewport vp;
    uint32_t disp_w, disp_h, present_id;
    bool warned_partial;
    HleBackStats st;
} g_back;

/* ---- visibility tests (occlusion queries) --------------------------------------
 * A test's draws are counted by one occlusion query per render pass they fall
 * in (a query may not span passes); at VIS_END the test's queries are parked
 * until the submission closing the recording completes, then summed and
 * published by index for GetVisibilityTestResult (front end), and written
 * to the guest's report when it has one (push-buffer mode's tests have only
 * that: HLE_VIS_NO_INDEX). */
#define VIS_SLOTS      4096                     /* query pool entries (ring) */
#define VIS_INDICES    HLE_VIS_INDICES
#define VIS_MAX_PARTS  16
#define VIS_PENDING    1024

static struct {
    VkQueryPool pool;
    uint32_t next;                              /* ring position */
    bool active;                                /* a test is open */
    int32_t in_pass;                            /* its query in the open pass, -1 none */
    uint32_t parts[VIS_MAX_PARTS], nparts;
    struct {
        uint32_t index, div, nparts, parts[VIS_MAX_PARTS];
        uint32_t report;                        /* guest PA of the device's report, 0 none */
        uint64_t after;                         /* HLE_SURF_PARKED until submitted */
        uint64_t expect;                        /* the value its poller waits for */
    } pend[VIS_PENDING];
    uint32_t npend;
    uint64_t lost;
} g_vis = { .in_pass = -1 };
static struct {
    int all;                                    /* -1: environment not read yet */
    int nlog;                                   /* results still to log */
    bool off;                                   /* no GPU queries (debug.hakux.visoff) */
} g_vis_dbg = { -1, 0 };

static bool vis_init(void)
{
    if (g_vis.pool) {
        return true;
    }
    if (!g_hvk.caps.host_query_reset) {
        return false;
    }
    VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                 .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = VIS_SLOTS };
    if (T.vkCreateQueryPool(g_hvk.dev, &qi, NULL, &g_vis.pool) != VK_SUCCESS) {
        g_vis.pool = VK_NULL_HANDLE;
        return false;
    }
    T.vkResetQueryPool(g_hvk.dev, g_vis.pool, 0, VIS_SLOTS);
    return true;
}

/* A draw inside the open pass while a test is open: its query starts. */
static void vis_draw(VkCommandBuffer cb)
{
    if (!g_vis.active || g_vis.in_pass >= 0 || g_vis.nparts >= VIS_MAX_PARTS ||
        g_vis_dbg.off || !vis_init()) {
        return;
    }
    uint32_t slot = g_vis.next++ % VIS_SLOTS;
    T.vkResetQueryPool(g_hvk.dev, g_vis.pool, slot, 1);    /* the ring is far ahead of use */
    T.vkCmdBeginQuery(cb, g_vis.pool, slot,
                      g_hvk.caps.occl_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    g_vis.in_pass = (int32_t)slot;
}

/* Before the pass ends (close_pass, VIS_END): the pass's query ends. */
static void vis_pass_end(VkCommandBuffer cb)
{
    if (g_vis.in_pass >= 0) {
        T.vkCmdEndQuery(cb, g_vis.pool, (uint32_t)g_vis.in_pass);
        g_vis.parts[g_vis.nparts++] = (uint32_t)g_vis.in_pass;
        g_vis.in_pass = -1;
    }
}

/* The guest's report for a finished test, as GET_REPORT writes it: time
 * stamp, count, then status 0 (LTCG titles poll the status word). */
static void vis_report(uint32_t pa, uint32_t count)
{
    uint8_t *p = pa ? hle_pa_ptr(pa, 16) : NULL;
    if (p) {
        hle_stl_le(p, (uint32_t)HLE_VIS_TIMESTAMP);
        hle_stl_le(p + 4, (uint32_t)(HLE_VIS_TIMESTAMP >> 32));
        hle_stl_le(p + 8, count);
        hle_smp_wmb();
        hle_atomic_set((uint32_t *)(p + 12), 0);
        hle_barrier_report_written(pa);     /* a guest read waiting for it goes on */
    }
}

static void vis_reap(uint64_t completed);

static void cmd_vis_end(const HleCmdVis *c)
{
    if (g_back.pass_open) {
        vis_pass_end(g_back.fr[g_back.cur].cb);
    }
    g_vis.active = false;
    if (c->index >= VIS_INDICES && !c->report) {
        return;                                 /* HLE_VIS_NO_INDEX has a report */
    }
    if (g_vis.npend >= VIS_PENDING) {
        vis_reap(hle_tl_completed());           /* the ring loop reaps only once it drains */
    }
    if (g_vis.npend >= VIS_PENDING) {
        if (g_vis.lost++ == 0) {
            hle_log("hakuX-hle-back", "vis: %u tests pending, index %u reported occluded",
                    g_vis.npend, c->index);
        }
        g_vis.nparts = 0;
        vis_report(c->report, 0);
        return;
    }
    typeof(g_vis.pend[0]) *p = &g_vis.pend[g_vis.npend++];
    p->index = c->index;
    HleVkSurf *vrt = hle_surf_get(g_back.rt);
    uint32_t vsc = vrt ? vrt->scale : g_hle_rscale;
    p->div = MAX(c->div, 1u) * vsc * vsc;               /* guest pixels */
    p->report = c->report;
    p->expect = c->expect;
    p->nparts = g_vis.nparts;
    memcpy(p->parts, g_vis.parts, sizeof(p->parts));
    /* parked until the next submission, also without an open recording:
     * the front end waits for the value after its last one (and kicks once
     * for it); the last submitted value is older (Halo ends tests with
     * nothing recorded and polled forever) */
    p->after = HLE_SURF_PARKED;
    g_vis.nparts = 0;
}

static void vis_stamp(uint64_t value)
{
    for (uint32_t i = 0; i < g_vis.npend; i++) {
        if (g_vis.pend[i].after == HLE_SURF_PARKED) {
            g_vis.pend[i].after = value;
        }
    }
}

static void vis_reap(uint64_t completed)
{
    uint32_t j = 0;
    for (uint32_t i = 0; i < g_vis.npend; i++) {
        typeof(g_vis.pend[0]) *p = &g_vis.pend[i];
        if (p->after == HLE_SURF_PARKED || p->after > completed) {
            g_vis.pend[j++] = *p;
            continue;
        }
        uint64_t sum = 0;
        for (uint32_t k = 0; k < p->nparts; k++) {
            uint64_t n = 0;
            if (T.vkGetQueryPoolResults(g_hvk.dev, g_vis.pool, p->parts[k], 1, sizeof(n), &n,
                                        sizeof(n), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                sum += n;
            }
        }
        uint32_t count = (uint32_t)(sum / p->div);
        {   /* debug: HAKUX_HLE_VISALL=1 (or debug.hakux.visall=1, live)
             * reports every test fully visible; HAKUX_HLE_VISLOG=<n> (or a new
             * debug.hakux.vislog=<n>) logs the next n results */
            if (g_vis_dbg.all < 0) {
                g_vis_dbg.all = getenv("HAKUX_HLE_VISALL") != NULL;
                const char *e = getenv("HAKUX_HLE_VISLOG");
                g_vis_dbg.nlog = e ? atoi(e) : 0;
            }
            if (g_vis_dbg.nlog > 0) {
                g_vis_dbg.nlog--;
                hle_log("hakuX-hle-back", "vis index %u: %u (parts %u, div %u, report %08x, "
                        "after %llu, expect %llu, completed %llu, swap %llu)",
                        p->index, count, p->nparts, p->div, p->report,
                        (unsigned long long)p->after, (unsigned long long)p->expect,
                        (unsigned long long)completed, (unsigned long long)g_back.st.swaps);
            }
            if (g_vis_dbg.all) {
                count = 640 * 480;
            }
        }
        vis_report(p->report, count);
    }
    g_vis.npend = j;
}

static void on_complete(uint64_t completed)
{
    hle_hitch_gpu_done();
    hle_disp_on_complete(completed);
    /* Visibility results are published by the render thread's reap, which
     * runs when it wakes: a title polling a test with nothing else to submit
     * (PGR2 loops on GetVisibilityTestResult right after kicking; Halo sends
     * nothing more after its one kick) would otherwise poll forever once the
     * render thread went idle. */
    if (hle_atomic_read(&g_vis.npend)) {
        hle_event_set(&g_back.kick);
    }
}

/* GPU time per command buffer while the host-time profile is on (debug): a
 * begin and an end timestamp per frame slot, read when the slot comes round
 * (its submission has completed); logged once a second. */
static struct {
    VkQueryPool pool;
    bool failed, armed[HLE_FRAMES];
    uint64_t ns, n, max_ns;
    int64_t last_log;
} g_gput;

static void gput_collect(unsigned slot)
{
    if (!g_gput.armed[slot]) {
        return;
    }
    g_gput.armed[slot] = false;
    uint64_t t[2];
    if (T.vkGetQueryPoolResults(g_hvk.dev, g_gput.pool, 2 * slot, 2, sizeof(t), t,
                                sizeof(t[0]), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
        t[1] >= t[0]) {
        uint64_t ns = (uint64_t)((double)(t[1] - t[0]) * g_hvk.props.limits.timestampPeriod);
        g_gput.ns += ns;
        g_gput.n++;
        g_gput.max_ns = MAX(g_gput.max_ns, ns);
    }
    int64_t now = hle_now_ns();
    if (now - g_gput.last_log >= 1000000000) {
        if (g_gput.last_log && g_gput.n) {
            hle_log("hakuX-hle-prof", "gpu=%.1fms/s cbs=%llu avg=%.2fms max=%.2fms",
                    g_gput.ns / 1e6, (unsigned long long)g_gput.n,
                    g_gput.ns / 1e6 / g_gput.n, g_gput.max_ns / 1e6);
        }
        g_gput.ns = g_gput.n = g_gput.max_ns = 0;
        g_gput.last_log = now;
    }
}

static void gput_begin(VkCommandBuffer cb, unsigned slot)
{
    if (!g_hle_prof_on || g_gput.failed || hle_quirk(HLE_QUIRK_NO_TIMESTAMPS)) {
        return;
    }
    if (!g_gput.pool) {
        VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                     .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                     .queryCount = 2 * HLE_FRAMES };
        if (T.vkCreateQueryPool(g_hvk.dev, &qi, NULL, &g_gput.pool) != VK_SUCCESS) {
            g_gput.pool = VK_NULL_HANDLE;
            g_gput.failed = true;
            return;
        }
    }
    T.vkCmdResetQueryPool(cb, g_gput.pool, 2 * slot, 2);
    T.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_gput.pool, 2 * slot);
    g_gput.armed[slot] = true;
}

static void gput_end(VkCommandBuffer cb, unsigned slot)
{
    if (g_gput.armed[slot]) {
        T.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_gput.pool,
                              2 * slot + 1);
    }
}

/* Why a pass ended (close_pass_why; the pass profile logs them). */
enum { PE_OTHER, PE_SUBMIT, PE_SETRT, PE_TEX, PE_PREP, PE_LREAD, PE_SWAP, PE_COPY, PE_N };
static const char *const k_pe_names[PE_N] = { "other", "submit", "target", "upload",
                                              "sampled target", "local read", "swap", "copy" };
static uint64_t g_pe[PE_N];

/* GPU time per render pass while the profile is on (debug): bottom-of-pipe
 * timestamps before and after each pass (the earlier work done, then this
 * pass), read when the slot comes round, summed by the pass's place in its
 * frame and logged once a second with what the passes held. */
#define PT_MAX 48
typedef struct PtPass {
    uint16_t ord;                       /* the pass's place in its frame */
    uint16_t w, h;
    int16_t fmt;                        /* the target's (-1: depth only) */
    uint8_t ds, clear;
    uint32_t draws, verts, fb, pipes;
} PtPass;
static struct {
    VkQueryPool pool;
    bool failed, open;
    unsigned n[HLE_FRAMES];             /* passes timed in the slot's buffer */
    PtPass pass[HLE_FRAMES][PT_MAX];
    uint16_t ord;                       /* passes so far this frame */
    uint32_t draws0, fb;                /* the open pass: at its start, fb-fetch draws */
    uint64_t verts0, pipes0;
    struct {
        double ns;
        uint64_t n, draws, verts, fb, pipes;
        PtPass last;
    } sum[PT_MAX];
    int64_t last_log;
} g_pt;

static void pt_slot_begin(VkCommandBuffer cb, unsigned slot)
{
    g_pt.n[slot] = 0;
    if (!g_hle_prof_on || g_pt.failed || hle_quirk(HLE_QUIRK_NO_TIMESTAMPS)) {
        return;
    }
    if (!g_pt.pool) {
        VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                     .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                     .queryCount = 2 * PT_MAX * HLE_FRAMES };
        if (T.vkCreateQueryPool(g_hvk.dev, &qi, NULL, &g_pt.pool) != VK_SUCCESS) {
            g_pt.pool = VK_NULL_HANDLE;
            g_pt.failed = true;
            return;
        }
    }
    T.vkCmdResetQueryPool(cb, g_pt.pool, 2 * PT_MAX * slot, 2 * PT_MAX);
}

static void pt_pass_begin(VkCommandBuffer cb, uint32_t w, uint32_t h, int fmt, bool ds,
                          uint32_t clear)
{
    unsigned slot = g_back.cur, k = g_pt.n[slot];
    g_pt.open = false;
    if (!g_hle_prof_on || !g_pt.pool || k >= PT_MAX) {
        return;
    }
    T.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_pt.pool,
                          2 * (PT_MAX * slot + k));
    PtPass *p = &g_pt.pass[slot][k];
    *p = (PtPass){ .ord = g_pt.ord, .w = (uint16_t)w, .h = (uint16_t)h, .fmt = (int16_t)fmt,
                   .ds = ds, .clear = (uint8_t)clear };
    g_pt.draws0 = (uint32_t)g_back.st.draws;
    hle_draw_counts(&g_pt.verts0, &g_pt.pipes0);
    g_pt.fb = 0;
    g_pt.open = true;
}

static void pt_pass_end(VkCommandBuffer cb)
{
    if (!g_pt.open) {
        g_pt.ord++;
        return;
    }
    g_pt.open = false;
    unsigned slot = g_back.cur, k = g_pt.n[slot]++;
    T.vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_pt.pool,
                          2 * (PT_MAX * slot + k) + 1);
    PtPass *p = &g_pt.pass[slot][k];
    uint64_t verts, pipes;
    hle_draw_counts(&verts, &pipes);
    p->draws = (uint32_t)g_back.st.draws - g_pt.draws0;
    p->verts = (uint32_t)(verts - g_pt.verts0);
    p->pipes = (uint32_t)(pipes - g_pt.pipes0);
    p->fb = g_pt.fb;
    g_pt.ord++;
}

static void pt_collect(unsigned slot)
{
    unsigned n = g_pt.n[slot];
    uint64_t t[2 * PT_MAX];
    if (n && T.vkGetQueryPoolResults(g_hvk.dev, g_pt.pool, 2 * PT_MAX * slot, 2 * n,
                                     sizeof(t[0]) * 2 * n, t, sizeof(t[0]),
                                     VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        for (unsigned k = 0; k < n; k++) {
            const PtPass *p = &g_pt.pass[slot][k];
            if (t[2 * k + 1] < t[2 * k] || p->ord >= PT_MAX) {
                continue;
            }
            typeof(g_pt.sum[0]) *s = &g_pt.sum[p->ord];
            s->ns += (double)(t[2 * k + 1] - t[2 * k]) * g_hvk.props.limits.timestampPeriod;
            s->n++;
            s->draws += p->draws;
            s->verts += p->verts;
            s->fb += p->fb;
            s->pipes += p->pipes;
            s->last = *p;
        }
    }
    g_pt.n[slot] = 0;
    if (!g_pt.pool) {
        return;
    }
    int64_t now = hle_now_ns();
    if (now - g_pt.last_log < 1000000000) {
        return;
    }
    double secs = g_pt.last_log ? (now - g_pt.last_log) / 1e9 : 0;
    g_pt.last_log = now;
    if (secs > 0) {
        uint8_t order[PT_MAX];
        unsigned m = 0;
        for (unsigned i = 0; i < PT_MAX; i++) {
            if (g_pt.sum[i].n) {
                order[m++] = (uint8_t)i;
            }
        }
        for (unsigned i = 1; i < m; i++) {      /* by GPU time, the most first */
            for (unsigned j = i; j > 0 && g_pt.sum[order[j]].ns > g_pt.sum[order[j - 1]].ns; j--) {
                uint8_t x = order[j];
                order[j] = order[j - 1];
                order[j - 1] = x;
            }
        }
        for (unsigned i = 0; i < m && i < 16; i++) {
            const typeof(g_pt.sum[0]) *s = &g_pt.sum[order[i]];
            const PtPass *l = &s->last;
            hle_log("hakuX-hle-prof", "pass #%u: %.1f ms/s (%.2f ms each, %.0f/s) %ux%u fmt %d%s%s"
                    " | draws %.0f verts %.0f fb-fetch %.0f pipelines %.0f each",
                    order[i], s->ns / 1e6 / secs, s->ns / 1e6 / s->n, s->n / secs, l->w, l->h,
                    l->fmt, l->ds ? " +depth" : "", l->clear ? " cleared" : "",
                    (double)s->draws / s->n, (double)s->verts / s->n, (double)s->fb / s->n,
                    (double)s->pipes / s->n);
        }
        static uint64_t prev[PE_N];
        char line[256];
        int len = snprintf(line, sizeof(line), "passes ended per s, by why:");
        for (unsigned i = 0; i < PE_N && len > 0 && (size_t)len < sizeof(line); i++) {
            if (g_pe[i] != prev[i]) {
                len += snprintf(line + len, sizeof(line) - (size_t)len, " %s %.0f", k_pe_names[i],
                                (g_pe[i] - prev[i]) / secs);
            }
        }
        memcpy(prev, g_pe, sizeof(prev));
        hle_log("hakuX-hle-prof", "%s", line);
    }
    memset(g_pt.sum, 0, sizeof(g_pt.sum));
}

static VkCommandBuffer cb_get(void)
{
    HleFrame *f = &g_back.fr[g_back.cur];
    if (!g_back.recording) {
        if (f->value > hle_tl_completed()) {
            hle_tl_wait(f->value);
        }
        gput_collect(g_back.cur);
        pt_collect(g_back.cur);
        T.vkResetCommandPool(g_hvk.dev, f->pool, 0);
        VkCommandBufferBeginInfo bi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        T.vkBeginCommandBuffer(f->cb, &bi);
        hle_batch_slot(g_back.cur);     /* this slot's indices: its last use has completed */
        hle_draw_slot(g_back.cur);      /* and its push blocks and fbfetch sets */
        {   /* Earlier submissions' writes before this one's commands: a frame
             * split by a kick resumes its pass here and loads the attachments
             * the last submission stored. Submission order orders execution,
             * not memory. */
            VkMemoryBarrier2 mb = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT };
            VkDependencyInfo di = {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
            T.vkCmdPipelineBarrier2(f->cb, &di);
        }
        hle_draw_rec_begin(f->cb);
        hle_draw_state_lost();
        gput_begin(f->cb, g_back.cur);
        pt_slot_begin(f->cb, g_back.cur);
        g_back.recording = true;
        hle_surf_rec_begin();
    }
    return f->cb;
}

/* The current target's clear value: an 8-bit (B8) target keeps blue, the
 * NV2A channel it stores (the shaders write .bgra to it as well). */
static VkClearColorValue color_of(uint32_t argb)
{
    float r = ((argb >> 16) & 0xFF) / 255.0f, b = (argb & 0xFF) / 255.0f;
    HleVkSurf *rt = hle_surf_get(g_back.rt);
    bool bgra = rt && hle_surf_out_bgra(rt->fmt);
    VkClearColorValue c = { .float32 = { bgra ? b : r, ((argb >> 8) & 0xFF) / 255.0f,
                                         bgra ? r : b, (argb >> 24) / 255.0f } };
    return c;
}

/* Work recorded between draws other than their state (pass boundaries,
 * clears, uploads, queries, swaps): two draws with one in between can't be
 * one (the draw merge count, hle_draw_ff_diag). */
uint64_t hle_back_breaks;

/* lrz_off: the pass's first draw runs without the driver's LRZ
 * (hle_draw_lrz_off; true when not known) */
static bool open_pass(bool lrz_off)
{
    hle_back_breaks++;
    HleVkSurf *rt = hle_surf_get(g_back.rt), *ds = hle_surf_get(g_back.ds);
    if (rt && !hle_surf_ensure(rt)) {
        rt = NULL;
    }
    if (ds && !hle_surf_ensure(ds)) {
        ds = NULL;
    }
    if (!rt && !ds) {
        g_back.pend_clear = false;
        return false;
    }
    VkCommandBuffer cb = cb_get();
    uint32_t pf = g_back.pend_clear ? g_back.pend_flags : 0;
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo da = ca, sa = ca;
    uint32_t w = rt ? rt->w : ds->w, h = rt ? rt->h : ds->h;
    if (rt) {
        /* nothing written yet and no clear: zeros, not what an attachment
         * compressor left (quirks.h) */
        const bool cfresh = rt->layout == VK_IMAGE_LAYOUT_UNDEFINED && !(pf & HLE_CLEAR_TARGET) &&
                            hle_quirk(HLE_QUIRK_FRESH_CLEAR);
        hle_surf_transition(cb, rt, rt->local_read ? VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR
                                                   : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        hle_surf_written(rt);           /* HLE_OP_ZWRITE compares it with a depth buffer's */
        ca.imageView = rt->view;
        ca.imageLayout = rt->layout;    /* GENERAL for texture levels / faces */
        ca.loadOp = (pf & HLE_CLEAR_TARGET) || cfresh ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                      : VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color = cfresh ? (VkClearColorValue){ .float32 = { 0 } } : g_back.pend_color;
    }
    VkImageAspectFlags in_pass_clear = 0;   /* HLE_QUIRK_DS_SAME_LOAD */
    bool stencil = ds && (ds->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    if (ds) {
        w = MIN(w, ds->w);
        h = MIN(h, ds->h);
        /* A depth buffer's first pass with nothing written to it yet starts
         * at the far end (stencil 0), not the image's undefined contents:
         * what the title clears or draws lands over that, the rest reads as
         * nothing in front. PGR2 clears its 1024-wide car shadow map 996
         * texels wide; the columns past it read 0 here, closer than the road,
         * and a bar of shadow crossed the road beside the car (D3D mode's
         * Clear covers the whole surface). */
        const bool fresh = ds->layout == VK_IMAGE_LAYOUT_UNDEFINED;
        const uint32_t title_pf = pf;       /* the title's clears: they write it */
        if (fresh) {
            if (!(pf & HLE_CLEAR_ZBUFFER)) {
                g_back.pend_z = 1.0f;
            }
            if (!(pf & HLE_CLEAR_STENCIL)) {
                g_back.pend_s = 0;
            }
            pf |= HLE_CLEAR_ZBUFFER | (stencil ? HLE_CLEAR_STENCIL : 0);
        }
        hle_surf_transition(cb, ds, ds->local_read ? VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR
                                                   : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        /* A pass clearing depth that opens on a draw without LRZ: an empty
         * pass of one pixel clears it first, and its end stores the driver's
         * cleared LRZ state, which the pass falls back to (quirks.h). */
        if ((pf & HLE_CLEAR_ZBUFFER) && lrz_off && hle_quirk(HLE_QUIRK_LRZ_CLEAR)) {
            VkRenderingAttachmentInfo za = {
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = ds->view,
                .imageLayout = ds->layout, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .clearValue.depthStencil.depth = g_back.pend_z };
            VkRenderingInfo zi = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                   .renderArea = { { 0, 0 }, { 1, 1 } }, .layerCount = 1,
                                   .pDepthAttachment = &za };
            T.vkCmdBeginRendering(cb, &zi);
            T.vkCmdEndRendering(cb);
            hle_img_barrier(cb, ds->img, ds->aspect, ds->layout, ds->layout);
        }
        if (title_pf & (HLE_CLEAR_ZBUFFER | HLE_CLEAR_STENCIL)) {
            hle_surf_written(ds);       /* else its draws say (draw.c), when they write it */
        }
        da.imageView = ds->view;
        da.imageLayout = ds->layout;
        da.loadOp = (pf & HLE_CLEAR_ZBUFFER) ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                             : VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        da.clearValue.depthStencil.depth = g_back.pend_z;
        da.clearValue.depthStencil.stencil = g_back.pend_s;
        sa = da;
        sa.loadOp = (pf & HLE_CLEAR_STENCIL) ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                             : VK_ATTACHMENT_LOAD_OP_LOAD;
        if (stencil && da.loadOp != sa.loadOp && hle_quirk(HLE_QUIRK_DS_SAME_LOAD)) {
            /* both loaded, the cleared aspect cleared once the pass begins */
            in_pass_clear = da.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                     : VK_IMAGE_ASPECT_STENCIL_BIT;
            da.loadOp = sa.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        }
    }
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = { { 0, 0 }, { w, h } }, .layerCount = 1,
        .colorAttachmentCount = rt ? 1 : 0, .pColorAttachments = rt ? &ca : NULL,
        .pDepthAttachment = ds ? &da : NULL, .pStencilAttachment = stencil ? &sa : NULL };
    pt_pass_begin(cb, w, h, rt ? (int)rt->fmt : -1, ds != NULL, pf);
    T.vkCmdBeginRendering(cb, &ri);
    if (in_pass_clear) {
        VkClearAttachment za = { .aspectMask = in_pass_clear,
                                 .clearValue.depthStencil = { g_back.pend_z, g_back.pend_s } };
        VkClearRect zr = { .rect = ri.renderArea, .baseArrayLayer = 0, .layerCount = 1 };
        T.vkCmdClearAttachments(cb, 1, &za, 1, &zr);
    }
    g_back.pass_open = true;
    g_back.zeq_wgen = 0;
    g_back.pend_clear = false;
    return true;
}

static const char *surf_desc(uint32_t id, char *buf, size_t cap)
{
    HleVkSurf *s = hle_surf_get(id);
    if (!s) {
        snprintf(buf, cap, "%u: none", id);
    } else {
        snprintf(buf, cap, "%u: %ux%u fmt %d aspect %x parent %u img %d", id, s->d.w, s->d.h,
                 (int)s->fmt, (unsigned)s->aspect, s->d.parent, s->img != VK_NULL_HANDLE);
    }
    return buf;
}

/* A draw no pass could take (neither the target nor the depth buffer has an
 * image): dropped, counted, the first ones and then powers of two logged. */
static void draw_nopass(void)
{
    uint64_t n = ++g_back.st.nopass;
    if (n <= 8 || !(n & (n - 1))) {
        char a[96], b[96];
        hle_log("hakuX-hle-back", "draw #%llu without a pass: rt %s, ds %s",
                (unsigned long long)n, surf_desc(g_back.rt, a, sizeof(a)),
                surf_desc(g_back.ds, b, sizeof(b)));
    }
}

static void close_pass_why(unsigned why)
{
    hle_back_breaks++;
    if (!g_back.pass_open && g_back.pend_clear) {
        open_pass(false);                     /* clear-only pass: loadOp=CLEAR */
    }
    if (g_back.pass_open) {
        vis_pass_end(g_back.fr[g_back.cur].cb);
        T.vkCmdEndRendering(g_back.fr[g_back.cur].cb);
        pt_pass_end(g_back.fr[g_back.cur].cb);
        g_back.pass_open = false;
        g_back.st.passes++;
        g_pe[why]++;
    }
}
#define close_pass() close_pass_why(PE_OTHER)

/* Fence and kick boundaries close the recording lazily: back-to-back ones
 * (Riddick kicks for every visibility result it polls) share one submit,
 * made when the ring runs dry, after 16 of them or once the first has waited
 * HLE_PEND_NS (checked as commands are dispatched, render_thread). Its
 * timeline value is the latest, which completes the earlier ones too. A lone
 * fence must not wait for the next one: Blinx inserts one a frame and blocks
 * on it a frame later, so a fence held back until the ring ran dry put the
 * guest, the recording and the GPU in series. */
#define HLE_PEND_NS 1000000
static struct {
    uint64_t value;
    unsigned n;
    int64_t since;
    uint64_t merged;
    /* a guest blocked on the timeline (hle_back_urgent): a pending boundary
     * goes out at once, not after the ring has stayed dry (ring_wait_brief).
     * Conker's level loads fence and block ~2100 times in a row: the brief
     * wait cost each round trip 200 us. */
    int urgent;
} g_pend;
/* debug, live: debug.hakux.pendus (the age in us; empty: HLE_PEND_NS) and
 * debug.hakux.dryhold=1 (a dry ring keeps boundaries until they age or a
 * waiter hurries them: fewer submits, each of which ends the open pass) */
static int64_t g_pend_ns = HLE_PEND_NS;
static bool g_dry_hold;

static void submit(uint64_t value);

static void pend_flush(void)
{
    hle_atomic_set(&g_pend.urgent, 0);
    if (g_pend.n) {
        g_pend.merged += g_pend.n - 1;
        g_pend.n = 0;
        submit(g_pend.value);
    }
}

static void pend_boundary(uint64_t value)
{
    int64_t now = hle_now_ns();
    if (!g_pend.n) {
        g_pend.since = now;
    }
    g_pend.value = value;
    if (++g_pend.n >= 16 || now - g_pend.since >= g_pend_ns ||
        hle_atomic_read(&g_pend.urgent)) {
        pend_flush();
    }
}

/* between commands: a boundary waiting longer than HLE_PEND_NS, or one a
 * progress waiter hurries (or made urgent), is submitted */
static inline void pend_age_check(void)
{
    if (unlikely(g_pend.n) && (hle_now_ns() - g_pend.since >= g_pend_ns ||
                               hle_atomic_read(&g_pend.urgent))) {
        pend_flush();
    }
}

static void submit(uint64_t value)
{
    if (g_pend.n && value >= g_pend.value) {
        g_pend.merged += g_pend.n;      /* this submit signals past them */
        g_pend.n = 0;
    }
    close_pass_why(PE_SUBMIT);
    HleFrame *f = &g_back.fr[g_back.cur];
    VkCommandBufferSubmitInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = f->cb };
    VkSemaphoreSubmitInfo sig = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = hle_tl_sem(),
        .value = value, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
    bool has_cb = g_back.recording;
    if (has_cb) {
        gput_end(f->cb, g_back.cur);
        T.vkEndCommandBuffer(f->cb);
    }
    VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = has_cb ? 1 : 0, .pCommandBufferInfos = &cbi,
        .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &sig };
    hle_tl_gap_serve();
    int64_t ts = hle_prof_begin();
    uint32_t pa = hle_hitch_enter(HLE_ACT_SUBMIT);
    hle_vk_queue_lock();
    VkResult r = T.vkQueueSubmit2(g_hvk.queue, 1, &si, VK_NULL_HANDLE);
    hle_vk_queue_unlock();
    hle_hitch_leave(pa);
    hle_prof_end(HLE_PROF_SUBMIT, ts);
    if (r != VK_SUCCESS) {
        /* value would never signal: every wait for it would hang */
        hle_vk_lose(r, "vkQueueSubmit2");
    }
    if (has_cb) {
        f->value = value;
        g_back.cur = (g_back.cur + 1) % HLE_FRAMES;
        g_back.recording = false;
        hle_surf_rec_end();
    }
    hle_surf_stamp_parked(value);       /* parked destroys wait for this submit */
    vis_stamp(value);
    hle_draw_tex_stamp(value);
    hle_tl_signaled(value);
    g_back.st.submits++;
}

/* The decoder's frame pace. The guest's push ring held about a second of
 * commands (Sega GT 2002's races measured: its flip reached the display
 * 1030-1130 ms later, ~25 frames — the hitch monitor's "slot written N ms
 * after the swap | swap -1000"), so the simulation ran that far ahead of
 * the display and the pad was read by a game state the screen showed a
 * second later. At each flip, the previous flip's work is waited for: one
 * frame in flight, and the guest itself throttles to the render pace
 * (measured: swap -> display 19 ms, faults 0, the same frame rate).
 *
 * Two halves, because the flip is processed mid-packet: kelvin's call only
 * notes the request (a submit there split a draw from its own pipeline and
 * descriptor binds across two command buffers, and a later batch draw ran
 * on a buffer without them — Mali r34p0 dereferenced freed state and
 * died); hle_back_pace_serve runs the wait and the submit at the decode
 * loop's packet boundary.
 *
 * ON by default. The submit is at a packet boundary, as every submit
 * before the pace was; a pre-existing FEX page-fault boot crash (the
 * guest's write into an armed ring page SEGVs in the JIT, ~1 boot in 10
 * here) is unaffected by it and still open. debug.hakux.pace = 0 turns it
 * off. */
static struct {
    int req;                            /* kelvin noted a flip */
    bool on;
    uint64_t prev;                      /* the previous flip's submitted value */
} g_pace;

void hle_back_frame_pace(void)
{
    /* the app's "Frame pacing" setting; the prop overrides when set (the
     * trigger API's first poll is a baseline, so the value is read each
     * flip instead) */
    char v[PROP_VALUE_MAX];
    hle_prop_get("debug.hakux.pace", v);
    g_pace.on = v[0] ? v[0] != '0' : hle_host_frame_pace();
    if (g_pace.on) {
        __atomic_store_n(&g_pace.req, 1, __ATOMIC_RELEASE);
    }
}

/* The decode loop, at a packet boundary. */
void hle_back_pace_serve(void)
{
    if (!__atomic_exchange_n(&g_pace.req, 0, __ATOMIC_ACQ_REL)) {
        return;
    }
    if (g_pace.prev && g_pace.prev > hle_tl_completed()) {
        hle_tl_wait(g_pace.prev);
    }
    pend_flush();                       /* this flip's work so far is submitted */
    g_pace.prev = g_pend.value;
}

/* Ends and submits the open recording without a timeline value (values stay
 * owned by the front end), then waits for the queue. Used before destroying
 * images the recording may reference (DEVICE_RESET/DESTROY). */
static void flush_idle(void)
{
    close_pass();
    if (g_back.recording) {
        HleFrame *f = &g_back.fr[g_back.cur];
        gput_end(f->cb, g_back.cur);
        T.vkEndCommandBuffer(f->cb);
        VkCommandBufferSubmitInfo cbi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = f->cb };
        VkSubmitInfo2 si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi };
        int64_t ts = hle_prof_begin();
        uint32_t pa = hle_hitch_enter(HLE_ACT_SUBMIT);
        hle_vk_queue_lock();
        VkResult r = T.vkQueueSubmit2(g_hvk.queue, 1, &si, VK_NULL_HANDLE);
        hle_vk_queue_unlock();
        hle_hitch_leave(pa);
        hle_prof_end(HLE_PROF_SUBMIT, ts);
        if (r != VK_SUCCESS) {
            hle_vk_lose(r, "vkQueueSubmit2 (flush)");
        }
        f->value = 0;                   /* queue idle below: nothing to wait for */
        g_back.cur = (g_back.cur + 1) % HLE_FRAMES;
        g_back.recording = false;
        hle_surf_rec_end();
    }
    uint32_t pa = hle_hitch_enter(HLE_ACT_GPU);
    hle_vk_queue_lock();
    const VkResult r = T.vkQueueWaitIdle(g_hvk.queue);
    hle_vk_queue_unlock();
    hle_hitch_leave(pa);
    hle_vk_check(r, "vkQueueWaitIdle");
}

uint32_t g_hle_rscale = 1;

static void cmd_device(const HleCmdDevice *c, bool reset)
{
    uint32_t scale = MIN(MAX(hle_host_render_scale(), 1), 4);
    /* A Reset replaces only the device's surfaces (the front end destroys the
     * old ones by OBJ_DESTROY): the title's own render targets and textures
     * keep their images. A new scale resizes every image. */
    if (reset || scale != g_hle_rscale) {
        flush_idle();
    }
    if (scale != g_hle_rscale) {
        hle_surf_destroy_all();
    }
    g_hle_rscale = scale;
    for (uint32_t i = 0; i < c->nsurf && i < HLE_CMD_MAX_SURF; i++) {
        hle_surf_define(&c->surf[i]);
    }
    g_back.disp_w = c->disp_w;
    g_back.disp_h = c->disp_h;
    g_back.present_id = c->present_id;
    hle_disp_init(c->disp_w * scale, c->disp_h * scale);
    hle_log("hakuX-hle-back", "DEVICE_%s %ux%u surfaces=%u present=%u scale=%u",
            reset ? "RESET" : "CREATE", c->disp_w, c->disp_h, c->nsurf, c->present_id,
            scale);
}

/* ---- debug: render-target dump (debug.hakux.rtdump) ---------------------- */

/* A new value of the property debug.hakux.rtdump dumps the next frame: every
 * colour target bound in it and the presented surface, as raw texels in
 * <base>/hle_shadow/rt_<swap>_<id>_<w>x<h>_f<VkFormat>_b<bytes>.raw, copied at
 * the swap (so each holds what the frame left in it). Each target is also
 * copied when the frame moves off it (rt_<swap>_s<k>_<id>_...: the k-th such
 * snapshot): offscreen passes over memory the frame reuses later (Forza's
 * minimap and glow chain in the back buffer) keep their own result. A value
 * with "+id,id..." in it also dumps those surfaces (textures the frame samples,
 * by the ids of the draw log's tex=). */
static struct {
    bool next, active;
    uint32_t ids[32];
    unsigned n;
    uint32_t extra[8];
    unsigned nextra;
    uint64_t poll;
    VkBuffer buf;
    VkDeviceMemory mem;
    struct { uint32_t id, w, h, fmt, bytes; VkDeviceSize off; } e[33];
    unsigned ne;
    struct { uint32_t id, w, h, fmt, bytes; VkBuffer buf; VkDeviceMemory mem; } sn[48];
    unsigned nsn;
} g_rtd;

static bool rtd_surf_ok(HleVkSurf *s)
{
    return s && !s->d.parent && hle_surf_ensure(s) && (s->aspect & VK_IMAGE_ASPECT_COLOR_BIT) &&
           s->layout != VK_IMAGE_LAYOUT_UNDEFINED && s->texel_bytes;
}

/* a host-visible buffer of size bytes */
static bool rtd_buf(VkDeviceSize size, VkBuffer *buf, VkDeviceMemory *mem)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = MAX(size, 16u),
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkMemoryRequirements mr;
    if (T.vkCreateBuffer(g_hvk.dev, &bci, NULL, buf) != VK_SUCCESS) {
        return false;
    }
    T.vkGetBufferMemoryRequirements(g_hvk.dev, *buf, &mr);
    int mt = hle_vk_memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, mem) != VK_SUCCESS) {
        T.vkDestroyBuffer(g_hvk.dev, *buf, NULL);
        return false;
    }
    T.vkBindBufferMemory(g_hvk.dev, *buf, *mem, 0);
    return true;
}

static void rtd_copy(VkCommandBuffer cb, HleVkSurf *s, VkBuffer buf, VkDeviceSize off)
{
    hle_surf_transition(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r = { .bufferOffset = off,
                            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                            .imageExtent = { s->w, s->h, 1 } };
    T.vkCmdCopyImageToBuffer(cb, s->img, s->general ? VK_IMAGE_LAYOUT_GENERAL : s->layout,
                             buf, 1, &r);
}

/* the target the frame moves off, as it is now (outside a render pass) */
static void rtd_snap(uint32_t id)
{
    HleVkSurf *s = hle_surf_get(id);
    if (!g_rtd.active || g_rtd.nsn >= ARRAY_SIZE(g_rtd.sn) || !rtd_surf_ok(s)) {
        return;
    }
    typeof(g_rtd.sn[0]) *e = &g_rtd.sn[g_rtd.nsn];
    if (!rtd_buf((VkDeviceSize)s->w * s->h * s->texel_bytes, &e->buf, &e->mem)) {
        return;
    }
    e->id = id;
    e->w = s->w;
    e->h = s->h;
    e->fmt = s->fmt;
    e->bytes = s->texel_bytes;
    rtd_copy(cb_get(), s, e->buf, 0);
    g_rtd.nsn++;
}

static void rtd_note(uint32_t id)
{
    if (!g_rtd.active || !id) {
        return;
    }
    for (unsigned i = 0; i < g_rtd.n; i++) {
        if (g_rtd.ids[i] == id) {
            return;
        }
    }
    if (g_rtd.n < ARRAY_SIZE(g_rtd.ids)) {
        g_rtd.ids[g_rtd.n++] = id;
    }
}

/* the "+id,id..." in a dump request: surfaces dumped with the frame */
static void rtd_extras(const char *v)
{
    g_rtd.nextra = 0;
    for (const char *x = strchr(v, '+'); x && g_rtd.nextra < ARRAY_SIZE(g_rtd.extra);
         x = strchr(x, ',')) {
        uint32_t id = (uint32_t)strtoul(++x, NULL, 0);
        if (id) {
            g_rtd.extra[g_rtd.nextra++] = id;
        }
    }
}

static void rtd_poll(void)
{
#ifdef __ANDROID__
    if (g_back.st.swaps < g_rtd.poll) {
        return;
    }
    g_rtd.poll = g_back.st.swaps + 11;  /* odd: either frame parity */
    static HleProp prop;
    char v[PROP_VALUE_MAX];
    if (hle_prop_new(&prop, "debug.hakux.rtdump", v)) {
        g_rtd.next = true;
        rtd_extras(v);
    }
#endif
}

/* debug: HAKUX_HLE_RTDUMP_SWAPS=<first>[-<last>][+id,id...] dumps the frames
 * of those swaps (this count, the files' names) in each process, with those
 * surfaces: a title's start, a relaunch or a moment a property value can't
 * be timed to (it is polled every 11 swaps) */
static bool rtd_range(void)
{
    static int on = -1;
    static uint64_t first, last;
    if (on < 0) {
        const char *e = getenv("HAKUX_HLE_RTDUMP_SWAPS");
        char *end = NULL;
        first = e ? strtoull(e, &end, 10) : 0;
        last = end && *end == '-' ? strtoull(end + 1, &end, 10) : first;
        on = first > 0;
        if (on) {
            rtd_extras(e);
        }
    }
    const uint64_t n = g_back.st.swaps + 1;     /* the swap that ends the frame */
    return on && n >= first && n <= last;
}

/* At the dumped frame's swap: copies its targets into a host buffer. */
static bool rtd_record(const HleCmdSwap *c)
{
    if (!g_rtd.active) {
        return false;
    }
    rtd_note(c->surf);
    close_pass();
    VkDeviceSize total = 0;
    g_rtd.ne = 0;
    g_rtd.buf = VK_NULL_HANDLE;
    for (unsigned i = 0; i < g_rtd.n; i++) {
        HleVkSurf *s = hle_surf_get(g_rtd.ids[i]);
        if (!rtd_surf_ok(s)) {
            hle_log("hakuX-hle-back", "rtdump: %u skipped: %s", g_rtd.ids[i],
                    !s ? "no surface" : s->d.parent ? "a view" :
                    s->layout == VK_IMAGE_LAYOUT_UNDEFINED ? "never written" :
                    "not a colour image");
            continue;
        }
        g_rtd.e[g_rtd.ne++] = (typeof(g_rtd.e[0])){ g_rtd.ids[i], s->w, s->h, s->fmt,
                                                     s->texel_bytes, total };
        total += (VkDeviceSize)s->w * s->h * s->texel_bytes;
    }
    if (!g_rtd.ne || !rtd_buf(total, &g_rtd.buf, &g_rtd.mem)) {
        g_rtd.ne = 0;
        g_rtd.buf = VK_NULL_HANDLE;
        return g_rtd.nsn > 0;
    }
    VkCommandBuffer cb = cb_get();
    for (unsigned i = 0; i < g_rtd.ne; i++) {
        rtd_copy(cb, hle_surf_get(g_rtd.e[i].id), g_rtd.buf, g_rtd.e[i].off);
    }
    return true;
}

static void rtd_write(uint64_t value)
{
    hle_tl_wait(value);
    g_autofree char *dir = g_strdup_printf("%shle_shadow", hle_host_base_path());
    g_mkdir_with_parents(dir, 0700);
    for (unsigned i = 0; i < g_rtd.nsn; i++) {
        typeof(g_rtd.sn[0]) *e = &g_rtd.sn[i];
        uint8_t *m = NULL;
        if (T.vkMapMemory(g_hvk.dev, e->mem, 0, VK_WHOLE_SIZE, 0, (void **)&m) == VK_SUCCESS) {
            g_autofree char *p = g_strdup_printf("%s/rt_%llu_s%02u_%u_%ux%u_f%u_b%u.raw", dir,
                                                 (unsigned long long)g_back.st.swaps, i, e->id,
                                                 e->w, e->h, e->fmt, e->bytes);
            g_file_set_contents(p, (const char *)m, (gssize)e->w * e->h * e->bytes, NULL);
            T.vkUnmapMemory(g_hvk.dev, e->mem);
        }
        T.vkDestroyBuffer(g_hvk.dev, e->buf, NULL);
        T.vkFreeMemory(g_hvk.dev, e->mem, NULL);
    }
    if (g_rtd.nsn) {
        hle_log("hakuX-hle-back", "rtdump: %u snapshots of swap %llu", g_rtd.nsn,
                (unsigned long long)g_back.st.swaps);
    }
    g_rtd.nsn = 0;
    uint8_t *map = NULL;
    if (g_rtd.buf &&
        T.vkMapMemory(g_hvk.dev, g_rtd.mem, 0, VK_WHOLE_SIZE, 0, (void **)&map) == VK_SUCCESS) {
        for (unsigned i = 0; i < g_rtd.ne; i++) {
            typeof(g_rtd.e[0]) *e = &g_rtd.e[i];
            g_autofree char *p = g_strdup_printf("%s/rt_%llu_%u_%ux%u_f%u_b%u.raw", dir,
                                                 (unsigned long long)g_back.st.swaps, e->id,
                                                 e->w, e->h, e->fmt, e->bytes);
            g_file_set_contents(p, (const char *)map + e->off,
                                (gssize)e->w * e->h * e->bytes, NULL);
        }
        T.vkUnmapMemory(g_hvk.dev, g_rtd.mem);
        hle_log("hakuX-hle-back", "rtdump: %u targets of swap %llu in %s", g_rtd.ne,
                (unsigned long long)g_back.st.swaps, dir);
    }
    if (g_rtd.buf) {
        T.vkDestroyBuffer(g_hvk.dev, g_rtd.buf, NULL);
        T.vkFreeMemory(g_hvk.dev, g_rtd.mem, NULL);
        g_rtd.buf = VK_NULL_HANDLE;
    }
}

/* ---- debug: pixel history (debug.hakux.pixhist) ---------------------------- */

/* A new value of the property debug.hakux.pixhist, "x,y" (texels of the
 * target) or "x,y,id" (that target only), or "hx,y[,id]" (a pixel of the
 * scaled target, one of a texel's scale^2), records over the next frame the
 * texel after every draw or clear into a target that covers it: the pass is
 * ended there and the colour texel and the depth texel copied out. At the
 * swap <base>/hle_shadow/pix_<swap>.txt lists those draws (the back end's
 * draw, its program and textures, a push-buffer run's or not) with the
 * texel before and after, and the constants of the draws that changed it.
 * "...@n" anywhere in the value starts recording at the n-th such draw or
 * clear (a frame of more than PIX_MAX: Blinx draws ~17k strips a frame);
 * "k" anywhere: every program draw's constants (draws that miss the texel);
 * "*n": n frames in a row, a file each (an effect fed by the frame before). */
#define PIX_MAX 2048
/* A record's bytes in the readback buffer: the texels (colour +0, stage 0's
 * target +4, depth +8, stencil +12), then what a small arena draw read as
 * the GPU ran it: its vertices (+16, PIX_VBYTES) and constants c0..c3 */
#define PIX_STRIDE 256u
#define PIX_VBYTES 128u
typedef struct PixRec {
    uint32_t draw, rt, ds, kind;        /* kind: 0 draw, 1 clear */
    uint32_t cbytes, dbytes, cfmt, dfmt, sbytes;
    uint32_t tbytes;                    /* stage 0's target texture: its texel after the draw */
    uint32_t vbytes;                    /* a small arena draw: its vertex bytes, as recorded */
    uint8_t v[PIX_VBYTES];
    uint32_t gvbytes;                   /* a draw from guest memory: its first vertex's bytes */
    uint8_t gv[64];
    uint32_t rs[9];                     /* hle_draw_rs_brief */
    HleCmdDraw c;
    float k[192][4];                    /* the draw's constants (program draws) */
} PixRec;
static struct {
    bool next, active;
    uint32_t x, y, id;
    bool host;                          /* x, y: pixels of the (scaled) image */
    uint64_t poll;
    VkBuffer buf;
    VkDeviceMemory mem;
    PixRec *rec;
    uint32_t n;
    uint32_t skip, seen;                /* "@n": the records before the n-th left out */
    bool all_k;                         /* "k": every program draw's constants */
    uint32_t frames;                    /* "*n": frames left to record, a file each */
} g_pix;

static void pix_poll(void)
{
#ifdef __ANDROID__
    if (g_back.st.swaps < g_pix.poll) {
        return;
    }
    g_pix.poll = g_back.st.swaps + 3;    /* odd: either frame parity */
    static HleProp prop;
    char v[PROP_VALUE_MAX];
    if (hle_prop_new(&prop, "debug.hakux.pixhist", v)) {
        unsigned x = 0, y = 0, id = 0;
        bool host = v[0] == 'h';
        if (sscanf(v + host, "%u,%u,%u", &x, &y, &id) >= 2) {
            g_pix.x = x;
            g_pix.y = y;
            g_pix.host = host;
            g_pix.id = id;
            const char *at = strchr(v, '@');
            g_pix.skip = at ? (uint32_t)strtoul(at + 1, NULL, 10) : 0;
            g_pix.all_k = strchr(v, 'k') != NULL;
            const char *nf = strchr(v, '*');
            g_pix.frames = nf ? MAX((uint32_t)strtoul(nf + 1, NULL, 10), 1u) : 1;
            g_pix.next = true;
        }
    }
#endif
}

/* After a draw (c) or a clear (c NULL) of the pixel history's frame. */
static void pix_after(const HleCmdDraw *c)
{
    HleVkSurf *rt = hle_surf_get(g_back.rt), *ds = hle_surf_get(g_back.ds);
    if (!g_pix.active || !g_pix.buf || !rtd_surf_ok(rt)) {
        return;
    }
    /* the texel, and the pixel of the target's image */
    uint32_t sc = rt->scale, gx = g_pix.host ? g_pix.x / sc : g_pix.x;
    uint32_t gy = g_pix.host ? g_pix.y / sc : g_pix.y;
    uint32_t px = g_pix.host ? g_pix.x : g_pix.x * sc, py = g_pix.host ? g_pix.y : g_pix.y * sc;
    if ((g_pix.id && g_back.rt != g_pix.id) || gx >= rt->d.w || gy >= rt->d.h) {
        return;
    }
    if (g_pix.seen++ < g_pix.skip || g_pix.n >= PIX_MAX) {
        return;
    }
    close_pass();
    VkCommandBuffer cb = cb_get();
    PixRec *r = &g_pix.rec[g_pix.n];
    memset(r, 0, sizeof(*r));
    r->draw = (uint32_t)g_back.st.draws;
    r->rt = g_back.rt;
    r->ds = g_back.ds;
    r->kind = c ? 0 : 1;
    r->cbytes = rt->texel_bytes;
    r->cfmt = rt->fmt;
    hle_surf_transition(cb, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy cr = { .bufferOffset = PIX_STRIDE * g_pix.n,
                             .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                             .imageOffset = { (int32_t)px, (int32_t)py, 0 },
                             .imageExtent = { 1, 1, 1 } };
    T.vkCmdCopyImageToBuffer(cb, rt->img, rt->general ? VK_IMAGE_LAYOUT_GENERAL : rt->layout,
                             g_pix.buf, 1, &cr);
    if (ds && hle_surf_ensure(ds) && (ds->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) &&
        px < ds->w && py < ds->h) {
        r->dbytes = ds->texel_bytes > 4 ? 4 : ds->texel_bytes;
        r->dfmt = ds->fmt;
        hle_surf_transition(cb, ds, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy dr = cr;
        dr.bufferOffset += 8;
        dr.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        T.vkCmdCopyImageToBuffer(cb, ds->img, ds->general ? VK_IMAGE_LAYOUT_GENERAL : ds->layout,
                                 g_pix.buf, 1, &dr);
        if (ds->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {     /* one byte at +12 */
            r->sbytes = 1;
            VkBufferImageCopy sr = cr;
            sr.bufferOffset += 12;
            sr.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            T.vkCmdCopyImageToBuffer(cb, ds->img,
                                     ds->general ? VK_IMAGE_LAYOUT_GENERAL : ds->layout,
                                     g_pix.buf, 1, &sr);
        }
    }
    /* a target sampled at stage 0 (a copy of a frame): its texel, at +4 */
    HleVkSurf *t0 = c && c->tex[0] && c->tex[0] != g_back.rt ? hle_surf_get(c->tex[0]) : NULL;
    if (rtd_surf_ok(t0) && t0->texel_bytes == 4 && px < t0->w && py < t0->h) {
        r->tbytes = 4;
        hle_surf_transition(cb, t0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy tr = cr;
        tr.bufferOffset += 4;
        T.vkCmdCopyImageToBuffer(cb, t0->img, t0->general ? VK_IMAGE_LAYOUT_GENERAL : t0->layout,
                                 g_pix.buf, 1, &tr);
    }
    /* a few vertices from the arena (a full-screen quad): the bytes now and,
     * copied by the GPU after the draw, the bytes it read; c0..c3 the same */
    if (c && c->nbind && !(c->guest_vb & 1) && c->bind_stride[0] &&
        c->count * c->bind_stride[0] <= PIX_VBYTES) {
        r->vbytes = c->count * c->bind_stride[0];
        memcpy(r->v, g_harena.map + c->bind_off[0], r->vbytes);
        VkBufferCopy bc[2] = {
            { c->bind_off[0], PIX_STRIDE * g_pix.n + 16, r->vbytes },
            { c->consts_off, PIX_STRIDE * g_pix.n + 16 + PIX_VBYTES, 64 } };
        T.vkCmdCopyBuffer(cb, g_harena.buf, g_pix.buf, c->vsh ? 2 : 1, bc);
    }
    /* a draw reading guest memory: the bytes of the vertex its first index names */
    if (c && c->nbind && (c->guest_vb & 1) && c->bind_stride[0]) {
        uint32_t ix = c->ib_off != 0xFFFFFFFFu ? *(const uint16_t *)(g_harena.map + c->ib_off) : 0;
        uint32_t len = MIN(c->bind_stride[0], (uint32_t)sizeof(r->gv));
        const uint8_t *gp = hle_pa_ptr(c->bind_off[0] + ix * c->bind_stride[0], len);
        if (gp) {
            memcpy(r->gv, gp, len);
            r->gvbytes = len;
        }
    }
    if (c) {
        r->c = *c;
        hle_draw_rs_brief(r->rs);
        if (c->vsh) {
            memcpy(r->k, g_harena.map + c->consts_off, sizeof(r->k));
        }
    }
    g_pix.n++;
}

static uint32_t pix_texel(const uint8_t *m, uint32_t bytes)
{
    return bytes == 2 ? hle_lduw_le(m) : bytes == 4 ? hle_ldl_le(m) : m[0];
}

/* At the history frame's swap (submitted): the file. */
static void pix_write(uint64_t value)
{
    hle_tl_wait(value);
    uint8_t *m = NULL;
    if (!g_pix.n || T.vkMapMemory(g_hvk.dev, g_pix.mem, 0, VK_WHOLE_SIZE, 0,
                                  (void **)&m) != VK_SUCCESS) {
        return;
    }
    g_autofree char *dir = g_strdup_printf("%shle_shadow", hle_host_base_path());
    g_mkdir_with_parents(dir, 0700);
    g_autofree char *p = g_strdup_printf("%s/pix_%llu.txt", dir,
                                         (unsigned long long)g_back.st.swaps);
    FILE *f = fopen(p, "w");
    unsigned changes = 0;
    if (f) {
        fprintf(f, "pixel %s%u,%u target %u, swap %llu, %u records from the %u-th (of %u)\n",
                g_pix.host ? "h" : "", g_pix.x, g_pix.y, g_pix.id, (unsigned long long)g_back.st.swaps,
                g_pix.n, g_pix.skip, g_pix.seen);
        uint32_t last_rt[16] = { 0 }, last_c[16] = { 0 }, last_d[16] = { 0 };
        uint32_t last_st[16] = { 0 };
        for (uint32_t i = 0; i < g_pix.n; i++) {
            const PixRec *r = &g_pix.rec[i];
            uint32_t col = pix_texel(m + PIX_STRIDE * i, r->cbytes);
            uint32_t dep = r->dbytes ? pix_texel(m + PIX_STRIDE * i + 8, r->dbytes) : 0;
            uint32_t stn = r->sbytes ? m[PIX_STRIDE * i + 12] : 0;
            unsigned s = r->rt % 16;
            bool known = last_rt[s] == r->rt;
            bool changed = !known || col != last_c[s] || dep != last_d[s] || stn != last_st[s];
            fprintf(f, "%s draw %u rt %u ds %u %s: colour %08x -> %08x depth %08x -> %08x "
                    "stencil %02x -> %02x",
                    changed ? "*" : " ", r->draw, r->rt, r->ds, r->kind ? "CLEAR" : "",
                    known ? last_c[s] : 0, col, known ? last_d[s] : 0, dep,
                    known ? last_st[s] : 0, stn);
            if (!r->kind) {
                fprintf(f, " | vsh %016llx%s count %u prim %u fvf %x tex %u,%u,%u,%u "
                        "nbind %u", (unsigned long long)r->c.vsh, r->c.dbg ? " RUN" : "",
                        r->c.count, r->c.prim, r->c.fvf, r->c.tex[0], r->c.tex[1], r->c.tex[2],
                        r->c.tex[3], r->c.nbind);
                for (uint32_t b = 0; b < r->c.nbind && b < HLE_DRAW_MAX_BIND; b++) {
                    /* g: guest PA read in place, else an arena copy */
                    fprintf(f, " %c%08x/%u", (r->c.guest_vb >> b) & 1 ? 'g' : 'a',
                            r->c.bind_off[b], r->c.bind_stride[b]);
                }
                fprintf(f, " attr0 %08x ib %08x atest %u/%x/%02x blend %u %x/%08x z %x/%x cw %x",
                        r->c.attr[0], r->c.ib_off, r->rs[0], r->rs[1], r->rs[2] & 0xFF, r->rs[3],
                        r->rs[4], r->rs[8], r->rs[5], r->rs[6], r->rs[7]);
                fprintf(f, " attrs");       /* vN=format byte | binding << 8 | offset << 16 */
                for (unsigned a = 0; a < HLE_DRAW_ATTRS; a++) {
                    if (r->c.attr[a] & 0xF0) {
                        fprintf(f, " v%u=%x", a, r->c.attr[a]);
                    }
                }
                if (r->gvbytes) {
                    fprintf(f, " vertex");
                    for (uint32_t o = 0; o + 4 <= r->gvbytes; o += 4) {
                        fprintf(f, " %08x", hle_ldl_le(r->gv + o));
                    }
                }
                if (r->tbytes) {
                    fprintf(f, " t0 texel %08x", pix_texel(m + PIX_STRIDE * i + 4, r->tbytes));
                }
                if (r->vbytes) {        /* positions (the first 4 floats of each vertex) */
                    const uint8_t *gv = m + PIX_STRIDE * i + 16;
                    uint32_t stride = r->c.bind_stride[0];
                    fprintf(f, " verts%s", memcmp(gv, r->v, r->vbytes) ? " CHANGED" : "");
                    for (uint32_t o = 0; o + 16 <= r->vbytes; o += stride) {
                        float p[4];
                        memcpy(p, gv + o, 16);
                        fprintf(f, " (%.1f %.1f %.3g %.3g)", p[0], p[1], p[2], p[3]);
                    }
                    if (r->c.vsh && memcmp(gv + PIX_VBYTES, r->k, 64)) {
                        fprintf(f, " c0-3 CHANGED");
                    }
                }
                if (r->c.vsh) {         /* c0..c3: the usual transform rows */
                    fprintf(f, " c0-3");
                    for (unsigned k = 0; k < 4; k++) {
                        fprintf(f, " [%.4g %.4g %.4g %.4g]", r->k[k][0], r->k[k][1],
                                r->k[k][2], r->k[k][3]);
                    }
                }
            }
            fprintf(f, "\n");
            if (((changed && known) || g_pix.all_k) && !r->kind && r->c.vsh) {
                changes++;
                for (unsigned k = 0; k < 192; k++) {
                    const float *v = r->k[k];
                    if (v[0] || v[1] || v[2] || v[3]) {
                        fprintf(f, "    c%u (%d) = %g %g %g %g\n", k, (int)k - 96, v[0], v[1],
                                v[2], v[3]);
                    }
                }
            }
            last_rt[s] = r->rt;
            last_c[s] = col;
            last_d[s] = dep;
            last_st[s] = stn;
        }
        fclose(f);
    }
    T.vkUnmapMemory(g_hvk.dev, g_pix.mem);
    hle_log("hakuX-hle-back", "pixhist: %u records, %u program draws changed %u,%u, in %s",
            g_pix.n, changes, g_pix.x, g_pix.y, p);
}

static void cmd_set_rt(const HleCmdSetRt *c)
{
    rtd_note(c->rt);
    if (c->rt != g_back.rt || c->ds != g_back.ds) {
        close_pass_why(PE_SETRT);
        if (c->rt != g_back.rt) {
            rtd_snap(g_back.rt);
        }
        g_back.rt = c->rt;
        g_back.ds = c->ds;
    }
}

/* ---- channel-masked colour clears ------------------------------------------- */

/* Vulkan clears every channel of an attachment; a subset of D3DCLEAR_TARGET_R/
 * G/B/A (Forza: alpha with Z and stencil, 0x83, keeping the colour under its
 * car-select backdrop) is a full-target triangle writing the clear colour
 * through the channel mask, depth and stencil untouched. One pipeline per
 * (attachment formats, mask), made on first use. */
static const char k_mc_vs[] =
    "#version 450\n"
    "void main() {\n"
    "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
static const char k_mc_fs[] =
    "#version 450\n"
    "layout(push_constant) uniform P { vec4 c; } pc;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = pc.c; }\n";

static struct {
    bool failed;
    VkPipelineLayout pl;
    VkShaderModule vs, fs;
    struct { VkFormat cf, df; bool st; uint8_t mask; VkPipeline p; } p[16];
    unsigned np;
} g_mc;

static VkShaderModule mc_module(HleShaderStage st, const char *src)
{
    size_t n = 0;
    uint32_t *spv = hle_glsl_compile(st, src, &n);
    VkShaderModule m = VK_NULL_HANDLE;
    if (spv) {
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                        .codeSize = n * 4, .pCode = spv };
        T.vkCreateShaderModule(g_hvk.dev, &ci, NULL, &m);
        g_free(spv);
    }
    return m;
}

static VkPipeline mc_pipe(VkFormat cf, VkFormat df, bool st, uint8_t mask)
{
    for (unsigned i = 0; i < g_mc.np; i++) {
        if (g_mc.p[i].cf == cf && g_mc.p[i].df == df && g_mc.p[i].st == st &&
            g_mc.p[i].mask == mask) {
            return g_mc.p[i].p;
        }
    }
    if (g_mc.failed || g_mc.np == ARRAY_SIZE(g_mc.p)) {
        return VK_NULL_HANDLE;
    }
    if (!g_mc.pl) {
        VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo pli = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
        g_mc.vs = mc_module(HLE_SHADER_VERTEX, k_mc_vs);
        g_mc.fs = mc_module(HLE_SHADER_FRAGMENT, k_mc_fs);
        if (!g_mc.vs || !g_mc.fs ||
            T.vkCreatePipelineLayout(g_hvk.dev, &pli, NULL, &g_mc.pl) != VK_SUCCESS) {
            g_mc.failed = true;
            hle_log("hakuX-hle-back", "masked clear: setup failed (cleared in full)");
            return VK_NULL_HANDLE;
        }
    }
    VkPipelineShaderStageCreateInfo sti[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_mc.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g_mc.fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo dsi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = mask };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkPipelineRenderingCreateInfo ri = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &cf,
        .depthAttachmentFormat = df, .stencilAttachmentFormat = st ? df : VK_FORMAT_UNDEFINED };
    VkGraphicsPipelineCreateInfo gci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
        .stageCount = 2, .pStages = sti, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &dsi, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = g_mc.pl };
    VkPipeline p = VK_NULL_HANDLE;
    if (T.vkCreateGraphicsPipelines(g_hvk.dev, VK_NULL_HANDLE, 1, &gci, NULL, &p) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    g_mc.p[g_mc.np++] = (typeof(g_mc.p[0])){ cf, df, st, mask, p };
    return p;
}

/* Xbox D3DCLEAR_TARGET_R/G/B/A (0x10/0x20/0x40/0x80) as a Vulkan write mask
 * on the current target (R and B swap on an 8-bit B8 target, color_of). */
static uint8_t clear_mask(uint32_t flags, const HleVkSurf *rt)
{
    bool bgra = hle_surf_out_bgra(rt->fmt);
    uint8_t m = 0;
    m |= (flags & 0x10) ? (bgra ? VK_COLOR_COMPONENT_B_BIT : VK_COLOR_COMPONENT_R_BIT) : 0;
    m |= (flags & 0x20) ? VK_COLOR_COMPONENT_G_BIT : 0;
    m |= (flags & 0x40) ? (bgra ? VK_COLOR_COMPONENT_R_BIT : VK_COLOR_COMPONENT_B_BIT) : 0;
    m |= (flags & 0x80) ? VK_COLOR_COMPONENT_A_BIT : 0;
    return m;
}

/* The masked part of a clear, in the open pass: false = not drawn. */
static bool clear_masked(const HleCmdClear *c, HleVkSurf *rt, HleVkSurf *ds, uint32_t flags)
{
    bool st = ds && (ds->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    VkPipeline p = mc_pipe(rt->fmt, ds ? ds->fmt : VK_FORMAT_UNDEFINED, st,
                           clear_mask(flags, rt));
    if (!p) {
        return false;
    }
    VkCommandBuffer cb = cb_get();
    uint32_t w = rt->w, h = rt->h, sc = rt->scale;
    if (ds) {
        w = MIN(w, ds->w);
        h = MIN(h, ds->h);
    }
    VkClearColorValue cv = color_of(c->color);
    T.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    hle_draw_state_lost();
    T.vkCmdPushConstants(cb, g_mc.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, cv.float32);
    VkViewport v = { 0, 0, (float)w, (float)h, 0.0f, 1.0f };
    T.vkCmdSetViewport(cb, 0, 1, &v);
    for (uint32_t i = 0; i < MAX(c->nrects, 1u) && i < HLE_CMD_MAX_RECTS; i++) {
        VkRect2D r = { { 0, 0 }, { w, h } };
        if (c->nrects) {
            int32_t x1 = MAX(c->rect[i].x1 * (int32_t)sc, 0);
            int32_t y1 = MAX(c->rect[i].y1 * (int32_t)sc, 0);
            int32_t x2 = MIN(c->rect[i].x2 * (int32_t)sc, (int32_t)w);
            int32_t y2 = MIN(c->rect[i].y2 * (int32_t)sc, (int32_t)h);
            if (x2 <= x1 || y2 <= y1) {
                continue;
            }
            r = (VkRect2D){ { x1, y1 }, { (uint32_t)(x2 - x1), (uint32_t)(y2 - y1) } };
        }
        T.vkCmdSetScissor(cb, 0, 1, &r);
        T.vkCmdDraw(cb, 3, 1, 0, 0);
    }
    return true;
}

static void cmd_clear(const HleCmdClear *c)
{
    HleVkSurf *rt = hle_surf_get(g_back.rt), *ds = hle_surf_get(g_back.ds);
    uint32_t flags = c->flags;
    if (!rt) {
        flags &= ~HLE_CLEAR_TARGET;
    }
    if (!ds) {
        flags &= ~(HLE_CLEAR_ZBUFFER | HLE_CLEAR_STENCIL);
    } else if (!(ds->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
        flags &= ~HLE_CLEAR_STENCIL;
    }
    if (!flags) {
        return;
    }
    if ((flags & HLE_CLEAR_TARGET) && (flags & HLE_CLEAR_TARGET) != HLE_CLEAR_TARGET) {
        /* a channel subset: drawn through the write mask, the rest as usual */
        uint32_t rest = flags & ~HLE_CLEAR_TARGET;
        if (!g_back.pass_open && !open_pass(true)) {
            return;
        }
        if (clear_masked(c, rt, ds, flags)) {
            if (!g_back.warned_partial) {
                g_back.warned_partial = true;
                hle_log("hakuX-hle-back", "partial channel clear 0x%x: masked", flags);
            }
            if (!rest) {
                g_back.st.clears++;
                return;
            }
            flags = rest;
        } else if (!g_back.warned_partial) {
            g_back.warned_partial = true;
            hle_log("hakuX-hle-back", "partial channel clear 0x%x treated as full", flags);
        }
    }
    g_back.st.clears++;
    static uint32_t s_logged_color;
    static unsigned s_changes;
    bool changed = (flags & HLE_CLEAR_TARGET) && c->color != s_logged_color;
    if (g_back.st.clears <= 16 || (changed && s_changes < 256)) {
        hle_log("hakuX-hle-back", "clear #%llu flags=0x%x color=%08x z=%.3f rects=%u rt=%u%s",
                (unsigned long long)g_back.st.clears, flags, c->color, c->z, c->nrects,
                g_back.rt, g_back.st.clears > 16 ? " (changed)" : "");
        s_changes += g_back.st.clears > 16;
        s_logged_color = c->color;
    }
    if ((flags & HLE_CLEAR_TARGET) && g_back.rt == g_back.present_id) {
        /* M2 evidence: the colour the presented surface was last cleared to */
        g_back.st.last_color = c->color;
        g_back.st.last_rt = g_back.rt;
    }
    if (c->nrects == 0 && !g_back.pass_open) {
        /* §5.10: a Clear before the first draw becomes loadOp=CLEAR */
        g_back.pend_flags = (g_back.pend_clear ? g_back.pend_flags : 0) | flags;
        if (flags & HLE_CLEAR_TARGET) {
            g_back.pend_color = color_of(c->color);
        }
        if (flags & HLE_CLEAR_ZBUFFER) {
            g_back.pend_z = c->z;
        }
        if (flags & HLE_CLEAR_STENCIL) {
            g_back.pend_s = c->stencil;
        }
        g_back.pend_clear = true;
        return;
    }
    if (!g_back.pass_open && !open_pass(true)) {
        return;
    }
    VkClearAttachment at[2];
    uint32_t na = 0;
    if (flags & HLE_CLEAR_TARGET) {
        at[na++] = (VkClearAttachment){ .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                        .colorAttachment = 0,
                                        .clearValue.color = color_of(c->color) };
    }
    if (flags & (HLE_CLEAR_ZBUFFER | HLE_CLEAR_STENCIL)) {
        at[na] = (VkClearAttachment){ 0 };
        at[na].aspectMask = ((flags & HLE_CLEAR_ZBUFFER) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                            ((flags & HLE_CLEAR_STENCIL) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        at[na].clearValue.depthStencil.depth = c->z;
        at[na].clearValue.depthStencil.stencil = c->stencil;
        na++;
    }
    uint32_t w = rt ? rt->w : ds->w, h = rt ? rt->h : ds->h;
    int32_t sc = (int32_t)(rt ? rt->scale : ds->scale);
    VkClearRect rects[HLE_CMD_MAX_RECTS];
    uint32_t nr = 0;
    if (c->nrects == 0) {
        rects[nr++] = (VkClearRect){ { { 0, 0 }, { w, h } }, 0, 1 };
    }
    for (uint32_t i = 0; i < c->nrects && i < HLE_CMD_MAX_RECTS; i++) {
        int32_t x1 = MAX(c->rect[i].x1 * sc, 0), y1 = MAX(c->rect[i].y1 * sc, 0);
        int32_t x2 = MIN(c->rect[i].x2 * sc, (int32_t)w);
        int32_t y2 = MIN(c->rect[i].y2 * sc, (int32_t)h);
        if (x2 > x1 && y2 > y1) {
            rects[nr++] = (VkClearRect){ { { x1, y1 },
                                           { (uint32_t)(x2 - x1), (uint32_t)(y2 - y1) } },
                                         0, 1 };
        }
    }
    if (nr) {
        T.vkCmdClearAttachments(g_back.fr[g_back.cur].cb, na, at, nr, rects);
        if (ds && (flags & (HLE_CLEAR_ZBUFFER | HLE_CLEAR_STENCIL))) {
            hle_surf_written(ds);
        }
    }
}

/* The display's gamma ramp (SetGammaRamp). Identity needs nothing. */
static struct {
    uint8_t rgb[3][256];
    bool identity;
    int off;                            /* debug: HAKUX_HLE_NOGAMMA, ramps ignored */
} g_gamma = { .identity = true, .off = -1 };

static void cmd_gamma(const HleCmdGamma *c)
{
    if (g_gamma.off < 0) {
        g_gamma.off = getenv("HAKUX_HLE_NOGAMMA") != NULL;
    }
    memcpy(g_gamma.rgb, c->rgb, sizeof(g_gamma.rgb));
    bool ident = true;
    for (unsigned ch = 0; ch < 3 && ident; ch++) {
        for (unsigned i = 0; i < 256 && ident; i++) {
            ident = c->rgb[ch][i] == i;
        }
    }
    g_gamma.identity = ident;
    hle_disp_set_gamma(c->rgb, ident || g_gamma.off);
}

/* The video overlay over the presented frame (dst: SHADER_READ_ONLY). */
static HleCmdOverlay g_ovl;
static struct {
    VkImage img;
    VkDeviceMemory mem;
    uint32_t w, h;
} g_ovl_img;

/* The overlay's texels (arena) into its image, left TRANSFER_SRC. */
static void cmd_overlay(const HleCmdOverlay *c)
{
    g_ovl = *c;
    if (!c->on) {
        return;
    }
    if (g_ovl_img.img && (g_ovl_img.w != c->w || g_ovl_img.h != c->h)) {
        flush_idle();
        T.vkDestroyImage(g_hvk.dev, g_ovl_img.img, NULL);
        T.vkFreeMemory(g_hvk.dev, g_ovl_img.mem, NULL);
        g_ovl_img.img = VK_NULL_HANDLE;
    }
    VkImageLayout old = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (!g_ovl_img.img) {
        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = { c->w, c->h, 1 }, .mipLevels = 1,
            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkMemoryRequirements req;
        if (T.vkCreateImage(g_hvk.dev, &ici, NULL, &g_ovl_img.img) != VK_SUCCESS) {
            g_ovl_img.img = VK_NULL_HANDLE;
            return;
        }
        T.vkGetImageMemoryRequirements(g_hvk.dev, g_ovl_img.img, &req);
        int mt = hle_vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt };
        if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &g_ovl_img.mem) != VK_SUCCESS) {
            T.vkDestroyImage(g_hvk.dev, g_ovl_img.img, NULL);
            g_ovl_img.img = VK_NULL_HANDLE;
            return;
        }
        T.vkBindImageMemory(g_hvk.dev, g_ovl_img.img, g_ovl_img.mem, 0);
        g_ovl_img.w = c->w;
        g_ovl_img.h = c->h;
        old = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    close_pass();
    VkCommandBuffer cb = cb_get();
    hle_img_barrier(cb, g_ovl_img.img, VK_IMAGE_ASPECT_COLOR_BIT, old,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy r = { .bufferOffset = c->arena_off,
                            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                            .imageExtent = { c->w, c->h, 1 } };
    T.vkCmdCopyBufferToImage(cb, g_harena.buf, g_ovl_img.img,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    hle_img_barrier(cb, g_ovl_img.img, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

static void overlay_blit(VkCommandBuffer cb, VkImage dst, uint32_t dw, uint32_t dh)
{
    VkImage img = g_ovl_img.img;
    uint32_t tw = g_ovl_img.w, th = g_ovl_img.h;
    if (!g_ovl.on || !img || !g_ovl.fb_w || !g_ovl.fb_h) {
        return;
    }
    int32_t s0x = MAX(g_ovl.src[0], 0), s0y = MAX(g_ovl.src[1], 0);
    int32_t s1x = MIN(g_ovl.src[2], (int32_t)tw), s1y = MIN(g_ovl.src[3], (int32_t)th);
    float sx = (float)dw / g_ovl.fb_w, sy = (float)dh / g_ovl.fb_h;
    int32_t d0x = (int32_t)(g_ovl.dst[0] * sx), d1x = (int32_t)(g_ovl.dst[2] * sx);
    int32_t d0y = (int32_t)(g_ovl.dst[1] * sy), d1y = (int32_t)(g_ovl.dst[3] * sy);
    d0x = MAX(d0x, 0); d1x = MIN(d1x, (int32_t)dw);
    d0y = MIN(MAX(d0y, 0), (int32_t)dh); d1y = MIN(MAX(d1y, 0), (int32_t)dh);
    if (s1x <= s0x || s1y <= s0y || d1x <= d0x || d0y == d1y) {
        return;
    }
    hle_img_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit2 r = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { s0x, s0y, 0 }, { s1x, s1y, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { d0x, d0y, 0 }, { d1x, d1y, 1 } } };
    VkBlitImageInfo2 bi = {
        .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
        .srcImage = img, .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstImage = dst, .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1, .pRegions = &r, .filter = VK_FILTER_LINEAR };
    T.vkCmdBlitImage2(cb, &bi);
    static bool s_first;
    if (!s_first) {
        s_first = true;
        hle_log("hakuX-hle-back", "overlay: %ux%u drawn at %d,%d-%d,%d", tw, th, d0x, d0y,
                d1x, d1y);
    }
    hle_img_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* ---- the frame hook (back.h HleFrameHook) ------------------------------- */

static struct {
    HleFrameHook hook;
    bool armed;                 /* this swap's frame asked for */
    VkBuffer buf;               /* its picture, copied at the swap */
    VkDeviceMemory mem;
    uint32_t w, h;
} g_fh;

void hle_back_set_frame_hook(const HleFrameHook *hook)
{
    g_fh.hook = hook ? *hook : (HleFrameHook){ 0 };
}

void hle_back_save_caches(void)
{
    hle_pcache_save(true);
    hle_pipe_flush(true);
}

/* In the swap's command buffer, once the frame is in its display image (dst,
 * SHADER_READ_ONLY): a copy of it into a host buffer. */
static void fh_copy(VkCommandBuffer cb, VkImage dst, uint32_t w, uint32_t h)
{
    if (!g_fh.armed || g_fh.buf || !rtd_buf((VkDeviceSize)w * h * 4, &g_fh.buf, &g_fh.mem)) {
        return;
    }
    g_fh.w = w;
    g_fh.h = h;
    hle_img_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                            .imageExtent = { w, h, 1 } };
    T.vkCmdCopyImageToBuffer(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_fh.buf, 1, &r);
    hle_img_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* After the swap's submit: the frame to the hook once the GPU reached value. */
static void fh_deliver(uint64_t n, uint64_t value)
{
    const uint8_t *px = NULL;
    void *map = NULL;
    if (g_fh.buf) {
        hle_tl_wait(value);
        if (T.vkMapMemory(g_hvk.dev, g_fh.mem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS) {
            px = map;
        }
    }
    g_fh.hook.got(n, px, px ? g_fh.w : 0, px ? g_fh.h : 0, g_fh.hook.opaque);
    if (map) {
        T.vkUnmapMemory(g_hvk.dev, g_fh.mem);
    }
    if (g_fh.buf) {
        T.vkDestroyBuffer(g_hvk.dev, g_fh.buf, NULL);
        T.vkFreeMemory(g_hvk.dev, g_fh.mem, NULL);
        g_fh.buf = VK_NULL_HANDLE;
    }
    g_fh.armed = false;
}

static void cmd_swap_present(const HleCmdSwap *c);

static void cmd_swap(const HleCmdSwap *c)
{
    rtd_poll();
    pix_poll();
    g_rtd.active |= rtd_range();
    bool dump = rtd_record(c);
    const uint64_t n = g_back.st.swaps + 1;     /* the swap that ends the frame */
    g_fh.armed = g_fh.hook.want && g_fh.hook.want(n, g_fh.hook.opaque);
    cmd_swap_present(c);
    if (dump) {
        rtd_write(c->value);
    }
    if (g_fh.armed) {
        fh_deliver(n, c->value);
    }
    if (g_pix.active) {
        pix_write(c->value);
    }
    g_rtd.active = g_rtd.next || rtd_range();   /* the frame after the request */
    g_rtd.next = false;
    g_rtd.n = 0;
    if (g_rtd.active) {
        rtd_note(g_back.rt);
        for (unsigned i = 0; i < g_rtd.nextra; i++) {
            rtd_note(g_rtd.extra[i]);
        }
    }
    bool more = g_pix.active && g_pix.frames > 1;      /* "*n": the next of the n frames */
    g_pix.frames -= more;
    g_pix.active = g_pix.next || more;
    g_pix.next = false;
    g_pix.n = 0;
    g_pix.seen = 0;
    if (g_pix.active && !g_pix.buf) {
        g_pix.rec = g_new0(PixRec, PIX_MAX);
        if (!rtd_buf(PIX_STRIDE * PIX_MAX, &g_pix.buf, &g_pix.mem)) {
            g_pix.buf = VK_NULL_HANDLE;
        }
    }
}

/* Live debug properties, every 3 swaps: debug.hakux.visall (1: every test
 * fully visible, 0: as counted), a new debug.hakux.vislog=<n> (log the next n
 * results), debug.hakux.visoff (1: no GPU queries; with visall=1 the draws
 * stay the same, the queries' GPU cost goes), debug.hakux.fbbar (0: no
 * barrier before a draw reading its target; its cost, the pixels may go
 * wrong) and debug.hakux.gpucut (frag: next to no fragments, draw: the
 * state without the draws, pass: the passes alone; what is left is the GPU's
 * cost without them); a new debug.hakux.gpulose takes the lost-device path
 * (hle_vk_lose) as a driver fault would: the game stops, the app says so */
static void back_props_poll(uint64_t swap)
{
#ifdef __ANDROID__
    static int64_t next_poll;
    static HleProp log_prop, lose_prop;
    if ((int64_t)swap < next_poll) {
        return;
    }
    next_poll = swap + 3;
    char v[PROP_VALUE_MAX] = "";
    if (hle_prop_get("debug.hakux.visall", v) > 0 && g_vis_dbg.all >= 0 &&
        atoi(v) != g_vis_dbg.all) {
        g_vis_dbg.all = atoi(v) != 0;
        hle_log("hakuX-hle-back", "vis: visall=%d", g_vis_dbg.all);
    }
    if (hle_prop_new(&log_prop, "debug.hakux.vislog", v)) {
        g_vis_dbg.nlog = atoi(v);
    }
    if (hle_prop_new(&lose_prop, "debug.hakux.gpulose", v)) {
        hle_vk_lose(VK_ERROR_DEVICE_LOST, "debug.hakux.gpulose");
    }
    v[0] = 0;
    hle_prop_get("debug.hakux.dryhold", v);
    if ((atoi(v) != 0) != g_dry_hold) {
        g_dry_hold = !g_dry_hold;
        hle_log("hakuX-hle-back", "a dry ring %s its boundaries", g_dry_hold ? "holds" : "submits");
    }
    v[0] = 0;
    hle_prop_get("debug.hakux.pendus", v);
    int64_t pend = atoi(v) > 0 ? atoi(v) * 1000ll : HLE_PEND_NS;
    if (pend != g_pend_ns) {
        g_pend_ns = pend;
        hle_log("hakuX-hle-back", "boundaries wait at most %lld us", (long long)(pend / 1000));
    }
    v[0] = 0;
    hle_prop_get("debug.hakux.visoff", v);
    if ((atoi(v) != 0) != g_vis_dbg.off) {
        g_vis_dbg.off = !g_vis_dbg.off;
        hle_log("hakuX-hle-back", "vis: GPU queries %s", g_vis_dbg.off ? "off" : "on");
    }
    v[0] = 0;
    hle_prop_get("debug.hakux.fbbar", v);
    if (!strcmp(v, "0") != g_hle_fbbar_off) {
        g_hle_fbbar_off = !g_hle_fbbar_off;
        hle_log("hakuX-hle-back", "fb fetch barriers %s", g_hle_fbbar_off ? "off" : "on");
    }
    v[0] = 0;
    hle_prop_get("debug.hakux.gpucut", v);
    int cut = !strcmp(v, "frag") ? 1 : !strcmp(v, "draw") ? 2 : !strcmp(v, "pass") ? 3 :
              !strcmp(v, "tri") ? 4 : 0;
    if (cut != g_hle_gpucut) {
        g_hle_gpucut = cut;
        hle_draw_state_lost();          /* the next draw records its state again */
        hle_log("hakuX-hle-back", "gpu cut: %s", cut == 1 ? "draws scissored to a pixel" :
                cut == 2 ? "no draws, the state alone" :
                cut == 3 ? "the passes alone" :
                cut == 4 ? "3-vertex draws scissored to a pixel" : "none");
    }
#endif
}

static void cmd_swap_present(const HleCmdSwap *c)
{
    close_pass_why(PE_SWAP);
    g_pt.ord = 0;                       /* the next frame's first pass */
    hle_draw_psframe_poll(g_back.st.swaps);
    back_props_poll(g_back.st.swaps);
    hle_pcache_save(false);
    hle_pipe_flush(false);              /* the pipelines made, recorded (at most once a second) */
    HleVkSurf *src = hle_surf_get(c->surf);
    int slot = hle_disp_begin_write(c->swap_id, c->value);
    if (src && slot >= 0 && hle_surf_ensure(src) &&
        (src->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
        VkCommandBuffer cb = cb_get();
        VkImage dst = hle_disp_image(slot);
        uint32_t dw, dh;
        hle_disp_size(&dw, &dh);
        /* the part of src shown (HleCmdSwap.src_w / src_h), in its pixels */
        const uint32_t sw = c->src_w ? MIN(src->w, c->src_w * src->scale) : src->w;
        const uint32_t sh = c->src_h ? MIN(src->h, c->src_h * src->scale) : src->h;
        if (!g_gamma.identity && !g_gamma.off && !hle_disp_gamma_consumer() &&
            hle_gamma_pass(cb, g_back.cur, g_gamma.rgb, src, sw, sh, dst, dw, dh)) {
            overlay_blit(cb, dst, dw, dh);
            fh_copy(cb, dst, dw, dh);
            submit(c->value);           /* through the ramp instead of the blit */
            g_back.st.swaps++;
            return;
        }
        hle_surf_transition(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        hle_img_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit2 r = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { 0, 0, 0 }, { (int32_t)sw, (int32_t)sh, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { { 0, 0, 0 }, { (int32_t)dw, (int32_t)dh, 1 } } };
        VkBlitImageInfo2 bi = {
            .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
            .srcImage = src->img, .srcImageLayout = src->layout,
            .dstImage = dst, .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .regionCount = 1, .pRegions = &r, .filter = VK_FILTER_LINEAR };
        T.vkCmdBlitImage2(cb, &bi);
        hle_img_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        overlay_blit(cb, dst, dw, dh);
        fh_copy(cb, dst, dw, dh);
    }
    submit(c->value);
    g_back.st.swaps++;
}

/* A GPU-rendered texture subresource the CPU locks (swizzled textures,
 * levels, faces: PGR2 reads its light probe back this way) into the arena. */
static bool readback_arena(const HleCmdReadback *c)
{
    HleVkSurf *s = hle_surf_get(c->surf);
    if (!s || !hle_surf_ensure(s) || !(s->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
        return false;
    }
    HleVkSurf *o = s->d.parent ? hle_surf_get(s->d.parent) : s;
    uint32_t level = s->d.parent ? s->d.level : c->level;
    uint32_t layer = s->d.parent ? s->d.face : c->face;
    if (!o || !o->img || level >= o->nlevels || layer >= o->nfaces ||
        MAX(o->d.w >> level, 1u) != c->w || MAX(o->d.h >> level, 1u) != c->h) {
        return false;                           /* mismatched */
    }
    if (o->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        return false;                           /* never rendered: guest memory is current */
    }
    VkCommandBuffer cb = cb_get();
    hle_surf_transition(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImage img = o->img;
    VkImageLayout il = o->general ? VK_IMAGE_LAYOUT_GENERAL : o->layout;
    if (o->scale > 1) {                         /* the guest gets guest-size texels */
        img = hle_surf_down(cb, o, il, level, layer, c->w, c->h);
        if (!img) {
            return false;
        }
        il = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        level = layer = 0;
    }
    VkBufferImageCopy r = {
        .bufferOffset = c->arena_off,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 },
        .imageExtent = { c->w, c->h, 1 } };
    T.vkCmdCopyImageToBuffer(cb, img, il, g_harena.buf, 1, &r);
    VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT };
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
    T.vkCmdPipelineBarrier2(cb, &di);
    return true;
}

static void cmd_readback(const HleCmdReadback *c)
{
    close_pass();
    if (c->arena_off != HLE_RB_DIRECT) {
        if (readback_arena(c)) {
            g_back.st.readbacks++;
        } else {
            g_back.st.readback_skips++;
        }
        submit(c->value);
        return;
    }
    HleVkSurf *s = hle_surf_get(c->surf);
    const HleFmtInfo *fi = s ? hle_fmt_info(s->d.xfmt) : NULL;
    /* Linear-row copies only: a swizzled target (OBJ_CREATE pitch 0, or a
     * non-HLE_FI_LIN format) would get linear rows written over its swizzled
     * guest data (swizzled readback is P4). An image the GPU never wrote
     * (layout UNDEFINED) holds nothing newer than guest memory: skip it so
     * CPU-written data survives. */
    bool linear = s && s->d.pitch && fi && (fi->flags & HLE_FI_LIN);
    bool written = s && s->img && s->layout != VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t w = s ? (c->w ? c->w : s->d.w) : 0, h = s ? (c->h ? c->h : s->d.h) : 0;
    /* without zero-copy RAM, the arena copy that lands in guest memory */
    bool fits = c->fb && s && s->texel_bytes && h &&
                (uint64_t)c->pitch * (h - 1) + (uint64_t)w * s->texel_bytes <= c->fb_len;
    if (s && linear && written && (g_hram.zero_copy || fits) && hle_surf_ensure(s) &&
        (s->aspect & VK_IMAGE_ASPECT_COLOR_BIT) && s->texel_bytes &&
        c->pitch % s->texel_bytes == 0) {
        VkCommandBuffer cb = cb_get();
        hle_surf_transition(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImage img = s->img;
        VkImageLayout il = s->layout;
        if (s->scale > 1 || w != s->d.w || h != s->d.h) {
            /* the guest gets guest-size pixels (or the size asked for:
             * a multisampled display persisted at display size) */
            img = hle_surf_down(cb, s, il, 0, 0, w, h);
            il = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        bool fb = !g_hram.zero_copy;
        VkBufferImageCopy r = {
            .bufferOffset = fb ? c->fb - 1 : c->pa, .bufferRowLength = c->pitch / s->texel_bytes,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { w, h, 1 } };
        if (img) {
            T.vkCmdCopyImageToBuffer(cb, img, il,
                                     !fb ? g_hram.buf : g_hland.buf ? g_hland.buf : g_harena.buf,
                                     1, &r);
            if (fb) {
                hle_tl_land_copied(c->fb - 1, c->pitch, w * s->texel_bytes, h);
            }
        }
        VkMemoryBarrier2 mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
            .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT };
        VkDependencyInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
        T.vkCmdPipelineBarrier2(cb, &di);
        g_back.st.readbacks++;
    } else if (s && !written) {
        /* nothing rendered yet: guest memory is already current */
    } else {
        if (g_back.st.readback_skips++ < 8) {
            hle_log("hakuX-hle-back", "readback of surface %u skipped (linear=%d "
                    "zero_copy=%d arena copy=%s fmt=0x%x %ux%u aspect %x pitch %u fb_len %u) - "
                    "guest keeps its memory (P4: swizzled)",
                    c->surf, linear, g_hram.zero_copy,
                    !c->fb ? "none" : fits ? "yes" : "too small", s ? s->d.xfmt : 0,
                    s ? s->d.w : 0, s ? s->d.h : 0, s ? (unsigned)s->aspect : 0, c->pitch, c->fb_len);
        }
    }
    if (c->value) {         /* 0: a write-back that rides the next submission */
        submit(c->value);
    }
}

/* CopyRects between two images: an exact copy when the formats match, else
 * a blit that converts (colour only). Rectangles scale with the images. */
static void cmd_copy(const HleCmdCopy *c)
{
    close_pass_why(PE_COPY);
    HleVkSurf *s = hle_surf_get(c->src), *d = hle_surf_get(c->dst);
    HleVkSurf *so = s && s->d.parent ? hle_surf_get(s->d.parent) : s;
    const char *why = !s || !d || !so ? "no surface" :
                      !hle_surf_ensure(s) || !hle_surf_ensure(d) ? "no image" :
                      so->layout == VK_IMAGE_LAYOUT_UNDEFINED ? "source never rendered" :
                      s->img == d->img ? "same image" :
                      s->aspect != d->aspect ? "depth and colour" :
                      s->fmt != d->fmt && !(s->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ?
                      "depth formats differ" : NULL;
    if (why) {
        if (g_back.st.copy_skips++ < 8) {
            hle_log("hakuX-hle-back", "copy %u -> %u skipped: %s", c->src, c->dst, why);
        }
        return;
    }
    VkImageCopy2 cr[HLE_CMD_MAX_RECTS];
    VkImageBlit2 br[HLE_CMD_MAX_RECTS];
    VkImageSubresourceLayers ss = { s->aspect, s->d.parent ? s->d.level : 0,
                                    s->d.parent ? s->d.face : 0, 1 };
    VkImageSubresourceLayers ds = { d->aspect, d->d.parent ? d->d.level : 0,
                                    d->d.parent ? d->d.face : 0, 1 };
    if (c->resolve) {
        /* a copy swap's downsample of the multisampled buffer into the back
         * buffer (present.c): the whole image, filtered */
        VkImageBlit2 rb = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
            .srcSubresource = ss,
            .srcOffsets = { { 0, 0, 0 }, { (int32_t)s->w, (int32_t)s->h, 1 } },
            .dstSubresource = ds,
            .dstOffsets = { { 0, 0, 0 }, { (int32_t)d->w, (int32_t)d->h, 1 } } };
        VkCommandBuffer cb = cb_get();
        hle_surf_transition(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        hle_surf_transition(cb, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        hle_surf_written(d);
        VkBlitImageInfo2 bi = {
            .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
            .srcImage = s->img, .srcImageLayout = s->layout,
            .dstImage = d->img, .dstImageLayout = d->layout,
            .regionCount = 1, .pRegions = &rb, .filter = VK_FILTER_LINEAR };
        T.vkCmdBlitImage2(cb, &bi);
        g_back.st.copies++;
        return;
    }
    int32_t k = (int32_t)s->scale, kd = (int32_t)d->scale;
    uint32_t n = 0;
    for (uint32_t i = 0; i < c->nrects && i < HLE_CMD_MAX_RECTS; i++) {
        const HleRect *r = &c->rect[i];
        int32_t w = MIN(MIN(r->x2, (int32_t)s->w / k) - r->x1, (int32_t)d->w / kd - c->x[i]);
        int32_t h = MIN(MIN(r->y2, (int32_t)s->h / k) - r->y1, (int32_t)d->h / kd - c->y[i]);
        if (r->x1 < 0 || r->y1 < 0 || c->x[i] < 0 || c->y[i] < 0 || w <= 0 || h <= 0) {
            continue;
        }
        cr[n] = (VkImageCopy2){
            .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
            .srcSubresource = ss, .srcOffset = { r->x1 * k, r->y1 * k, 0 },
            .dstSubresource = ds, .dstOffset = { c->x[i] * k, c->y[i] * k, 0 },
            .extent = { (uint32_t)(w * k), (uint32_t)(h * k), 1 } };
        br[n] = (VkImageBlit2){
            .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
            .srcSubresource = ss,
            .srcOffsets = { { r->x1 * k, r->y1 * k, 0 }, { (r->x1 + w) * k, (r->y1 + h) * k, 1 } },
            .dstSubresource = ds,
            .dstOffsets = { { c->x[i] * kd, c->y[i] * kd, 0 },
                            { (c->x[i] + w) * kd, (c->y[i] + h) * kd, 1 } } };
        n++;
    }
    if (!n) {
        return;
    }
    VkCommandBuffer cb = cb_get();
    hle_surf_transition(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    hle_surf_transition(cb, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    hle_surf_written(d);
    if (s->fmt == d->fmt && k == kd) {
        VkCopyImageInfo2 ci = {
            .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
            .srcImage = s->img, .srcImageLayout = s->layout,
            .dstImage = d->img, .dstImageLayout = d->layout,
            .regionCount = n, .pRegions = cr };
        T.vkCmdCopyImage2(cb, &ci);
    } else {
        VkBlitImageInfo2 bi = {
            .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
            .srcImage = s->img, .srcImageLayout = s->layout,
            .dstImage = d->img, .dstImageLayout = d->layout,
            .regionCount = n, .pRegions = br, .filter = VK_FILTER_NEAREST };
        T.vkCmdBlitImage2(cb, &bi);
    }
    g_back.st.copies++;
}

/* Texels the CPU wrote into guest memory, into a surface's image at its
 * scale: CopyRects from a surface that has no image, a target the title
 * locked and wrote (DOA2 Ultimate copies its persisted display into the
 * back buffer of the device it just Reset). Each rectangle goes through a
 * guest-size scratch image, blitted up. */
static void cmd_upload(const HleCmdUpload *c)
{
    close_pass();
    HleVkSurf *d = hle_surf_get(c->dst);
    const char *why = !d ? "no surface" : !g_hram.zero_copy && !c->arena ? "guest RAM not mapped" :
                      d->d.parent ? "a level or face" : !hle_surf_ensure(d) ? "no image" :
                      !(d->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? "not colour" :
                      !d->texel_bytes || c->pitch % d->texel_bytes ? "pitch" : NULL;
    int32_t k = d ? (int32_t)MAX(d->scale, 1u) : 1, n = 0;
    VkCommandBuffer cb = why ? VK_NULL_HANDLE : cb_get();
    for (uint32_t i = 0; !why && i < c->nrects && i < HLE_CMD_MAX_RECTS; i++) {
        const HleRect *r = &c->rect[i];
        int32_t w = MIN(r->x2 - r->x1, (int32_t)d->w / k - c->x[i]);
        int32_t h = MIN(r->y2 - r->y1, (int32_t)d->h / k - c->y[i]);
        if (r->x1 < 0 || r->y1 < 0 || c->x[i] < 0 || c->y[i] < 0 || w <= 0 || h <= 0) {
            continue;
        }
        VkImage tmp = hle_surf_scratch(d->fmt, (uint32_t)w, (uint32_t)h);
        if (!tmp) {
            why = "no scratch image";
            break;
        }
        hle_img_barrier(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        /* the rows in guest RAM, or their copy in the arena (from lo) */
        const uint32_t at = c->pa + (uint32_t)r->y1 * c->pitch + (uint32_t)r->x1 * d->texel_bytes;
        VkBufferImageCopy bc = {
            .bufferOffset = c->arena ? (VkDeviceSize)(c->arena - 1) + (at - c->lo) : at,
            .bufferRowLength = c->pitch / d->texel_bytes,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { (uint32_t)w, (uint32_t)h, 1 } };
        T.vkCmdCopyBufferToImage(cb, c->arena ? g_harena.buf : g_hram.buf, tmp,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
        hle_img_barrier(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        hle_surf_transition(cb, d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit2 br = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { { 0, 0, 0 }, { w, h, 1 } },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { { c->x[i] * k, c->y[i] * k, 0 },
                            { (c->x[i] + w) * k, (c->y[i] + h) * k, 1 } } };
        VkBlitImageInfo2 bi = {
            .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
            .srcImage = tmp, .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .dstImage = d->img, .dstImageLayout = d->layout,
            .regionCount = 1, .pRegions = &br, .filter = VK_FILTER_NEAREST };
        T.vkCmdBlitImage2(cb, &bi);
        n++;
    }
    if (why) {
        if (g_back.st.upload_skips++ < 8) {
            hle_log("hakuX-hle-back", "upload into %u skipped: %s", c->dst, why);
        }
        return;
    }
    if (n) {
        hle_surf_written(d);
        g_back.st.uploads++;
    }
}

static void cmd_zread(const HleCmdZRead *c)
{
    HleVkSurf *s = hle_surf_get(c->src), *d = hle_surf_get(c->dst);
    if (s && d && d->img && d->zr_src == c->src && d->zr_gen == s->wgen &&
        !memcmp(d->zr_map, c->map, 4) && d->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        return;                         /* the depth buffer is unchanged since */
    }
    if (s && d && d->img && d->wgen > s->wgen) {
        /* The memory's last writer was the texture's own image (a copy or
         * an upload into it), not the depth buffer: it holds the texels.
         * Splinter Cell copies the screen into a texture over its depth
         * buffer's memory (CopyRects of the front buffer) and cross-fades
         * from it to the mission briefing after a level loads; converting
         * the depth would overwrite the copy. */
        return;
    }
    close_pass();
    const char *why = !s || !d ? "no surface" :
                      !hle_surf_ensure(s) || !hle_surf_ensure(d) ? "no image" :
                      s->layout == VK_IMAGE_LAYOUT_UNDEFINED ? "depth never rendered" :
                      !(s->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? "no stencil" :
                      !(d->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? "not colour" :
                      hle_zread_pass(cb_get(), g_back.cur, s, d, c->map);
    if (why) {
        if (g_back.st.zread_skips++ < 8) {
            hle_log("hakuX-hle-back", "zread %u -> %u skipped: %s", c->src, c->dst, why);
        }
        return;
    }
    g_back.st.zreads++;
    if (d->zr_src != c->src) {
        hle_log("hakuX-hle-back", "zread: texture %u reads depth surface %u (%ux%u fmt %d)",
                c->dst, c->src, s->w, s->h, (int)s->fmt);
    }
    d->zr_src = c->src;
    d->zr_gen = s->wgen;
    memcpy(d->zr_map, c->map, 4);
    rtd_snap(c->dst);                   /* debug.hakux.rtdump: what the texture samples */
}

static void cmd_zwrite(const HleCmdZWrite *c)
{
    HleVkSurf *s = hle_surf_get(c->src), *d = hle_surf_get(c->dst);
    if (!s || !d || s->wgen <= d->wgen) {
        return;                         /* the depth buffer is newer (or the same) */
    }
    close_pass();
    const char *why = !hle_surf_ensure(s) || !hle_surf_ensure(d) ? "no image" :
                      s->layout == VK_IMAGE_LAYOUT_UNDEFINED ? "colour never rendered" :
                      !(s->aspect & VK_IMAGE_ASPECT_COLOR_BIT) || s->texel_bytes != 4 ?
                      "not 32-bit colour" :
                      !(d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? "not depth" :
                      hle_zwrite_pass(cb_get(), g_back.cur, s, d);
    if (why) {
        if (g_back.st.zread_skips++ < 8) {
            hle_log("hakuX-hle-back", "zwrite %u -> %u skipped: %s", c->src, c->dst, why);
        }
        return;
    }
    hle_surf_written(d);
    static unsigned s_logged;
    if (!s_logged++) {
        hle_log("hakuX-hle-back", "zwrite: colour surface %u's words as depth surface %u "
                "(first logged)", c->src, c->dst);
    }
}

uint64_t hle_back_swap_count(void)
{
    return g_back.st.swaps;
}

bool hle_back_capture_armed(void)
{
    return g_pix.active || g_pix.next;
}

bool hle_back_vis_query_open(void)
{
    return g_vis.in_pass >= 0;
}

uint64_t hle_back_submits(void)
{
    return g_back.st.submits;
}

static void dispatch(const HleCmdHdr *h)
{
    const void *p = hle_cmd_payload(h);
    switch (h->op) {
    case HLE_OP_STATE: case HLE_OP_STATE_PART: case HLE_OP_VIEWPORT: case HLE_OP_VSH:
    case HLE_OP_OBJ_CREATE: case HLE_OP_KICK: case HLE_OP_DRAW:
        break;
    default:
        hle_back_breaks++;
        break;
    }
    switch (h->op) {
    case HLE_OP_DEVICE_CREATE:  cmd_device(p, false); break;
    case HLE_OP_DEVICE_RESET:   cmd_device(p, true); break;
    case HLE_OP_OBJ_CREATE:
        hle_draw_forget_tex(((const HleCmdObj *)p)->s.id);
        hle_surf_define(&((const HleCmdObj *)p)->s);
        break;
    case HLE_OP_OBJ_DESTROY: {
        const HleCmdObjDestroy *d = p;
        if (d->id == g_back.rt || d->id == g_back.ds) {
            close_pass();
        }
        /* d->after (front end: last value + 1) predicts the next submission;
         * parking ties the destroy to the submit that actually closes the
         * open recording, whatever value it carries. */
        hle_surf_destroy_after(d->id, g_back.recording ? HLE_SURF_PARKED : d->after);
        hle_draw_forget_tex(d->id);     /* its uploaded texture, if any */
        hle_draw_forget_surf(d->id);
        break;
    }
    case HLE_OP_SET_RT:   cmd_set_rt(p); break;
    case HLE_OP_VIEWPORT: g_back.vp = *(const HleCmdViewport *)p; break;
    case HLE_OP_CLEAR:
        cmd_clear(p);
        if (g_pix.active) {
            pix_after(NULL);
        }
        break;
    case HLE_OP_SWAP:     cmd_swap(p); break;
    case HLE_OP_KICK:
        if (((const HleCmdFence *)p)->value) {
            pend_boundary(((const HleCmdFence *)p)->value);
        } else {
            close_pass();       /* debug (debug.hakux.kick=3): the pass split alone */
        }
        break;
    case HLE_OP_READBACK: cmd_readback(p); break;
    case HLE_OP_UPLOAD:   cmd_upload(p); break;
    case HLE_OP_STATE:    hle_draw_cmd_state(p); break;
    case HLE_OP_STATE_PART: hle_draw_cmd_state_part(p); break;
    case HLE_OP_GAMMA:    cmd_gamma(p); break;
    case HLE_OP_OVERLAY:  cmd_overlay(p); break;
    case HLE_OP_COPY:     cmd_copy(p); break;
    case HLE_OP_ZREAD:    cmd_zread(p); break;
    case HLE_OP_ZWRITE:   cmd_zwrite(p); break;
    case HLE_OP_VSH:      hle_draw_cmd_vsh(p); break;
    case HLE_OP_VIS_BEGIN:
        g_vis.active = true;
        g_vis.nparts = 0;
        g_vis.in_pass = -1;
        break;
    case HLE_OP_VIS_END:  cmd_vis_end(p); break;
    case HLE_OP_TEX:
        close_pass_why(PE_TEX);                 /* uploads never split a pass (P3 Task 9 hoists) */
        hle_draw_cmd_tex(p, cb_get());
        break;
    case HLE_OP_DRAW: {
        /* a draw reading the target it renders: the pass renders in
         * RENDERING_LOCAL_READ (reopened once when it was not) */
        unsigned fb = hle_draw_fb_stages(p, g_back.rt);
        HleVkSurf *fbrt = fb ? hle_surf_get(g_back.rt) : NULL;
        if (fbrt && !fbrt->general && !fbrt->local_read) {
            fbrt->local_read = true;
            close_pass_why(PE_LREAD);
        }
        /* an EQUAL pass reading the depth buffer (hle_draw_zeq): the same */
        unsigned zq = hle_draw_zeq(p, g_back.ds);
        HleVkSurf *zds = zq ? hle_surf_get(g_back.ds) : NULL;
        if (zds && !zds->general && !zds->local_read) {
            zds->local_read = true;
            close_pass_why(PE_LREAD);
        }
        if (hle_draw_tex_needs_prep(p, g_back.rt)) {
            close_pass_why(PE_PREP);             /* sampled render targets change layout */
            hle_draw_tex_prep(p, g_back.rt, cb_get());
        }
        /* combiner stages reading the target at their own coordinates: a
         * copy of it, made outside the pass */
        bool snap = hle_draw_snap_needed(p, g_back.rt);
        if (snap) {
            close_pass_why(PE_COPY);
            hle_draw_snap(cb_get(), hle_surf_get(g_back.rt));
        }
        if (g_hle_gpucut == 3) {
            if (!g_back.pass_open) {
                open_pass(true);        /* debug: the passes alone, no draw state */
            }
        } else if (g_back.pass_open || open_pass(hle_draw_lrz_off())) {
            vis_draw(g_back.fr[g_back.cur].cb);
            VkImageView zv = zds ? hle_zread_depth_view(zds) : VK_NULL_HANDLE;
            if (fbrt || zv) {
                /* the depth read waits for the pass's depth writes since the last */
                bool zbar = zv && zds->wgen != g_back.zeq_wgen;
                hle_draw_fb_bind(g_back.fr[g_back.cur].cb, fbrt, zds, zv, zbar);
                if (zbar) {
                    g_back.zeq_wgen = zds->wgen;
                }
                g_pt.fb++;
            }
            g_hle_zeq = zv ? zq : 0;
            hle_draw_cmd_draw(p, g_back.fr[g_back.cur].cb, g_back.rt, g_back.ds, &g_back.vp);
            g_back.st.draws++;
            if (g_pix.active) {
                pix_after(p);
            }
        } else {
            draw_nopass();
        }
        if (snap) {
            hle_draw_snap_end();
        }
        break;
    }
    default:
        hle_log("hakuX-hle-back", "unknown op %u (len %u)", h->op, h->len);
        break;
    }
}

/* After draining: up to ~200 us waiting for the producer's next publish, in
 * WFE on the ring head (its store clears the exclusive monitor; interrupts
 * and the timer event stream bound each WFE), before sleeping on the kick
 * event. While the vCPU emits draws every few microseconds the render thread
 * never sleeps, so its commits never pay a futex wake. */
static bool ring_wait_brief(HleRing *r, uint32_t seen)
{
#if defined(__aarch64__)
    uint64_t f, t0, t;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t0));
    for (;;) {
        uint32_t h;
        __asm__ volatile("ldaxr %w0, [%1]" : "=r"(h) : "r"(&r->head) : "memory");
        if (h != seen) {
            return true;
        }
        if (hle_atomic_read(&g_pend.urgent)) {
            return false;                       /* a guest waits: submit now */
        }
        __asm__ volatile("wfe" ::: "memory");
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
        if (t - t0 > f / 5000) {
            return false;
        }
    }
#else
    return false;
#endif
}

/* What finished GPU work frees: destroyed surfaces, retired textures and
 * visibility slots (only when the timeline moved). */
static void reap(void)
{
    static uint64_t reaped = UINT64_MAX;
    uint64_t done = hle_tl_completed();
    if (done != reaped) {
        reaped = done;
        hle_surf_reap(done);
        hle_draw_tex_reap(done);
        vis_reap(done);
    }
}

static void *render_thread(void *opaque)
{
    HleRing *r = hle_fe_ring();
    hle_hitch_thread(HLE_HT_RENDER);
    for (;;) {
        hle_event_reset(&g_back.kick);
        const HleCmdHdr *h;
        int64_t t0 = hle_prof_begin();
        while ((h = hle_ring_peek(r)) != NULL) {
            if (unlikely(hle_vk_lost())) {
                /* nothing goes to a lost device: the ring drains (the
                 * front end's waits on the timeline all return) */
                hle_ring_consume(r);
                continue;
            }
            dispatch(h);
            hle_ring_consume(r);
            g_back.st.cmds++;
            pend_age_check();
            if (!(g_back.st.cmds & 1023)) {
                /* a ring that never drains (DOA2 uncapped at 400 frames a
                 * second, a target chain made and dropped every frame) must
                 * still free: the surface table filled in a second */
                reap();
            }
        }
        hle_prof_end(HLE_PROF_BACK, t0);
        uint32_t used = hle_ring_used(r);
        if (used > g_back.st.ring_hw) {
            g_back.st.ring_hw = used;
        }
        reap();
        uint32_t seen = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
        if (hle_ring_peek(r) || ring_wait_brief(r, seen)) {
            continue;
        }
        if (g_dry_hold && g_pend.n && !hle_atomic_read(&g_pend.urgent) &&
            hle_now_ns() - g_pend.since < g_pend_ns) {
            continue;                   /* debug: held until it ages (WFE waits above) */
        }
        if (!hle_vk_lost()) {
            pend_flush();               /* the ring ran dry: submit what waits */
        }
        if (hle_ring_peek(r)) {
            continue;
        }
        /* asleep from here: a commit sets the event only now (hle_back_kick),
         * and the ring is checked again after the flag (a publish before it
         * is seen here, one after it sets the event) */
        __atomic_store_n(&g_back.sleeping, 1, __ATOMIC_SEQ_CST);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (hle_ring_peek(r)) {
            __atomic_store_n(&g_back.sleeping, 0, __ATOMIC_RELAXED);
            continue;
        }
        uint32_t pa = hle_hitch_enter(HLE_ACT_IDLE);
        hle_event_wait(&g_back.kick);
        __atomic_store_n(&g_back.sleeping, 0, __ATOMIC_RELAXED);
        hle_hitch_leave(pa);
    }
    return NULL;
}

int hle_back_start(void)
{
    if (g_back.running) {
        return 0;
    }
    int r = hle_vk_init();
    if (r) {
        if (r == -1 && !g_hle_unavailable[0]) {
            snprintf(g_hle_unavailable, sizeof(g_hle_unavailable),
                     "%s can't create a Vulkan device", g_hvk.props.deviceName[0] ?
                     g_hvk.props.deviceName : "this GPU");
        }
        return r;
    }
    /* DXT decoded on the way in (no BC: pbres.c) takes four to eight times the bytes */
    const char *what = hle_vk_guest_ram_init() ? "guest RAM" :
                       hle_vk_arena_init(g_hvk.caps.bc ? 32u << 20 : 64u << 20) ? "the upload arena" :
                       hle_vk_land_init(16u << 20) ? "the readback buffer" :
                       hle_fe_init() ? "the front end" :
                       hle_tl_init(on_complete) ? "the timeline" :
                       hle_draw_init() ? "the draw state" : NULL;
    if (what) {
        hle_log("hakuX-hle-back", "back end init failed: %s", what);
        snprintf(g_hle_unavailable, sizeof(g_hle_unavailable), "%s can't set up %s",
                 g_hvk.props.deviceName, what);
        return -3;
    }
    /* before the render thread and the game: the pipeline table is this
     * thread's until then */
    hle_draw_prebuild();
    for (unsigned i = 0; i < HLE_FRAMES; i++) {
        VkCommandPoolCreateInfo pci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = g_hvk.qfam };
        T.vkCreateCommandPool(g_hvk.dev, &pci, NULL, &g_back.fr[i].pool);
        VkCommandBufferAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = g_back.fr[i].pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        T.vkAllocateCommandBuffers(g_hvk.dev, &ai, &g_back.fr[i].cb);
    }
    hle_event_init(&g_back.kick, false);
    hle_barrier_set_waiter(hle_tl_completed, hle_tl_wait);
    hle_thread_create(&g_back.thread, "hle.render", render_thread, NULL,
                       HLE_THREAD_DETACHED);
    g_back.running = true;
    hle_batch_init();
    hle_disp_setup();
    hle_vk_set_ready();                 /* the window's presenter may start */
    hle_log("hakuX-hle-back", "back end started (zero_copy=%d)", g_hram.zero_copy);
    return 0;
}

bool hle_back_running(void)
{
    return g_back.running;
}

/* A commit (hle_fe_commit, its ring publish before): the event only for a
 * render thread going to sleep. Set at every commit, its mutex was ~6 % of
 * the push-buffer decoder's time (Blinx), the render thread mostly awake
 * (draining the ring, or waiting briefly on it). */
void hle_back_kick(void)
{
    if (g_back.running) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&g_back.sleeping, __ATOMIC_SEQ_CST)) {
            hle_event_set(&g_back.kick);
        }
    }
}

void hle_back_urgent(void)
{
    if (g_back.running) {
        hle_atomic_set(&g_pend.urgent, 1);
#if defined(__aarch64__)
        __asm__ volatile("sev" ::: "memory");  /* out of ring_wait_brief's WFE */
#endif
        hle_event_set(&g_back.kick);
    }
}

void hle_back_draw_diag(void)
{
    hle_draw_ff_diag();
}

