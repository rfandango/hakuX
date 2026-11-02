/*
 * hakuX native core — the Xbox Live Communicator: the headset that plugs into
 * a controller's expansion slot, for voice chat (Xbox Live through Insignia,
 * and the games that talk over system link). A USB audio device of its own
 * class (0x78): 16-bit mono PCM on isochronous OUT 4 (the headphone) and IN 5
 * (the microphone), one packet a frame, at the rate the title picks with a
 * vendor SET_FEATURE (8 to 24 kHz; 16 kHz until then). Descriptors and
 * requests as xemu's (hw/xbox/xblc.c, Ryan Wendland and Fred Hallock), which
 * the titles' voice drivers run on there.
 *
 * Sound goes through AAudio: the phone's microphone, or the headset's when one
 * is plugged in (Android routes it), under the voice communication preset
 * (echo cancelling, noise suppression); the headphone's sound plays with the
 * game's. The USB frame thread only moves samples through two rings; the
 * streams open on a thread of their own when the title first moves audio (the
 * microphone isn't held until a game uses it), and again at each rate it sets.
 */
#include "native/dev/usb.h"

#include <aaudio/AAudio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define XBLC_EP_OUT 4
#define XBLC_EP_IN  5
#define XBLC_MAX_PACKET 48

#define XBLC_SET_SAMPLE_RATE 0
#define XBLC_SET_AGC         1

/* Samples queued at most: the microphone's 100 ms (as xemu: older ones are
 * dropped, the voice stays live), the headphone's 200 ms. */
#define RING_SAMPLES 8192

static const uint8_t k_dev_desc[18] = {
    18, 1, 0x10, 0x01, /* USB 1.1 */
    0, 0, 0,           /* class per interface */
    8,
    0x5e, 0x04, 0x83, 0x02, /* Microsoft, Xbox Live Communicator (0x0283) */
    0x10, 0x01, 1, 2, 3, 1,
};

static const uint8_t k_cfg_desc[45] = {
    9, 2, 45, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 0x78, 0, 0, 0,               /* headphone */
    9, 5, XBLC_EP_OUT, 0x01, XBLC_MAX_PACKET, 0, 1, 0, 0, /* isochronous OUT 4 */
    9, 4, 1, 0, 1, 0x78, 0, 0, 0,               /* microphone */
    9, 5, 0x80 | XBLC_EP_IN, 0x01, XBLC_MAX_PACKET, 0, 1, 0, 0, /* isochronous IN 5 */
};

static const char *const k_strings[] = {"hakuX", "Microsoft Xbox Live Communicator", "1"};

typedef struct Ring {
    int16_t s[RING_SAMPLES];
    unsigned head, count;
    unsigned max;
} Ring;

typedef struct Xblc {
    NcUsbDev dev;
    unsigned player;
    pthread_mutex_t lock;        /* the rings, rate and resampling state */
    Ring mic, phone;
    int rate;                    /* the title's */
    int mic_rate, phone_rate;    /* the streams' */
    double mic_pos, phone_pos;   /* the resamplers' fractional position */
    int16_t mic_last, phone_last;
    pthread_mutex_t streams;     /* opening and closing */
    AAudioStream *in, *out;
    bool started;                /* the title has moved audio: the streams are open */
    bool logged_no_mic;
} Xblc;

static void ring_put(Ring *r, int16_t v)
{
    if (r->count == r->max) {
        r->head = (r->head + 1) % RING_SAMPLES; /* full: the oldest goes */
        r->count--;
    }
    r->s[(r->head + r->count) % RING_SAMPLES] = v;
    r->count++;
}

static bool ring_get(Ring *r, int16_t *v)
{
    if (!r->count) {
        return false;
    }
    *v = r->s[r->head];
    r->head = (r->head + 1) % RING_SAMPLES;
    r->count--;
    return true;
}

/* Linear resampling from rate `from` to `to`. *pos is where the next output
 * sample reads, counting the previous call's last input (*last) as -1 and
 * this call's as 0..n-1; each reads between the inputs either side of it. */
static void resample_put(Ring *r, const int16_t *in, int n, int from, int to, double *pos, int16_t *last)
{
    if (from == to || from <= 0 || to <= 0) {
        for (int i = 0; i < n; i++) {
            ring_put(r, in[i]);
        }
        return;
    }
    if (n <= 0) {
        return;
    }
    const double step = (double)from / to;
    double p = *pos;
    while (p < n - 1) {
        const int i = p < 0 ? -1 : (int)p;
        const double f = p - i;
        const int a = i < 0 ? *last : in[i], b = in[i + 1];
        ring_put(r, (int16_t)(a + (b - a) * f));
        p += step;
    }
    *pos = p - n;
    *last = in[n - 1];
}

static aaudio_data_callback_result_t on_mic(AAudioStream *s, void *user, void *data, int32_t frames)
{
    Xblc *x = user;
    pthread_mutex_lock(&x->lock);
    resample_put(&x->mic, data, frames, x->mic_rate, x->rate, &x->mic_pos, &x->mic_last);
    pthread_mutex_unlock(&x->lock);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static aaudio_data_callback_result_t on_phone(AAudioStream *s, void *user, void *data, int32_t frames)
{
    Xblc *x = user;
    int16_t *out = data;
    pthread_mutex_lock(&x->lock);
    const double step = x->phone_rate > 0 ? (double)x->rate / x->phone_rate : 1.0;
    for (int32_t i = 0; i < frames; i++) {
        /* the title's samples, stretched to the stream's rate (silence when
         * it sends none: nobody talking) */
        x->phone_pos += step;
        while (x->phone_pos >= 1.0) {
            int16_t v;
            x->phone_last = ring_get(&x->phone, &v) ? v : 0;
            x->phone_pos -= 1.0;
        }
        out[i] = x->phone_last;
    }
    pthread_mutex_unlock(&x->lock);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static AAudioStream *open_stream(Xblc *x, bool mic, int rate)
{
    AAudioStreamBuilder *b = NULL;
    AAudioStream *s = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) {
        return NULL;
    }
    AAudioStreamBuilder_setDirection(b, mic ? AAUDIO_DIRECTION_INPUT : AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setChannelCount(b, 1);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    if (mic) {
        /* the platform's voice path: echo cancelling and noise suppression */
        AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
    } else {
        AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
        AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SPEECH);
    }
    AAudioStreamBuilder_setDataCallback(b, mic ? on_mic : on_phone, x);
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        if (mic && !x->logged_no_mic) {
            x->logged_no_mic = true;
            nc_log("hakuX-native-usb", "communicator %u: no microphone (AAudio: %s; the microphone permission?)",
                   x->player + 1, AAudio_convertResultToText(r));
        }
        return NULL;
    }
    if (AAudioStream_requestStart(s) != AAUDIO_OK) {
        AAudioStream_close(s);
        return NULL;
    }
    return s;
}

/* The streams at the title's rate (on a thread of its own: opening takes
 * tens of milliseconds, a USB frame one). */
static void *reopen_thread(void *arg)
{
    Xblc *x = arg;
    pthread_mutex_lock(&x->streams);
    pthread_mutex_lock(&x->lock);
    const int rate = x->rate;
    pthread_mutex_unlock(&x->lock);
    if (x->in) {
        AAudioStream_close(x->in);
        x->in = NULL;
    }
    if (x->out) {
        AAudioStream_close(x->out);
        x->out = NULL;
    }
    AAudioStream *in = open_stream(x, true, rate), *out = open_stream(x, false, rate);
    pthread_mutex_lock(&x->lock);
    x->mic_rate = in ? AAudioStream_getSampleRate(in) : 0;
    x->phone_rate = out ? AAudioStream_getSampleRate(out) : 0;
    x->mic.count = x->phone.count = 0;
    x->mic.max = (unsigned)rate / 10;          /* 100 ms */
    x->phone.max = (unsigned)rate / 5;         /* 200 ms */
    x->mic_pos = -1.0;
    x->phone_pos = 0;
    pthread_mutex_unlock(&x->lock);
    x->in = in;
    x->out = out;
    nc_log("hakuX-native-usb", "communicator %u: %d Hz (microphone %d Hz, headphone %d Hz)", x->player + 1, rate,
           x->mic_rate, x->phone_rate);
    pthread_mutex_unlock(&x->streams);
    return NULL;
}

static void reopen(Xblc *x)
{
    pthread_t t;
    if (pthread_create(&t, NULL, reopen_thread, x) == 0) {
        pthread_detach(t);
    }
}

static int xblc_control(NcUsbDev *d, const uint8_t *s, uint8_t *data, int len)
{
    Xblc *x = (Xblc *)d;
    const uint8_t type = s[0], req = s[1];
    const uint16_t value = nc_rd16(s + 2), index = nc_rd16(s + 4);
    if (type == 0x41 && req == 0x03) { /* vendor, interface: SET_FEATURE */
        if (index == XBLC_SET_SAMPLE_RATE) {
            static const int k_rates[] = {8000, 11025, 16000, 22050, 24000};
            const unsigned i = value & 0xff;
            if (i >= sizeof(k_rates) / sizeof(k_rates[0])) {
                return NC_USB_STALL;
            }
            pthread_mutex_lock(&x->lock);
            const bool changed = x->rate != k_rates[i] && x->started;
            x->rate = k_rates[i];
            pthread_mutex_unlock(&x->lock);
            if (changed) {
                reopen(x);
            }
            return 0;
        }
        if (index == XBLC_SET_AGC) {
            return 0;               /* the voice preset's own gain control does it */
        }
    }
    nc_log("hakuX-native-usb", "communicator: unhandled request %02x %02x value %04x index %04x", type, req, value,
           index);
    return NC_USB_STALL;
}

static int xblc_data(NcUsbDev *d, bool in, int ep, uint8_t *buf, int len)
{
    Xblc *x = (Xblc *)d;
    pthread_mutex_lock(&x->lock);
    const bool start = !x->started;
    x->started = true;
    int n = 0;
    if (in && ep == XBLC_EP_IN) {
        /* what the microphone has, up to the packet (an empty one: nothing yet) */
        int16_t v;
        while (n + 2 <= len && ring_get(&x->mic, &v)) {
            buf[n] = (uint8_t)v;
            buf[n + 1] = (uint8_t)(v >> 8);
            n += 2;
        }
    } else if (!in && ep == XBLC_EP_OUT) {
        for (; n + 2 <= len; n += 2) {
            ring_put(&x->phone, (int16_t)(buf[n] | buf[n + 1] << 8));
        }
    }
    pthread_mutex_unlock(&x->lock);
    if (start) {
        reopen(x);
    }
    return n;
}

static void xblc_reset(NcUsbDev *d)
{
    Xblc *x = (Xblc *)d;
    pthread_mutex_lock(&x->lock);
    x->mic.count = x->phone.count = 0;
    pthread_mutex_unlock(&x->lock);
}

NcUsbDev *nc_xblc_new(unsigned player)
{
    Xblc *x = calloc(1, sizeof(Xblc));
    x->player = player;
    pthread_mutex_init(&x->lock, NULL);
    pthread_mutex_init(&x->streams, NULL);
    x->rate = 16000;                /* the Communicator's until the title sets one */
    x->mic.max = x->rate / 10;
    x->phone.max = x->rate / 5;
    x->dev.name = "communicator";
    x->dev.dev_desc = k_dev_desc;
    x->dev.dev_desc_len = sizeof(k_dev_desc);
    x->dev.cfg_desc = k_cfg_desc;
    x->dev.cfg_desc_len = sizeof(k_cfg_desc);
    x->dev.strings = k_strings;
    x->dev.nstrings = 3;
    x->dev.control = xblc_control;
    x->dev.data = xblc_data;
    x->dev.reset = xblc_reset;
    x->dev.pending_addr = -1;
    return &x->dev;
}
