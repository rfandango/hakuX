/* Paced silent output for Linux diagnostics. The emulated APU stays active. */
#include "native/dev/apu/apu_int.h"

static struct {
    uint32_t capacity;
    int samples;
    uint64_t head, tail;
    pthread_cond_t *wake;
    pthread_mutex_t lock;
    bool paused;
    uint32_t callbacks, short_callbacks;
    pthread_t thread;
    int quit;                   /* apu_out_close */
} g_null = {.lock = PTHREAD_MUTEX_INITIALIZER};

int apu_out_queued(void)
{
    return (int)(__atomic_load_n(&g_null.head, __ATOMIC_ACQUIRE) -
                 __atomic_load_n(&g_null.tail, __ATOMIC_ACQUIRE));
}

void apu_out_push(const void *frame, int bytes)
{
    (void)frame;
    uint64_t head = __atomic_load_n(&g_null.head, __ATOMIC_RELAXED);
    if (bytes < 0 || head + (uint64_t)bytes - __atomic_load_n(&g_null.tail, __ATOMIC_ACQUIRE) > g_null.capacity) {
        nc_log("hakuX-apu", "diagnostic audio: invalid push or ring overflow");
        abort();
    }
    __atomic_store_n(&g_null.head, head + (uint64_t)bytes, __ATOMIC_RELEASE);
}

void apu_out_stats(uint32_t *callbacks, uint32_t *short_callbacks)
{
    *callbacks = __atomic_exchange_n(&g_null.callbacks, 0, __ATOMIC_RELAXED);
    *short_callbacks = __atomic_exchange_n(&g_null.short_callbacks, 0, __ATOMIC_RELAXED);
}

static void *drain(void *unused)
{
    const int bytes = g_null.samples * 4;
    const int64_t period = (int64_t)g_null.samples * 1000000000 / 48000;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!__atomic_load_n(&g_null.quit, __ATOMIC_ACQUIRE)) {
        next.tv_nsec += period;
        next.tv_sec += next.tv_nsec / 1000000000;
        next.tv_nsec %= 1000000000;
        int error;
        do {
            error = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        } while (error == EINTR);
        if (error) {
            nc_log("hakuX-apu", "diagnostic audio sleep failed: %d", error);
            abort();
        }
        pthread_mutex_lock(&g_null.lock);
        if (!g_null.paused && !nc_paused()) {
            uint64_t tail = __atomic_load_n(&g_null.tail, __ATOMIC_RELAXED);
            uint64_t available = __atomic_load_n(&g_null.head, __ATOMIC_ACQUIRE) - tail;
            uint64_t n = MIN(available, (uint64_t)bytes);
            __atomic_store_n(&g_null.tail, tail + n, __ATOMIC_RELEASE);
            __atomic_fetch_add(&g_null.callbacks, 1, __ATOMIC_RELAXED);
            if (n < (uint64_t)bytes) {
                __atomic_fetch_add(&g_null.short_callbacks, 1, __ATOMIC_RELAXED);
            }
            pthread_cond_broadcast(g_null.wake);
        }
        pthread_mutex_unlock(&g_null.lock);
        /* Suspension or a long scheduling gap must not cause a burst of
         * catch-up drains. Resume with one period of audio per wake. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((int64_t)(now.tv_sec - next.tv_sec) * 1000000000 + now.tv_nsec - next.tv_nsec > period) {
            next = now;
        }
    }
    return NULL;
}

int apu_out_open(int capacity_bytes, int samples, pthread_cond_t *wake)
{
    if (capacity_bytes <= 0 || capacity_bytes % 4 || samples <= 0 || samples > 4096 || !wake || g_null.capacity) {
        nc_log("hakuX-apu", "diagnostic audio: invalid configuration or duplicate open");
        abort();
    }
    g_null.capacity = (uint32_t)capacity_bytes;
    g_null.samples = samples;
    g_null.wake = wake;
    g_null.quit = 0;
    int error = pthread_create(&g_null.thread, NULL, drain, NULL);
    if (error) {
        nc_log("hakuX-apu", "diagnostic audio thread failed: %d", error);
        abort();
    }
    nc_log("hakuX-apu", "diagnostic audio: paced silent sink, 48000 Hz stereo, %d samples/callback", samples);
    return samples * 4;
}

void apu_out_close(void)
{
    if (!g_null.capacity) {
        return;
    }
    __atomic_store_n(&g_null.quit, 1, __ATOMIC_RELEASE);
    pthread_join(g_null.thread, NULL);
    pthread_mutex_lock(&g_null.lock);
    g_null.capacity = 0;
    g_null.samples = 0;
    g_null.head = g_null.tail = 0;
    g_null.wake = NULL;
    pthread_mutex_unlock(&g_null.lock);
}

void nc_audio_set_paused(bool on)
{
    pthread_mutex_lock(&g_null.lock);
    g_null.paused = on;
    pthread_mutex_unlock(&g_null.lock);
}
