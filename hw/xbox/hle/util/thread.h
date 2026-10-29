/*
 * hakuX HLE GPU — threads, locks, events and timers over pthreads, and the
 * monotonic clock.
 */
#ifndef HW_XBOX_HLE_UTIL_THREAD_H
#define HW_XBOX_HLE_UTIL_THREAD_H

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef struct HleMutex {
    pthread_mutex_t m;
} HleMutex;

typedef struct HleCond {
    pthread_cond_t c;
} HleCond;

typedef struct HleSem {
    sem_t s;
} HleSem;

/* Set / reset by hand; waiters wait for "set". */
typedef struct HleEvent {
    pthread_mutex_t m;
    pthread_cond_t c;
    bool set;
} HleEvent;

typedef struct HleThread {
    pthread_t t;
} HleThread;

enum { HLE_THREAD_JOINABLE, HLE_THREAD_DETACHED };

static inline void hle_mutex_init(HleMutex *m)
{
    pthread_mutex_init(&m->m, NULL);
}
static inline void hle_mutex_destroy(HleMutex *m)
{
    pthread_mutex_destroy(&m->m);
}
static inline void hle_mutex_lock(HleMutex *m)
{
    pthread_mutex_lock(&m->m);
}
static inline void hle_mutex_unlock(HleMutex *m)
{
    pthread_mutex_unlock(&m->m);
}

void hle_cond_init(HleCond *c);
void hle_cond_destroy(HleCond *c);
static inline void hle_cond_signal(HleCond *c)
{
    pthread_cond_signal(&c->c);
}
static inline void hle_cond_broadcast(HleCond *c)
{
    pthread_cond_broadcast(&c->c);
}
static inline void hle_cond_wait(HleCond *c, HleMutex *m)
{
    pthread_cond_wait(&c->c, &m->m);
}
/* False on timeout. */
bool hle_cond_timedwait(HleCond *c, HleMutex *m, int ms);

void hle_sem_init(HleSem *s, int init);
void hle_sem_destroy(HleSem *s);
void hle_sem_post(HleSem *s);
void hle_sem_wait(HleSem *s);
/* Takes a post if one is there (true), without waiting. */
bool hle_sem_trywait(HleSem *s);
/* False on timeout. */
bool hle_sem_timedwait(HleSem *s, int ms);

void hle_event_init(HleEvent *e, bool set);
void hle_event_destroy(HleEvent *e);
void hle_event_set(HleEvent *e);
void hle_event_reset(HleEvent *e);
void hle_event_wait(HleEvent *e);

/* An 8 MiB stack (the code was written for desktop thread stacks; bionic's
 * default is 1 MiB), named for the profilers. */
void hle_thread_create(HleThread *t, const char *name, void *(*fn)(void *), void *arg, int mode);
/* The timers' thread stopped and joined and every timer freed (the renderer
 * stopping: their owners forget them); the next hle_timer_new starts it. */
void hle_timers_stop(void);
void *hle_thread_join(HleThread *t);

/* The monotonic clock. */
static inline int64_t hle_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static inline int64_t hle_now_ms(void)
{
    return hle_now_ns() / 1000000;
}

/* One-shot timers on the HLE's timer thread (diagnostics: their callbacks
 * run there, with no lock held). */
typedef struct HleTimer HleTimer;
HleTimer *hle_timer_new(void (*cb)(void *), void *opaque);
/* Fires once at deadline_ms (hle_now_ms's clock); re-arm from the callback. */
void hle_timer_mod(HleTimer *t, int64_t deadline_ms);

#endif
