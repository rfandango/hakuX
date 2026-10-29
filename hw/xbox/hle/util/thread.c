/*
 * hakuX HLE GPU — threads, events and timers over pthreads (thread.h).
 */
#include "hw/xbox/hle/util/thread.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static void monotonic_cond_init(pthread_cond_t *c)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
}

void hle_cond_init(HleCond *c)
{
    monotonic_cond_init(&c->c);
}

void hle_cond_destroy(HleCond *c)
{
    pthread_cond_destroy(&c->c);
}

bool hle_cond_timedwait(HleCond *c, HleMutex *m, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    return pthread_cond_timedwait(&c->c, &m->m, &ts) == 0;
}

void hle_sem_init(HleSem *s, int init)
{
    sem_init(&s->s, 0, (unsigned)init);
}

void hle_sem_destroy(HleSem *s)
{
    sem_destroy(&s->s);
}

void hle_sem_post(HleSem *s)
{
    sem_post(&s->s);
}

void hle_sem_wait(HleSem *s)
{
    while (sem_wait(&s->s) != 0 && errno == EINTR) {
    }
}

bool hle_sem_trywait(HleSem *s)
{
    return sem_trywait(&s->s) == 0;
}

bool hle_sem_timedwait(HleSem *s, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    int r;
    while ((r = sem_timedwait(&s->s, &ts)) != 0 && errno == EINTR) {
    }
    return r == 0;
}

void hle_event_init(HleEvent *e, bool set)
{
    pthread_mutex_init(&e->m, NULL);
    monotonic_cond_init(&e->c);
    e->set = set;
}

void hle_event_destroy(HleEvent *e)
{
    pthread_cond_destroy(&e->c);
    pthread_mutex_destroy(&e->m);
}

void hle_event_set(HleEvent *e)
{
    pthread_mutex_lock(&e->m);
    e->set = true;
    pthread_cond_broadcast(&e->c);
    pthread_mutex_unlock(&e->m);
}

void hle_event_reset(HleEvent *e)
{
    pthread_mutex_lock(&e->m);
    e->set = false;
    pthread_mutex_unlock(&e->m);
}

void hle_event_wait(HleEvent *e)
{
    pthread_mutex_lock(&e->m);
    while (!e->set) {
        pthread_cond_wait(&e->c, &e->m);
    }
    pthread_mutex_unlock(&e->m);
}

void hle_thread_create(HleThread *t, const char *name, void *(*fn)(void *), void *arg, int mode)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_create(&t->t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (name) {
        char n[16];
        snprintf(n, sizeof(n), "%s", name);
        pthread_setname_np(t->t, n);
    }
    if (mode == HLE_THREAD_DETACHED) {
        pthread_detach(t->t);
    }
}

void *hle_thread_join(HleThread *t)
{
    void *r = NULL;
    pthread_join(t->t, &r);
    return r;
}

/* ---- timers ---------------------------------------------------------------- */

struct HleTimer {
    void (*cb)(void *);
    void *opaque;
    int64_t deadline_ms;        /* -1: not armed */
    HleTimer *next;
};

static pthread_mutex_t g_tm = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tc;
static HleTimer *g_timers;
static bool g_tstarted, g_tquit;    /* under g_tm (hle_timers_stop) */
static pthread_t g_tthread;

static void *timer_thread(void *arg)
{
    pthread_mutex_lock(&g_tm);
    while (!g_tquit) {
        int64_t now = hle_now_ms(), next = INT64_MAX;
        HleTimer *due = NULL;
        for (HleTimer *t = g_timers; t; t = t->next) {
            if (t->deadline_ms >= 0 && t->deadline_ms <= now) {
                due = t;
                break;
            }
            if (t->deadline_ms >= 0 && t->deadline_ms < next) {
                next = t->deadline_ms;
            }
        }
        if (due) {
            due->deadline_ms = -1;
            pthread_mutex_unlock(&g_tm);
            due->cb(due->opaque);
            pthread_mutex_lock(&g_tm);
            continue;
        }
        if (next == INT64_MAX) {
            pthread_cond_wait(&g_tc, &g_tm);
        } else {
            struct timespec ts = { (time_t)(next / 1000), (long)(next % 1000) * 1000000 };
            pthread_cond_timedwait(&g_tc, &g_tm, &ts);
        }
    }
    pthread_mutex_unlock(&g_tm);
    return arg;
}

/* g_tm held */
static void timers_start(void)
{
    monotonic_cond_init(&g_tc);
    g_tquit = false;
    if (pthread_create(&g_tthread, NULL, timer_thread, NULL) == 0) {
        pthread_setname_np(g_tthread, "hle-timers");
        g_tstarted = true;
    }
}

void hle_timers_stop(void)
{
    pthread_mutex_lock(&g_tm);
    const bool started = g_tstarted;
    g_tquit = true;
    if (started) {
        pthread_cond_broadcast(&g_tc);
    }
    pthread_mutex_unlock(&g_tm);
    if (started) {
        pthread_join(g_tthread, NULL);  /* after the callback it may be in */
        pthread_cond_destroy(&g_tc);
    }
    pthread_mutex_lock(&g_tm);
    while (g_timers) {
        HleTimer *t = g_timers;
        g_timers = t->next;
        free(t);
    }
    g_tstarted = g_tquit = false;
    pthread_mutex_unlock(&g_tm);
}

HleTimer *hle_timer_new(void (*cb)(void *), void *opaque)
{
    HleTimer *t = calloc(1, sizeof(*t));
    t->cb = cb;
    t->opaque = opaque;
    t->deadline_ms = -1;
    pthread_mutex_lock(&g_tm);
    if (!g_tstarted) {
        timers_start();
    }
    t->next = g_timers;
    g_timers = t;
    pthread_mutex_unlock(&g_tm);
    return t;
}

void hle_timer_mod(HleTimer *t, int64_t deadline_ms)
{
    pthread_mutex_lock(&g_tm);
    t->deadline_ms = deadline_ms;
    pthread_cond_broadcast(&g_tc);
    pthread_mutex_unlock(&g_tm);
}
