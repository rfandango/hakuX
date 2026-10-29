/* A title relaunch makes another APU in the frontend's process. No guest
 * executable, renderer or sound device needed: exercise the real lifecycle
 * and feed a known EP DMA frame through the real libretro output. */
#include "native/dev/apu/apu_int.h"
#include "native/dev/dev.h"
#include "native/core/irq.h"
#include "native/core/mmio.h"
#include "native/core/vm.h"
#include "native/libretro/libretro_int.h"

static unsigned failures, received, mismatches;

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

static size_t batch(const int16_t *pcm, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        mismatches += pcm[2 * i] != 1234 || pcm[2 * i + 1] != -2345;
    }
    received += frames;
    return frames;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(8);
    static uint8_t ram[8192];
    NcVm vm = {.phys = ram, .ram_size = sizeof(ram)};
    setenv("NC_APU_WORKERS", "1", 1);
    unsetenv("NC_TRACE_APU");
    /* Each preference twice: the bug hides after a preference change and
     * fails specifically when the next device uses the SAME preference. */
    const bool modes[] = {false, false, true, true, false};
    for (unsigned cycle = 0; cycle < ARRAY_SIZE(modes); cycle++) {
        printf("APU cycle %u, DSP %u\n", cycle + 1, modes[cycle]);
        setenv("NC_APU_DSP", modes[cycle] ? "1" : "0", 1);
        nc_dev_apu_init(&vm);
        MCPXAPUState *d = g_state;
        /* SECTL is off: the real frame thread remains parked. Hold its
         * lock while simulating the EP's FIFO DMA callback. */
        pthread_mutex_lock(&d->lock);
        check(d->monitor.point == (modes[cycle] ? MCPX_APU_DEBUG_MON_GP_OR_EP : MCPX_APU_DEBUG_MON_VP),
              "fresh APU selects an audible monitor");
        check(d->gp.realtime == modes[cycle] && d->ep.realtime == modes[cycle],
              "fresh APU applies the DSP execution mode");
        check(apu_out_queued() == 0, "new output ring starts empty");
        if (modes[cycle]) {
            int16_t pcm[256][2];
            for (unsigned i = 0; i < ARRAY_SIZE(pcm); i++) {
                pcm[i][0] = 1234;
                pcm[i][1] = -2345;
            }
            /* One scatter/gather entry and a 1024-byte output FIFO. */
            apu_wr32(ram, 4096);
            d->regs[NV_PAPU_EPFADDR] = 0;
            d->regs[NV_PAPU_EPFMAXSGE] = 0;
            d->regs[NV_PAPU_EPOFBASE0] = 0;
            d->regs[NV_PAPU_EPOFEND0] = sizeof(pcm);
            d->regs[NV_PAPU_EPOFCUR0] = 0;
            d->ep.dsp->dma.fifo_rw(d, (uint8_t *)pcm, 0, sizeof(pcm), true);
            apu_out_push(d->monitor.frame_buf, sizeof(d->monitor.frame_buf));
            received = mismatches = 0;
            nc_lr_audio_run(batch);
            printf("EP callback: %u frames, %u mismatched stereo samples\n", received, mismatches);
            check(received == 256 && mismatches == 0,
                  "EP PCM reaches the libretro callback unchanged after re-init");
        }
        pthread_mutex_unlock(&d->lock);
        nc_dev_apu_stop();
        nc_dev_apu_free();
        nc_mmio_reset();
        nc_irq_reset();
        received = 0;
        nc_lr_audio_run(batch);
        check(received == 0, "closed output makes no frontend callback");
    }
    return failures ? 1 : 0;
}
