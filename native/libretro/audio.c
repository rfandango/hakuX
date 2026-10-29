/*
 * hakuX native core — the APU's output in the libretro core: 48 kHz 16-bit
 * stereo, handed to RetroArch from retro_run.
 *
 * The frame thread pushes each EP frame into a ring, and one consumer
 * drains it (two counters). The consumer is retro_run, which takes
 * one emulated period's samples each call (800.8 frames on average at
 * 60000/1001 Hz, through a fractional accumulator): the APU sees it as a device's
 * callback of one period and keeps the ring between one and three of them,
 * pacing itself in real time. Draining everything each call would not do:
 * every drain to empty sets the frame thread producing ahead of real time
 * to the low watermark again.
 */
#include "native/dev/apu/apu_int.h"
#include "native/libretro/libretro_int.h"

#define OUT_RATE 48000
#define OUT_FRAME_BYTES 4   /* a stereo sample: two 16-bit channels */

static struct {
    uint8_t *buf;
    uint32_t cap;
    uint64_t head, tail;     /* bytes written, bytes read */
    pthread_cond_t *wake;
    uint64_t acc;            /* the frames' remainder, in 1/NC_LR_FPS_NUM of one */
    uint32_t callbacks, short_callbacks;
} g_out;

int apu_out_queued(void)
{
    return (int)(apu_ld_acq(&g_out.head) - apu_ld_acq(&g_out.tail));
}

void apu_out_push(const void *frame, int bytes)
{
    const uint64_t head = g_out.head;
    assert(head + (uint64_t)bytes - apu_ld_acq(&g_out.tail) <= g_out.cap);
    const uint32_t at = (uint32_t)(head % g_out.cap);
    const uint32_t first = MIN((uint32_t)bytes, g_out.cap - at);
    memcpy(g_out.buf + at, frame, first);
    memcpy(g_out.buf, (const uint8_t *)frame + first, (uint32_t)bytes - first);
    apu_st_rel(&g_out.head, head + (uint64_t)bytes);
}

void apu_out_stats(uint32_t *callbacks, uint32_t *short_callbacks)
{
    *callbacks = apu_xchg(&g_out.callbacks, 0);
    *short_callbacks = apu_xchg(&g_out.short_callbacks, 0);
}

/* The frames one call takes: a period's, 48000 x 1001 / 60000 = 800.8. */
static uint32_t period_frames(void)
{
    g_out.acc += (uint64_t)OUT_RATE * NC_LR_FPS_DEN;
    const uint64_t n = g_out.acc / NC_LR_FPS_NUM;
    g_out.acc -= n * NC_LR_FPS_NUM;
    return (uint32_t)n;
}

int apu_out_open(int capacity_bytes, int samples, pthread_cond_t *wake)
{
    g_out.buf = calloc(1, (size_t)capacity_bytes);
    g_out.cap = (uint32_t)capacity_bytes;
    g_out.wake = wake;
    const int frames = (int)((uint64_t)OUT_RATE * NC_LR_FPS_DEN / NC_LR_FPS_NUM);
    nc_log("hakuX-apu", "output: libretro, one period (%d.%d frames) a retro_run", frames,
           (int)((uint64_t)OUT_RATE * NC_LR_FPS_DEN * 10 / NC_LR_FPS_NUM % 10));
    return frames * OUT_FRAME_BYTES;
}

void apu_out_close(void)
{
    free(g_out.buf);
    memset(&g_out, 0, sizeof(g_out));   /* nc_lr_audio_run hands over nothing until the next open */
}

/* retro_run: one period's samples to RetroArch, what the ring has of them
 * (a short call); none before the APU opened its output. */
void nc_lr_audio_run(retro_audio_sample_batch_t batch)
{
    if (!g_out.buf || !batch) {
        return;
    }
    static int16_t pcm[4096 * 2];
    uint32_t want = period_frames();
    const uint64_t tail = g_out.tail;
    const uint32_t have = (uint32_t)((apu_ld_acq(&g_out.head) - tail) / OUT_FRAME_BYTES);
    const uint32_t n = MIN(MIN(want, have), (uint32_t)(sizeof(pcm) / OUT_FRAME_BYTES));
    const uint32_t bytes = n * OUT_FRAME_BYTES;
    const uint32_t at = (uint32_t)(tail % g_out.cap);
    const uint32_t first = MIN(bytes, g_out.cap - at);
    memcpy(pcm, g_out.buf + at, first);
    memcpy((uint8_t *)pcm + first, g_out.buf, bytes - first);
    apu_st_rel(&g_out.tail, tail + bytes);
    apu_inc(&g_out.callbacks);
    if (n < want) {
        apu_inc(&g_out.short_callbacks);
    }
    pthread_cond_broadcast(g_out.wake);
    for (uint32_t done = 0; done < n;) {
        const size_t took = batch(pcm + 2 * done, n - done);
        if (!took) {
            break;          /* RetroArch's buffer is full: the rest is dropped, not kept */
        }
        done += (uint32_t)took;
    }
}
