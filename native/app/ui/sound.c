/*
 * hakuX front end — the dashboard's sounds (sound.h). Each a few hundred
 * ms of float stereo at 48 kHz: sine sweeps with a soft attack, an
 * exponential tail and a short echo, a filtered-noise whoosh for a launch.
 */
#include "native/app/ui/sound.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RATE 48000

static struct {
    SDL_AudioStream *st;
    float *buf[SND_COUNT];
    int len[SND_COUNT];     /* frames */
    bool on;
} S;

/* a tone: frequency f0 -> f1 over its length, amplitude a, decay d (1/s) */
static void tone(float *o, int n, float at, float f0, float f1, float a, float d)
{
    double ph = 0;
    const int start = (int)(at * RATE);
    for (int i = 0; start + i < n; i++) {
        const float t = (float)i / RATE, k = (float)i / (n - start);
        const float f = f0 + (f1 - f0) * k;
        ph += 2 * M_PI * f / RATE;
        const float env = fminf(1.0f, t * 400) * expf(-d * t);
        /* a sine with a little of its octave: glassy */
        const float s = (float)(sin(ph) + 0.25 * sin(2 * ph) + 0.08 * sin(3 * ph)) * a * env;
        o[(start + i) * 2] += s;
        o[(start + i) * 2 + 1] += s;
    }
}

static void echo(float *o, int n, float delay, float g)
{
    const int dl = (int)(delay * RATE);
    for (int i = dl; i < n; i++) {
        o[i * 2] += o[(i - dl) * 2 + 1] * g;    /* crossed: a little width */
        o[i * 2 + 1] += o[(i - dl) * 2] * g;
    }
}

static float *make(int which, int *frames)
{
    static const float k_len[SND_COUNT] = {0.16f, 0.42f, 0.30f, 0.20f, 0.36f, 1.1f};
    const int n = (int)(k_len[which] * RATE);
    float *o = calloc((size_t)n * 2, sizeof(float));
    switch (which) {
    case SND_MOVE:
        tone(o, n, 0, 1800, 2400, 0.10f, 38);
        tone(o, n, 0, 900, 1200, 0.05f, 30);
        echo(o, n, 0.045f, 0.3f);
        break;
    case SND_SELECT:
        tone(o, n, 0, 880, 900, 0.14f, 14);
        tone(o, n, 0.07f, 1320, 1360, 0.13f, 12);
        tone(o, n, 0.14f, 1760, 1800, 0.10f, 10);
        echo(o, n, 0.08f, 0.35f);
        break;
    case SND_BACK:
        tone(o, n, 0, 1320, 700, 0.13f, 14);
        echo(o, n, 0.07f, 0.3f);
        break;
    case SND_TAB:
        tone(o, n, 0, 600, 1500, 0.10f, 22);
        echo(o, n, 0.05f, 0.3f);
        break;
    case SND_ERROR:
        tone(o, n, 0, 220, 200, 0.16f, 8);
        tone(o, n, 0.12f, 180, 170, 0.14f, 8);
        break;
    case SND_LAUNCH: {
        /* a rising whoosh: noise through a sweeping one-pole low-pass, and a chord */
        uint32_t r = 12345;
        float lp = 0;
        for (int i = 0; i < n; i++) {
            const float k = (float)i / n;
            r = r * 1664525u + 1013904223u;
            const float noise = ((r >> 9) / 4194304.0f - 1);
            const float cut = 0.01f + 0.25f * k;
            lp += (noise - lp) * cut;
            const float env = sinf((float)M_PI * k) * 0.25f;
            o[i * 2] += lp * env;
            o[i * 2 + 1] += lp * env;
        }
        tone(o, n, 0.25f, 440, 880, 0.08f, 3);
        tone(o, n, 0.35f, 660, 1320, 0.07f, 3);
        echo(o, n, 0.11f, 0.35f);
        break;
    }
    }
    *frames = n;
    return o;
}

void snd_init(void)
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return;
    }
    const SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, RATE};
    S.st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!S.st) {
        return;
    }
    for (int i = 0; i < SND_COUNT; i++) {
        S.buf[i] = make(i, &S.len[i]);
    }
    SDL_ResumeAudioStreamDevice(S.st);
    S.on = true;
}

void snd_enable(bool on)
{
    S.on = on;
}

void snd_play(int which)
{
    if (!S.st || !S.on || which < 0 || which >= SND_COUNT) {
        return;
    }
    /* a new sound cuts what is still queued: navigation stays crisp */
    SDL_ClearAudioStream(S.st);
    SDL_PutAudioStreamData(S.st, S.buf[which], S.len[which] * 2 * (int)sizeof(float));
}

void snd_shutdown(void)
{
    if (S.st) {
        SDL_DestroyAudioStream(S.st);
    }
    for (int i = 0; i < SND_COUNT; i++) {
        free(S.buf[i]);
    }
    memset(&S, 0, sizeof(S));
}
