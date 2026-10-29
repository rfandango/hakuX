/*
 * hakuX HLE GPU — stutter / hang monitor.
 *
 * Stutters are what the screen shows: each frame's timeline (the decoder's
 * swap, the guest's flip, render-thread write, GPU done, front at a vblank,
 * picked up by the UI) is stamped, and a frame reaching the front more than
 * half a vblank after the title's cadence, or frames never shown (dropped),
 * is a stutter, attributed to the stage that made it late. The decoder's
 * flips and display starts close the frame windows (hle_hitch_pb_swap); one
 * running past the frame interval (the cadence the screen shows: there is
 * no presentation interval to go by) is explained (where the time went) for
 * that attribution, and one of 500 ms or more is a hang. Without the display
 * path a window of 2 frame intervals (at least 50 ms) counts as a stutter.
 * HAKUX_HLE_HITCH_MS / HAKUX_HLE_HANG_MS override the thresholds,
 * HAKUX_HLE_HITCH=0 turns the monitor off. A watchdog thread reports a
 * hang while it lasts and samples the vCPU (EIP, halted) once a window is
 * overdue. The reason comes from where the window's time went: activity
 * spans the decoder and the render thread mark at their rare transitions
 * (the flip, the guest's interrupt handler, the command ring, the arena,
 * the GPU, compiles, submits, sleeps; never per draw), GPU progress, disc
 * reads, JIT flushes and thread CPU time. Events go to logcat
 * (hakuX-hle-hitch) and to the overlay.
 */
#ifndef HW_XBOX_HLE_HITCH_H
#define HW_XBOX_HLE_HITCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hw/xbox/hle/prof.h"

enum { HLE_HT_VCPU, HLE_HT_RENDER, HLE_HT_DECODE, HLE_HT_N };

enum {
    HLE_ACT_RUN,        /* working: guest code (vCPU), commands (render), decoding */
    HLE_ACT_IDLE,       /* render: asleep, no commands */
    HLE_ACT_GPU,        /* waiting for the GPU timeline */
    HLE_ACT_RING,       /* decode: command ring full (render thread behind) */
    HLE_ACT_ARENA,      /* decode: upload arena full (GPU behind) */
    HLE_ACT_COMPILE,    /* render: shader / pipeline compile */
    HLE_ACT_SUBMIT,     /* render: queue submit */
    HLE_ACT_DISPLAY,    /* render: no free display slot */
    HLE_ACT_FLIP,       /* decode: waiting for the flip (FLIP_STALL) */
    HLE_ACT_TRAP,       /* decode: waiting for the guest's interrupt handler (a trap) */
    HLE_ACT_N
};

typedef struct HleHitchThr {
    uint32_t act;                   /* current activity */
    int64_t since;                  /* its start (counter ticks) */
    int64_t acc[HLE_ACT_N];         /* ticks in completed spans */
    uint32_t n[HLE_ACT_N];          /* spans entered */
} HleHitchThr;

extern HleHitchThr g_hitch_thr[HLE_HT_N];
extern __thread int8_t t_hitch;     /* this thread's slot, -1 = not monitored */
extern bool g_hitch_on;

static inline void hitch_set(HleHitchThr *h, uint32_t act)
{
    int64_t now = hle_prof_now();
    h->acc[h->act] += now - h->since;
    __atomic_store_n(&h->since, now, __ATOMIC_RELAXED);
    __atomic_store_n(&h->act, act, __ATOMIC_RELEASE);
}

/* Enter an activity on this thread; returns what to restore. An activity
 * other than RUN is kept (the more specific one: an arena wait's timeline
 * wait stays ARENA). */
static inline uint32_t hle_hitch_enter(uint32_t act)
{
    int t = t_hitch;
    if (t < 0 || !g_hitch_on) {
        return HLE_ACT_N;
    }
    HleHitchThr *h = &g_hitch_thr[t];
    uint32_t prev = h->act;
    if (prev != HLE_ACT_RUN && act != HLE_ACT_RUN) {
        return HLE_ACT_N;
    }
    h->n[act]++;
    hitch_set(h, act);
    return prev;
}

static inline void hle_hitch_leave(uint32_t prev)
{
    int t = t_hitch;
    if (prev < HLE_ACT_N && t >= 0) {
        hitch_set(&g_hitch_thr[t], prev);
    }
}

/* the calling thread's slot (render and decode threads at start; the vCPU) */
void hle_hitch_thread(int slot);
/* the decode thread: a flip or a display start of swap id (closes the
 * frame window) */
void hle_hitch_pb_swap(uint32_t id);
/* The emulation paused (the host's): no hang while it is, and the window it
 * falls in isn't measured (its next swap closes it). */
void hle_hitch_pause(void);
/* The renderer stopping (M2): the watchdog stopped and joined, the monitor
 * as at a new process. */
void hle_hitch_stop(void);
/* a frame's stages (any thread): the display slot written, GPU done, made
 * the front image at a vblank, picked up by the UI; the guest flipped to
 * it (its vblank handler, or a display start) */
enum { HLE_FR_SWAP, HLE_FR_WRITE, HLE_FR_READY, HLE_FR_FRONT, HLE_FR_SHOWN, HLE_FR_FLIP,
       HLE_FR_N };
void hle_hitch_frame(uint32_t id, int stage);
/* the GPU timeline advanced (render / timeline thread) */
void hle_hitch_gpu_done(void);
/* overlay text (lines separated by '\n'); false = nothing to show */
bool hle_hitch_overlay(char *buf, size_t n);

/* The vCPU's PC for the samplers (stalls here, the perf capture in hle.c),
 * from a front end whose vCPU state is not first_cpu's env (the native core:
 * env only changes at hooks and kernel calls). True with *pc the guest EIP,
 * or HLE_PC_HOST | (host offset >> 4) while the vCPU runs host code, with
 * HLE_PC_SYSOP in a kernel call or hook (else FEX: the JIT compiler, the
 * dispatcher); false when no sample came. NULL: env. */
#define HLE_PC_HOST 0xF0000000u
#define HLE_PC_SYSOP 0x08000000u    /* host code of a kernel call or hook (else FEX) */
#define HLE_PC_IDLE 0xFFFFFFFFu     /* the vCPU waits for an interrupt (halted) */
extern bool (*g_hle_pc_sampler)(uint32_t *pc);

#endif
