/*
 * hakuX native core — the APU's output: 48 kHz 16-bit stereo on AAudio.
 *
 * The frame thread pushes each EP frame (256 samples, 1024 bytes) into a
 * ring; AAudio's data callback drains it. One producer and one consumer, so
 * the ring is two counters: the producer publishes its writes with the head
 * (release), the callback its reads with the tail. A stream lost (AAudio's
 * error callback: a headset or Bluetooth device coming or going, a route
 * change) or none opened: a thread drains the ring at the device's rate
 * instead (the sound goes nowhere, the APU keeps its timing) and opens a
 * stream again (reopen_thread).
 */
#include "native/dev/apu/apu_int.h"

#ifdef NC_DESKTOP
#include <SDL3/SDL.h>
#else
#include <aaudio/AAudio.h>
#endif

#define OUT_RATE 48000
/* The channels out: 2, or 6 for 5.1 (nc_audio_set_channels: FL FR C LFE BL BR,
 * SDL's order and the Xbox's mixbins'); a sample frame: 16 bits each. */
int g_apu_out_channels = 2;
int16_t g_apu_frame6[256][6];
#define OUT_FRAME_BYTES (2 * g_apu_out_channels)

/* The channels out (2 or 6), before the APU starts: SDL mixes six down to
 * the device's speakers when it has fewer. AAudio's stream stays stereo. */
void nc_audio_set_channels(int n)
{
#ifdef NC_DESKTOP
    g_apu_out_channels = n == 6 ? 6 : 2;
#else
    (void)n;
#endif
}

#define REOPEN_NS 1000000000LL  /* a failed open is tried again after this */

static struct {
    uint8_t *buf;
    uint32_t cap;
    uint64_t head, tail;     /* bytes written, bytes read */
    int samples;             /* the callback's size, in frames */
    pthread_cond_t *wake;
    pthread_mutex_t mu;      /* stream, app_paused: the activity's thread, reopen_thread */
#ifndef NC_DESKTOP
    AAudioStream *stream;    /* NULL: none (reopen_thread drains the ring) */
#endif
    bool started, app_paused;
    bool reopening;          /* reopen_thread runs */
    uint32_t callbacks, short_callbacks;
} g_out = { .mu = PTHREAD_MUTEX_INITIALIZER };

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

static int pop(uint8_t *out, int bytes)
{
    const uint64_t tail = g_out.tail;
    const int n = (int)MIN((uint64_t)bytes, apu_ld_acq(&g_out.head) - tail);
    const uint32_t at = (uint32_t)(tail % g_out.cap);
    const uint32_t first = MIN((uint32_t)n, g_out.cap - at);
    memcpy(out, g_out.buf + at, first);
    memcpy(out + first, g_out.buf, (uint32_t)n - first);
    apu_st_rel(&g_out.tail, tail + (uint64_t)n);
    return n;
}

void apu_out_stats(uint32_t *callbacks, uint32_t *short_callbacks)
{
    *callbacks = apu_xchg(&g_out.callbacks, 0);
    *short_callbacks = apu_xchg(&g_out.short_callbacks, 0);
}

/* One callback's worth: what the ring has, the rest silence (a short
 * callback). It never waits: the callback runs on the device's real-time
 * thread, and the frame thread keeps one to three callbacks queued (apu.c's
 * watermarks). Paused (nc_pause_set), silence, the ring kept: the frame
 * thread stops at its high watermark. */
static void fill(uint8_t *stream, int bytes)
{
    if (nc_paused()) {
        memset(stream, 0, (size_t)bytes);
        return;
    }
    const int n = pop(stream, bytes);
    apu_inc(&g_out.callbacks);
    if (n < bytes) {
        apu_inc(&g_out.short_callbacks);
        memset(stream + n, 0, (size_t)(bytes - n));
    }
    pthread_cond_broadcast(g_out.wake);
}

#ifndef NC_DESKTOP
static aaudio_data_callback_result_t on_data(AAudioStream *s, void *user, void *data, int32_t frames)
{
    fill(data, frames * OUT_FRAME_BYTES);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void on_error(AAudioStream *s, void *user, aaudio_result_t error);

/* A stream on the default device, not started; NULL (*r: why) if none opens */
static AAudioStream *open_stream(aaudio_result_t *r)
{
    AAudioStreamBuilder *b = NULL;
    AAudioStream *s = NULL;
    *r = AAudio_createStreamBuilder(&b);
    if (*r == AAUDIO_OK) {
        AAudioStreamBuilder_setSampleRate(b, OUT_RATE);
        AAudioStreamBuilder_setChannelCount(b, 2);
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setFramesPerDataCallback(b, g_out.samples);
        AAudioStreamBuilder_setDataCallback(b, on_data, NULL);
        AAudioStreamBuilder_setErrorCallback(b, on_error, NULL);
        *r = AAudioStreamBuilder_openStream(b, &s);
        AAudioStreamBuilder_delete(b);
        if (*r != AAUDIO_OK) {
            s = NULL;
        }
    }
    return s;
}

/* g_out.mu held: the stream started (false: it would not) */
static bool start_locked(void)
{
    const aaudio_result_t r = AAudioStream_requestStart(g_out.stream);
    if (r != AAUDIO_OK) {
        nc_log("hakuX-apu", "output stream: start failed (%s)", AAudio_convertResultToText(r));
    }
    return r == AAUDIO_OK;
}

/* The stream lost or none opened: the one in g_out.stream (if any) closed,
 * then a stream opened again every REOPEN_NS until one starts. Meanwhile
 * this thread drains the ring at the device's rate (one consumer at a time:
 * the old stream is closed before, and the new one started after). */
static void *reopen_thread(void *arg)
{
    pthread_mutex_lock(&g_out.mu);
    AAudioStream *old = g_out.stream;
    g_out.stream = NULL;
    pthread_mutex_unlock(&g_out.mu);
    if (old) {
        AAudioStream_requestStop(old);
        AAudioStream_close(old);        /* no more callbacks from it */
    }

    static uint8_t buf[16384];
    const int len = MIN(g_out.samples * OUT_FRAME_BYTES, (int)sizeof(buf));
    const long period_ns = (long)((int64_t)(len / OUT_FRAME_BYTES) * 1000000000LL / OUT_RATE);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int64_t waited_ns = REOPEN_NS;      /* since the last try: try at once */
    bool said = false;
    for (;;) {
        if (waited_ns >= REOPEN_NS) {
            waited_ns = 0;
            aaudio_result_t r;
            AAudioStream *s = open_stream(&r);
            if (s) {
                pthread_mutex_lock(&g_out.mu);
                g_out.stream = s;
                const bool ok = g_out.app_paused || start_locked();
                if (ok) {
                    /* a later loss starts a new reopen_thread */
                    __atomic_store_n(&g_out.reopening, false, __ATOMIC_RELEASE);
                } else {
                    g_out.stream = NULL;
                }
                pthread_mutex_unlock(&g_out.mu);
                if (ok) {
                    nc_log("hakuX-apu", "output stream open again");
                    return NULL;
                }
                AAudioStream_close(s);
            } else if (!said) {
                said = true;
                nc_log("hakuX-apu", "no output stream (AAudio: %s); draining without sound, "
                       "trying again each second", AAudio_convertResultToText(r));
            }
        }
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        fill(buf, len);
        waited_ns += period_ns;
    }
}

/* false: reopen_thread already runs */
static bool reopen(void)
{
    if (__atomic_exchange_n(&g_out.reopening, true, __ATOMIC_ACQ_REL)) {
        return false;
    }
    pthread_t t;
    apu_thread_start(&t, "apu.reopen", reopen_thread, NULL, false);
    return true;
}

/* AAudio's own thread, after the stream is lost (disconnected): it is
 * closed and opened again on another thread, never in this callback. One
 * from a stream no longer current (reopen_thread clears it before closing
 * it; none come after the close) is ignored. */
static void on_error(AAudioStream *s, void *user, aaudio_result_t error)
{
    pthread_mutex_lock(&g_out.mu);
    const bool current = s == g_out.stream;
    pthread_mutex_unlock(&g_out.mu);
    if (current && reopen()) {
        nc_log("hakuX-apu", "output stream lost (%s); opening it again",
               AAudio_convertResultToText(error));
    }
}

int apu_out_open(int capacity_bytes, int samples, pthread_cond_t *wake)
{
    g_out.buf = calloc(1, (size_t)capacity_bytes);
    g_out.cap = (uint32_t)capacity_bytes;
    g_out.samples = samples;
    g_out.wake = wake;

    aaudio_result_t r;
    AAudioStream *s = open_stream(&r);
    pthread_mutex_lock(&g_out.mu);
    g_out.started = true;
    g_out.stream = s;
    const bool ok = s && (g_out.app_paused || start_locked());
    pthread_mutex_unlock(&g_out.mu);
    if (!ok) {
        reopen();
    }
    return samples * OUT_FRAME_BYTES;
}

/* The emulation paused with the activity (app_main.c): the stream stops
 * pulling silence meanwhile, and starts again on resuming. */
void nc_audio_set_paused(bool on)
{
    pthread_mutex_lock(&g_out.mu);
    g_out.app_paused = on;
    if (g_out.stream && g_out.started) {
        if (on) {
            AAudioStream_requestPause(g_out.stream);
        } else if (!start_locked()) {
            reopen();
        }
    }
    pthread_mutex_unlock(&g_out.mu);
}
#else /* NC_DESKTOP (Windows, Linux): SDL3's audio stream on the default device */

/* SDL follows the default device itself (a headset plugged in, the default
 * changed in Windows' settings): one stream for the run. Its callback asks
 * for what the device will play next; the ring gives it, padded with
 * silence as AAudio's callback was. */
static SDL_AudioStream *g_sdl;
static float g_volume = 1.0f;

static void SDLCALL on_sdl(void *user, SDL_AudioStream *s, int additional, int total)
{
    static uint8_t buf[16384];
    while (additional > 0) {
        int n = MIN(additional, (int)sizeof(buf));
        n -= n % OUT_FRAME_BYTES;
        if (n <= 0) {
            break;
        }
        fill(buf, n);
        SDL_PutAudioStreamData(s, buf, n);
        additional -= n;
    }
}

/* No device at all: the ring drained at the device's rate, without sound. */
static void *drain_thread(void *arg)
{
    static uint8_t buf[16384];
    const int len = MIN(g_out.samples * OUT_FRAME_BYTES, (int)sizeof(buf));
    const int64_t period_ns = (int64_t)(len / OUT_FRAME_BYTES) * 1000000000LL / OUT_RATE;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int64_t next = (int64_t)t0.tv_sec * 1000000000LL + t0.tv_nsec;
    for (;;) {
        next += period_ns;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const int64_t left = next - ((int64_t)now.tv_sec * 1000000000LL + now.tv_nsec);
        if (left > 0) {
            usleep((unsigned long long)(left / 1000));
        }
        fill(buf, len);
    }
    return NULL;
}

int apu_out_open(int capacity_bytes, int samples, pthread_cond_t *wake)
{
    g_out.buf = calloc(1, (size_t)capacity_bytes);
    g_out.cap = (uint32_t)capacity_bytes;
    g_out.samples = samples;
    g_out.wake = wake;
    g_out.started = true;

    char frames[16];
    snprintf(frames, sizeof(frames), "%d", samples);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
    const SDL_AudioSpec spec = {SDL_AUDIO_S16LE, g_apu_out_channels, OUT_RATE};
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        g_sdl = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, on_sdl, NULL);
    }
    if (!g_sdl) {
        nc_log("hakuX-apu", "no output stream (SDL: %s); draining without sound", SDL_GetError());
        pthread_t t;
        apu_thread_start(&t, "apu.drain", drain_thread, NULL, false);
    } else {
        pthread_mutex_lock(&g_out.mu);
        if (!g_out.app_paused) {
            SDL_ResumeAudioStreamDevice(g_sdl);
        }
        SDL_SetAudioStreamGain(g_sdl, g_volume);
        pthread_mutex_unlock(&g_out.mu);
        nc_log("hakuX-apu", "output stream: %s, %d frames a callback asked, %d channels",
               SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(g_sdl)), samples, g_apu_out_channels);
    }
    return samples * OUT_FRAME_BYTES;
}

/* The output's gain (0-1, the front end's volume), before or after it opens. */
void nc_audio_set_volume(float v)
{
    g_volume = v < 0 ? 0 : v > 1 ? 1 : v;
    if (g_sdl) {
        SDL_SetAudioStreamGain(g_sdl, g_volume);
    }
}

/* The emulation paused (the window minimized, the front end's pause): the
 * device stops pulling silence meanwhile, and starts again on resuming. */
void nc_audio_set_paused(bool on)
{
    pthread_mutex_lock(&g_out.mu);
    g_out.app_paused = on;
    if (g_sdl) {
        if (on) {
            SDL_PauseAudioStreamDevice(g_sdl);
        } else {
            SDL_ResumeAudioStreamDevice(g_sdl);
        }
    }
    pthread_mutex_unlock(&g_out.mu);
}
#endif
