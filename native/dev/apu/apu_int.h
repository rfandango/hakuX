/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
/* hakuX native core: the device's state, on the core's own services
 * (apu_base.h) in place of QOM/PCI and the QEMU memory API. */
#ifndef NATIVE_DEV_APU_INT_H
#define NATIVE_DEV_APU_INT_H

#include "native/dev/apu/apu_base.h"
#include "native/dev/apu/apu_regs.h"
#include "native/dev/apu/apu_debug.h"
#include "native/dev/apu/fpconv.h"
#include "native/dev/apu/vp/vp.h"
#include "native/dev/apu/dsp/gp_ep.h"

#define GET_MASK(v, mask) (((v) & (mask)) >> ctz32(mask))

#define SET_MASK(v, mask, val)                                       \
    do {                                                             \
        (v) &= ~(mask);                                              \
        (v) |= ((val) << ctz32(mask)) & (mask);                      \
    } while (0)

#define CASE_4(v, step)                                              \
    case (v):                                                        \
    case (v)+(step):                                                 \
    case (v)+(step)*2:                                               \
    case (v)+(step)*3

// #define DEBUG_MCPX

#ifdef DEBUG_MCPX
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

/* The APU's interrupt line (PCI INTA of device 5). */
#define MCPX_APU_IRQ 5

typedef struct MCPXAPUState {
    bool exiting;
    bool set_irq;
    /* One-shot guard: set after a savestate's load so the frame thread
     * swallows exactly the FIRST FE/notify interrupt after it (a voice that
     * was at completion when the state was saved re-emits its stale
     * notification, which would dispatch a since-freed guest callback ->
     * BugCheck 0xA). See mcpx_apu_post_load() / the frame thread. */
    bool snapshot_just_loaded;

    pthread_t apu_thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    /* update_irq: the line recomputed from ISTS/IEN and set as one step. The
     * guest's acknowledge (its CPU thread) and the frame thread's raise
     * both recompute it; interleaved, an acknowledge could lower the line
     * over the raise just made (Fable's movies stopped at a stream switch). */
    pthread_mutex_t irq_lock;

    /* guest RAM (g_apu_ram, g_apu_ram_size) */
    uint8_t *ram_ptr;
    uint32_t ram_size;

    MCPXAPUVPState vp;
    MCPXAPUGPState gp;
    MCPXAPUEPState ep;

    uint32_t regs[0x20000];

    int ep_frame_div;
    int64_t sleep_acc_us;
    int frame_count;
    int64_t frame_count_time_ms;
    int64_t next_frame_time_us;
    bool pace_gp;               /* pace the GP frames of this EP frame */
    int64_t gp_boost_until_us;  /* a CPU thread waits; GP frames run to completion */

    /* the guest's ISTS writes (its acknowledges), and the frame thread's
     * once-a-second look at states that last (apu.c stuck_check) */
    uint32_t ists_writes;
    struct {
        int64_t at_ms;          /* the last look */
        uint32_t ists_writes;   /* ists_writes then */
        unsigned seen, logged;  /* STUCK_* seen at the last look, logged since they began */
    } stuck;

    struct {
        McpxApuDebugMonitorPoint point;
        int16_t frame_buf[256][2]; // 1 EP frame (0x400 bytes), queued to the output
        int fifo_capacity_bytes;
        int device_buffer_bytes;
        int queued_bytes_low, queued_bytes_high;
    } monitor;
} MCPXAPUState;

extern MCPXAPUState *g_state; /* the one APU: the savestate section's */

/* ---- the output device (out.c): 48 kHz 16-bit stereo, a ring of EP frames
 * (1024 bytes each) the frame thread fills and the device's callback drains */

/* Opens the device with the ring's capacity (bytes) and the callback's size
 * (frames); wake is broadcast after each callback (the frame thread waits on
 * it at the high watermark). Returns the callback's size in bytes (the
 * device's, or the request's). */
int apu_out_open(int capacity_bytes, int samples, pthread_cond_t *wake);
/* After the frame thread has stopped: the device closed and the ring freed,
 * for another open (the core stopped in its process; nc_dev_apu_free). */
void apu_out_close(void);
/* the bytes queued */
int apu_out_queued(void);
/* one EP frame in (the frame thread: the only producer; there is room) */
void apu_out_push(const void *frame, int bytes);
/* debug (HAKUX_APU_STATS): callbacks, and those that came up short, since
 * the last call */
void apu_out_stats(uint32_t *callbacks, uint32_t *short_callbacks);

/* ---- savestates (state.c): the frame thread held off around them */
void mcpx_apu_reset(MCPXAPUState *d);
void mcpx_apu_post_load(MCPXAPUState *d);

#endif
