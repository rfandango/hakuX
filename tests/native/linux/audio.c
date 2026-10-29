#include "native/dev/apu/apu_int.h"
#include <stdio.h>

void nc_audio_set_paused(bool on);

#define MIN_DRAINED 48000
#define MIN_CALLBACKS 25

static void check(bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        exit(1);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* it leaves by _exit */
    alarm(5);
    /* static: the output's thread may still broadcast while the process exits */
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
    static uint8_t silence[192000];
    nc_audio_set_paused(true);
    check(apu_out_open(sizeof(silence), 480, &wake) == 1920, "480-frame diagnostic callback");
    apu_out_push(silence, sizeof(silence));
    usleep(50000);
    check(apu_out_queued() == sizeof(silence), "paused sink preserves a full ring");
    pthread_mutex_lock(&lock);
    nc_audio_set_paused(false);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec++;
    check(pthread_cond_timedwait(&wake, &lock, &deadline) == 0, "full-ring producer receives drain wakeup");
    pthread_mutex_unlock(&lock);
    int before = apu_out_queued();
    usleep(300000);
    nc_audio_set_paused(true);
    int after = apu_out_queued();
    printf("drained %d bytes in approximately 300 ms (expected 57600)\n", before - after);
    check(before - after >= MIN_DRAINED && before - after <= 67200, "output drains at 48 kHz stereo, not as fast as possible");
    uint32_t cb, short_cb;
    apu_out_stats(&cb, &short_cb);
    check(cb >= MIN_CALLBACKS && short_cb == 0, "buffered output has no short callbacks");
    usleep(100000);
    check(apu_out_queued() == after, "pause acknowledges before returning");
    apu_out_stats(&cb, &short_cb);
    check(cb == 0, "no callbacks during pause");
    nc_pause_set(true);
    nc_audio_set_paused(false);
    usleep(50000);
    check(apu_out_queued() == after, "core pause also preserves queued audio");
    nc_pause_set(false);
    usleep(50000);
    check(apu_out_queued() < after, "audio resumes after core pause");
    /* As hakux leaves: the output's thread stopped, then no teardown (static
     * destructors, the stack's cond) with it still running. */
    nc_audio_set_paused(true);
    _exit(0);
}
