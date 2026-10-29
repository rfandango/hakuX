/*
 * hakuX HLE GPU — asynchronous pipeline builds, the per-title pipeline
 * record and its prebuild (pipeasync.h).
 *
 * Workers: WORKERS threads below normal priority, so the guest's CPU and the
 * render thread keep the big cores; jobs carry copies of their key and
 * vertex program, results go back to the render thread (hle_pipe_take).
 *
 * The record: a header (magic, version, key size), then records appended as
 * the session makes pipelines: a vertex program (once per file), a pipeline
 * (its key, its program's hash, when it was last used) and a touch (a
 * prebuilt pipeline used again). Read at the title's start, it is written
 * back compact: each pipeline once with its latest use, the newest REC_KEEP.
 */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"

#include "hw/xbox/hle/vk/pipeasync.h"
#include "hw/xbox/hle/vk/vkdev.h"

#ifdef __ANDROID__
#include <sys/system_properties.h>
#include "hw/xbox/hle/prop.h"
#endif
#ifdef __linux__
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

#define WORKERS 2
#define QUEUE_MAX 1024
#define PREBUILD_THREADS 6
#define REC_MAGIC 0x43525048u       /* "HPRC" */
/* Up with every change of the pipeline key's layout (PipeKey, vk/draw.c), as
 * a record of the old one would build other pipelines: 2, the pixel keys'
 * signed blend (sblend) */
#define REC_VERSION 2
#define REC_KEEP 8192               /* pipelines kept in the record, the newest */
enum { REC_PROG = 1, REC_PIPE = 2, REC_TOUCH = 3 };

typedef struct Job {
    uint64_t hash, vsh;
    void *key;
    uint32_t *tok;
    uint32_t nslots;
    VkPipeline pipe;
    HlePbResult r;
} Job;

typedef struct Buf {
    uint8_t *p;
    size_t n, cap;
} Buf;

/* A vertex program in the record */
typedef struct Prog {
    uint64_t vsh;
    uint32_t nslots;
    uint32_t *tok;
} Prog;

/* A recorded pipeline (the prebuild's list) */
typedef struct RecPipe {
    uint64_t hash, vsh;
    uint32_t stamp;
    void *key;
    VkPipeline pipe;                /* the prebuild's result */
    HlePbResult r;
} RecPipe;

typedef struct Touch {
    uint64_t hash;
    uint32_t stamp;
} Touch;

static struct {
    bool opt_async, opt_prebuild;   /* the app's settings */
    bool async, prebuild, nocache;  /* in effect */
    size_t key_size;
    const char *variant;            /* the record's name suffix (hle_pipe_init) */
    HlePipeBuildFn build;

    HleMutex mu;
    HleCond cv;
    HleThread th[WORKERS];
    bool started, quit;             /* quit: hle_pipe_stop */
    Job *queue[QUEUE_MAX];
    unsigned qhead, qn;
    Job **done;
    unsigned ndone, done_cap;
    uint32_t inflight, ready;       /* atomics */

    char *path;                     /* the record, NULL: none */
    Buf pending;                    /* records not written yet */
    Prog *progs;                    /* read, and recorded since */
    unsigned nprogs, progs_cap;
    RecPipe *list;                  /* read: one per pipeline, newest first */
    unsigned nlist;

    uint32_t pb_done, pb_total, pb_next;    /* atomics */
} g_pa;

/* -1 unset, else the value of HAKUX_HLE_<env> or debug.hakux.<prop> (0 / 1) */
static int debug_switch(const char *env, const char *prop)
{
    const char *e = getenv(env);
    if (e && *e) {
        return atoi(e) != 0;
    }
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX] = "";
    if (hle_prop_get(prop, v) > 0 && (v[0] == '0' || v[0] == '1')) {
        return v[0] == '1';
    }
#endif
    return -1;
}

void hle_pipe_set_options(bool async, bool prebuild)
{
    g_pa.opt_async = async;
    g_pa.opt_prebuild = prebuild;
}

void hle_pipe_init(size_t key_size, HlePipeBuildFn build, const char *variant)
{
    g_pa.key_size = key_size;
    g_pa.variant = variant;
    g_pa.build = build;
    int a = debug_switch("HAKUX_HLE_ASYNCPIPE", "debug.hakux.asyncpipe");
    int p = debug_switch("HAKUX_HLE_PREBUILD", "debug.hakux.prebuild");
    g_pa.async = a >= 0 ? a : g_pa.opt_async;
    g_pa.prebuild = p >= 0 ? p : g_pa.opt_prebuild;
    g_pa.nocache = debug_switch("HAKUX_HLE_NOCACHE", "debug.hakux.nocache") == 1;
    hle_mutex_init(&g_pa.mu);
    hle_cond_init(&g_pa.cv);
    hle_log("hakuX-hle-pipe", "pipelines: asynchronous %s%s, prebuild %s%s%s",
            g_pa.async ? "on" : "off", a >= 0 ? " (debug)" : "",
            g_pa.prebuild ? "on" : "off", p >= 0 ? " (debug)" : "",
            g_pa.nocache ? ", no disk caches (debug)" : "");
}

bool hle_pipe_async(void)
{
    return g_pa.async;
}

bool hle_pipe_nocache(void)
{
    return g_pa.nocache;
}

/* ---- workers ----------------------------------------------------------------- */

static void lower_priority(void)
{
#ifdef __linux__
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
}

/* Until hle_pipe_stop: what is still queued then is dropped (hle_pipe_reset) */
static void *worker(void *opaque)
{
    lower_priority();
    hle_mutex_lock(&g_pa.mu);
    for (;;) {
        while (!g_pa.qn && !g_pa.quit) {
            hle_cond_wait(&g_pa.cv, &g_pa.mu);
        }
        if (g_pa.quit) {
            break;
        }
        Job *j = g_pa.queue[g_pa.qhead];
        g_pa.qhead = (g_pa.qhead + 1) % QUEUE_MAX;
        g_pa.qn--;
        hle_mutex_unlock(&g_pa.mu);
        j->r = g_pa.build(j->key, j->tok, j->nslots, false, &j->pipe);
        hle_mutex_lock(&g_pa.mu);
        if (g_pa.ndone == g_pa.done_cap) {
            g_pa.done_cap = g_pa.done_cap ? 2 * g_pa.done_cap : 64;
            g_pa.done = g_renew(Job *, g_pa.done, g_pa.done_cap);
        }
        g_pa.done[g_pa.ndone++] = j;
        hle_atomic_inc(&g_pa.ready);
    }
    hle_mutex_unlock(&g_pa.mu);
    return NULL;
}

bool hle_pipe_queue(uint64_t hash, const void *key, uint64_t vsh, const uint32_t *tok,
                    uint32_t nslots)
{
    if (!g_pa.started) {
        g_pa.quit = false;
        for (unsigned i = 0; i < WORKERS; i++) {
            hle_thread_create(&g_pa.th[i], "hle.pipe", worker, NULL, HLE_THREAD_JOINABLE);
        }
        g_pa.started = true;
    }
    Job *j = g_new0(Job, 1);
    j->hash = hash;
    j->vsh = vsh;
    j->key = g_memdup2(key, g_pa.key_size);
    j->tok = tok ? g_memdup2(tok, 16 * nslots) : NULL;
    j->nslots = nslots;
    hle_mutex_lock(&g_pa.mu);
    bool ok = g_pa.qn < QUEUE_MAX;
    if (ok) {
        g_pa.queue[(g_pa.qhead + g_pa.qn++) % QUEUE_MAX] = j;
        hle_atomic_inc(&g_pa.inflight);
        hle_cond_signal(&g_pa.cv);
    }
    hle_mutex_unlock(&g_pa.mu);
    if (!ok) {
        g_free(j->key);
        g_free(j->tok);
        g_free(j);
    }
    return ok;
}

bool hle_pipe_take(uint64_t *hash, VkPipeline *p, HlePbResult *r)
{
    if (!hle_atomic_read(&g_pa.ready)) {
        return false;
    }
    hle_mutex_lock(&g_pa.mu);
    Job *j = g_pa.ndone ? g_pa.done[--g_pa.ndone] : NULL;
    if (j) {
        hle_atomic_dec(&g_pa.ready);
        hle_atomic_dec(&g_pa.inflight);
    }
    hle_mutex_unlock(&g_pa.mu);
    if (!j) {
        return false;
    }
    *hash = j->hash;
    *p = j->pipe;
    *r = j->r;
    if (j->r == HLE_PB_OK) {
        hle_pipe_record(j->hash, j->key, j->vsh, j->tok, j->nslots);
    }
    g_free(j->key);
    g_free(j->tok);
    g_free(j);
    return true;
}

uint32_t hle_pipe_inflight(void)
{
    return hle_atomic_read(&g_pa.inflight);
}

/* ---- the record ------------------------------------------------------------------- */

static uint32_t now_stamp(void)
{
    return (uint32_t)(g_get_real_time() / G_USEC_PER_SEC);
}

static void put(Buf *b, const void *p, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = MAX(b->cap * 2, b->n + n + 4096);
        b->p = g_realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void put_prog(Buf *b, uint64_t vsh, const uint32_t *tok, uint32_t nslots)
{
    uint8_t t = REC_PROG;
    put(b, &t, 1);
    put(b, &vsh, 8);
    put(b, &nslots, 4);
    put(b, tok, 16 * (size_t)nslots);
}

static void put_pipe(Buf *b, uint64_t hash, uint32_t stamp, uint64_t vsh, const void *key)
{
    uint8_t t = REC_PIPE;
    put(b, &t, 1);
    put(b, &hash, 8);
    put(b, &stamp, 4);
    put(b, &vsh, 8);
    put(b, key, g_pa.key_size);
}

static Prog *prog_find(uint64_t vsh)
{
    for (unsigned i = 0; i < g_pa.nprogs; i++) {
        if (g_pa.progs[i].vsh == vsh) {
            return &g_pa.progs[i];
        }
    }
    return NULL;
}

static void prog_add(uint64_t vsh, const uint32_t *tok, uint32_t nslots)
{
    if (g_pa.nprogs == g_pa.progs_cap) {
        g_pa.progs_cap = g_pa.progs_cap ? 2 * g_pa.progs_cap : 64;
        g_pa.progs = g_renew(Prog, g_pa.progs, g_pa.progs_cap);
    }
    g_pa.progs[g_pa.nprogs++] = (Prog){ vsh, nslots, g_memdup2(tok, 16 * (size_t)nslots) };
}

static int by_hash_then_stamp(const void *a, const void *b)
{
    const RecPipe *x = a, *y = b;
    if (x->hash != y->hash) {
        return x->hash < y->hash ? -1 : 1;
    }
    return x->stamp < y->stamp ? -1 : x->stamp > y->stamp;
}

static int newest_first(const void *a, const void *b)
{
    const RecPipe *x = a, *y = b;
    return x->stamp < y->stamp ? 1 : x->stamp > y->stamp ? -1 : 0;
}

static int touch_by_hash(const void *k, const void *e)
{
    const uint64_t *h = k;
    const RecPipe *r = e;
    return *h < r->hash ? -1 : *h > r->hash;
}

/* The file's records: programs into g_pa.progs, pipelines into g_pa.list
 * (one each, their latest use); false if it is not a record of this
 * version and key size (it is started again). */
static bool rec_read(const uint8_t *b, size_t n)
{
    uint32_t h[3];
    if (n < sizeof(h)) {
        return false;
    }
    memcpy(h, b, sizeof(h));
    if (h[0] != REC_MAGIC || h[1] != REC_VERSION || h[2] != g_pa.key_size) {
        return false;
    }
    RecPipe *pipes = NULL;
    Touch *touches = NULL;
    unsigned np = 0, npcap = 0, nt = 0, ntcap = 0;
    size_t o = sizeof(h);
    while (o < n) {
        uint8_t t = b[o++];
        if (t == REC_PROG && o + 12 <= n) {
            uint64_t vsh;
            uint32_t nslots;
            memcpy(&vsh, b + o, 8);
            memcpy(&nslots, b + o + 8, 4);
            o += 12;
            if (nslots > 4096 || o + 16 * (size_t)nslots > n) {
                break;
            }
            if (!prog_find(vsh)) {
                prog_add(vsh, (const uint32_t *)(b + o), nslots);
            }
            o += 16 * (size_t)nslots;
        } else if (t == REC_PIPE && o + 20 + g_pa.key_size <= n) {
            if (np == npcap) {
                npcap = npcap ? 2 * npcap : 256;
                pipes = g_renew(RecPipe, pipes, npcap);
            }
            RecPipe *r = &pipes[np++];
            memset(r, 0, sizeof(*r));
            memcpy(&r->hash, b + o, 8);
            memcpy(&r->stamp, b + o + 8, 4);
            memcpy(&r->vsh, b + o + 12, 8);
            r->key = g_memdup2(b + o + 20, g_pa.key_size);
            o += 20 + g_pa.key_size;
        } else if (t == REC_TOUCH && o + 12 <= n) {
            if (nt == ntcap) {
                ntcap = ntcap ? 2 * ntcap : 256;
                touches = g_renew(Touch, touches, ntcap);
            }
            memcpy(&touches[nt].hash, b + o, 8);
            memcpy(&touches[nt].stamp, b + o + 8, 4);
            nt++;
            o += 12;
        } else {
            break;              /* a torn tail (the process died mid-append) */
        }
    }
    /* one per pipeline, its latest record's stamp, then its touches' */
    qsort(pipes, np, sizeof(*pipes), by_hash_then_stamp);
    unsigned u = 0;
    for (unsigned i = 0; i < np; i++) {
        if (u && pipes[u - 1].hash == pipes[i].hash) {
            g_free(pipes[u - 1].key);
            pipes[u - 1] = pipes[i];
        } else {
            pipes[u++] = pipes[i];
        }
    }
    for (unsigned i = 0; i < nt; i++) {
        RecPipe *r = bsearch(&touches[i].hash, pipes, u, sizeof(*pipes), touch_by_hash);
        if (r && touches[i].stamp > r->stamp) {
            r->stamp = touches[i].stamp;
        }
    }
    g_free(touches);
    g_pa.list = pipes;
    g_pa.nlist = u;
    return true;
}

void hle_pipe_title(uint32_t title_id, uint32_t timedate)
{
    if (g_pa.nocache || g_pa.path) {
        return;
    }
    g_autofree char *dir = g_strdup_printf("%shle_pipelines", hle_host_base_path());
    g_mkdir_with_parents(dir, 0755);
    g_pa.path = g_strdup_printf("%s/%08x-%08x%s.bin", dir, title_id, timedate, g_pa.variant);

    g_autofree gchar *data = NULL;
    gsize n = 0;
    bool read = g_file_get_contents(g_pa.path, &data, &n, NULL) &&
                rec_read((const uint8_t *)data, n);

    /* newest first, at most REC_KEEP: the prebuild's list and the compact file */
    qsort(g_pa.list, g_pa.nlist, sizeof(*g_pa.list), newest_first);
    for (unsigned i = REC_KEEP; i < g_pa.nlist; i++) {
        g_free(g_pa.list[i].key);
    }
    g_pa.nlist = MIN(g_pa.nlist, REC_KEEP);

    Buf b = { 0 };
    uint32_t hdr[3] = { REC_MAGIC, REC_VERSION, (uint32_t)g_pa.key_size };
    put(&b, hdr, sizeof(hdr));
    for (unsigned i = 0; i < g_pa.nprogs; i++) {        /* the kept pipelines' programs */
        Prog *p = &g_pa.progs[i];
        bool used = false;
        for (unsigned k = 0; k < g_pa.nlist && !used; k++) {
            used = g_pa.list[k].vsh == p->vsh;
        }
        if (used) {
            put_prog(&b, p->vsh, p->tok, p->nslots);
        }
    }
    for (unsigned i = 0; i < g_pa.nlist; i++) {
        RecPipe *r = &g_pa.list[i];
        put_pipe(&b, r->hash, r->stamp, r->vsh, r->key);
    }
    g_autofree char *tmp = g_strdup_printf("%s.tmp", g_pa.path);
    if (g_file_set_contents(tmp, (const gchar *)b.p, (gssize)b.n, NULL)) {
        rename(tmp, g_pa.path);
    }
    hle_log("hakuX-hle-pipe", "pipeline record %s: %s, %u pipelines, %u programs (%zu bytes)",
            g_pa.path, read ? "read" : "new", g_pa.nlist, g_pa.nprogs, b.n);
    g_free(b.p);
}

void hle_pipe_record(uint64_t hash, const void *key, uint64_t vsh, const uint32_t *tok,
                     uint32_t nslots)
{
    if (!g_pa.path) {
        return;
    }
    if (vsh && tok && !prog_find(vsh)) {
        put_prog(&g_pa.pending, vsh, tok, nslots);
        prog_add(vsh, tok, nslots);
    }
    put_pipe(&g_pa.pending, hash, now_stamp(), vsh, key);
}

void hle_pipe_touch(uint64_t hash)
{
    if (!g_pa.path) {
        return;
    }
    uint8_t t = REC_TOUCH;
    uint32_t stamp = now_stamp();
    put(&g_pa.pending, &t, 1);
    put(&g_pa.pending, &hash, 8);
    put(&g_pa.pending, &stamp, 4);
}

void hle_pipe_flush(bool now)
{
    if (!g_pa.path || !g_pa.pending.n) {
        return;
    }
    static int64_t last;
    const int64_t t = g_get_monotonic_time();
    if (!now && t - last < G_USEC_PER_SEC) {
        return;                         /* a burst's records in one write */
    }
    last = t;
    FILE *f = fopen(g_pa.path, "ab");
    if (f) {
        fwrite(g_pa.pending.p, 1, g_pa.pending.n, f);
        fclose(f);
    }
    g_pa.pending.n = 0;
}

/* ---- the prebuild --------------------------------------------------------------- */

static void *prebuild_worker(void *opaque)
{
    uint32_t n = hle_atomic_read(&g_pa.pb_total);
    for (;;) {
        uint32_t i = __atomic_fetch_add(&g_pa.pb_next, 1, __ATOMIC_SEQ_CST);
        if (i >= n) {
            break;
        }
        RecPipe *r = &g_pa.list[i];
        const Prog *p = r->vsh ? prog_find(r->vsh) : NULL;
        /* from the caches when they hold it, else compiled (only those make
         * the pipeline cache worth writing again, vk/pcache.c) */
        r->r = g_pa.build(r->key, p ? p->tok : NULL, p ? p->nslots : 0, true, &r->pipe);
        if (r->r == HLE_PB_COMPILE) {
            r->r = g_pa.build(r->key, p ? p->tok : NULL, p ? p->nslots : 0, false, &r->pipe);
        }
        hle_atomic_inc(&g_pa.pb_done);
    }
    return NULL;
}

void hle_pipe_prebuild(uint32_t max, void (*add)(uint64_t hash, VkPipeline p))
{
    if (!g_pa.prebuild || !g_pa.path || !g_pa.nlist) {
        return;
    }
    uint32_t n = MIN(g_pa.nlist, max);
    int64_t t0 = g_get_monotonic_time();
    hle_atomic_set(&g_pa.pb_next, 0);
    hle_atomic_set(&g_pa.pb_done, 0);
    hle_atomic_set(&g_pa.pb_total, n);
    HleThread th[PREBUILD_THREADS];
    unsigned nth = MIN(PREBUILD_THREADS, n);
    for (unsigned i = 0; i < nth; i++) {
        hle_thread_create(&th[i], "hle.prebuild", prebuild_worker, NULL, HLE_THREAD_JOINABLE);
    }
    for (unsigned i = 0; i < nth; i++) {
        hle_thread_join(&th[i]);
    }
    unsigned ok = 0, failed = 0;
    for (uint32_t i = 0; i < n; i++) {
        RecPipe *r = &g_pa.list[i];
        if (r->r == HLE_PB_OK && r->pipe) {
            add(r->hash, r->pipe);
            ok++;
        } else {
            failed++;
        }
    }
    hle_log("hakuX-hle-pipe", "prebuild: %u of %u recorded pipelines in %.0f ms on %u threads "
            "(%u failed)", ok, g_pa.nlist, (g_get_monotonic_time() - t0) / 1000.0, nth, failed);
    hle_atomic_set(&g_pa.pb_total, 0);
    hle_atomic_set(&g_pa.pb_done, 0);
}

void hle_pipe_progress(uint32_t *done, uint32_t *total)
{
    *total = hle_atomic_read(&g_pa.pb_total);
    *done = *total ? hle_atomic_read(&g_pa.pb_done) : 0;
}

void hle_pipe_stop(void)
{
    if (!g_pa.started) {
        return;
    }
    hle_mutex_lock(&g_pa.mu);
    g_pa.quit = true;
    hle_cond_broadcast(&g_pa.cv);
    hle_mutex_unlock(&g_pa.mu);
    for (unsigned i = 0; i < WORKERS; i++) {
        hle_thread_join(&g_pa.th[i]);   /* after the build each may be in */
    }
    g_pa.started = false;
}

static void job_free(Job *j)
{
    g_free(j->key);
    g_free(j->tok);
    g_free(j);
}

void hle_pipe_reset(void)
{
    /* the records not written yet, then everything the title's run made */
    hle_pipe_flush(true);
    for (unsigned i = 0; i < g_pa.qn; i++) {
        job_free(g_pa.queue[(g_pa.qhead + i) % QUEUE_MAX]);
    }
    for (unsigned i = 0; i < g_pa.ndone; i++) {
        if (g_pa.done[i]->pipe) {
            g_hvk.t.vkDestroyPipeline(g_hvk.dev, g_pa.done[i]->pipe, NULL);   /* built, never taken */
        }
        job_free(g_pa.done[i]);
    }
    g_free(g_pa.done);
    for (unsigned i = 0; i < g_pa.nprogs; i++) {
        g_free(g_pa.progs[i].tok);
    }
    g_free(g_pa.progs);
    for (unsigned i = 0; i < g_pa.nlist; i++) {
        g_free(g_pa.list[i].key);       /* their pipelines are draw.c's (prebuild_add) */
    }
    g_free(g_pa.list);
    g_free(g_pa.pending.p);
    g_free(g_pa.path);
    hle_cond_destroy(&g_pa.cv);
    hle_mutex_destroy(&g_pa.mu);
    const bool async = g_pa.opt_async, prebuild = g_pa.opt_prebuild;
    memset(&g_pa, 0, sizeof(g_pa));
    g_pa.opt_async = async;             /* the front end's settings */
    g_pa.opt_prebuild = prebuild;
}
