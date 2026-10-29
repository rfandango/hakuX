/*
 * hakuX HLE GPU — on-disk shader and pipeline caches (P3 Task 13).
 * - SPIR-V: <base>/hle_spirv/<fnv64 of the GLSL>-<stage>.spv, a header with
 *   the source length and a second hash so a collision reads as a miss
 *   (hle_spirv-vk11/: SPIR-V 1.3, for a device below Vulkan 1.3).
 * - Pipelines: <base>/hle_pipeline_cache/<title id>.bin, the title's
 *   VkPipelineCache blob (the driver rejects a blob from another device or
 *   driver version), written by a worker thread: the blob is megabytes and
 *   the write syncs to flash (~110 ms), which on the render thread stalled a
 *   frame every 5 s. The cache is internally synchronized: the worker reads
 *   it while the render thread creates pipelines. Written when pipelines
 *   were compiled since (hle_pcache_compiled), at most once a minute, from a
 *   copy: the driver holds its cache's lock while it serializes, and a write
 *   of the live cache kept the render thread's pipeline lookups out for
 *   0.2-0.8 s at a time (NGB froze, then dropped the frames it caught up
 *   with). A pipeline compiled since the last write and lost with the
 *   process is built again by the next start's prebuild (its record,
 *   pipeasync.c, is written apart). One cache for every title (until
 *   1.0.0's) grew to 515 MB on the RP5 over 45 titles: each game held all of
 *   it in memory and wrote all of it back.
 */
#include "hw/xbox/hle/base.h"
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include "hw/xbox/hle/hle.h"

#include "hw/xbox/hle/vk/pcache.h"
#include "hw/xbox/hle/vk/pipeasync.h"
#include "hw/xbox/hle/vk/vkdev.h"

#define T (g_hvk.t)
#define SPV_MAGIC   0x56505348u     /* "HSPV" */
#define SAVE_EVERY_US (60 * G_USEC_PER_SEC)

static struct {
    VkPipelineCache cache;
    char *dir, *blob;
    uint64_t added, saved;          /* pipelines compiled / at the last write */
    int64_t last_save;
    uint64_t spv_hits, spv_misses;
    HleThread th;
    HleSem go;
    bool started, busy, stop;       /* busy: a write in flight */
} g_pc;

static uint64_t fnv64(const void *p, size_t n, uint64_t h)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 0x100000001b3ull;
    }
    return h;
}

void hle_pcache_init(void)
{
    const char *base = hle_host_base_path();
    gchar *data = NULL;
    gsize len = 0;
    if (!hle_pipe_nocache()) {      /* else neither read nor written (pipeasync.h) */
        /* SPIR-V 1.3 for a device below Vulkan 1.3 apart */
        g_pc.dir = g_strdup_printf("%shle_spirv%s", base, g_hle_glsl_vk11 ? "-vk11" : "");
        g_autofree char *pdir = g_strdup_printf("%shle_pipeline_cache", base);
        g_pc.blob = g_strdup_printf("%s/%08x.bin", pdir, g_hle.title.xbe.title_id);
        g_mkdir_with_parents(g_pc.dir, 0700);
        g_mkdir_with_parents(pdir, 0700);
        g_file_get_contents(g_pc.blob, &data, &len, NULL);
        /* every title's, before: its pipelines come back by each title's
         * prebuild, compiled once into the title's own */
        g_autofree char *shared = g_strdup_printf("%shle_pipeline_cache.bin", base);
        struct stat sb;
        if (stat(shared, &sb) == 0 && remove(shared) == 0) {
            hle_log("hakuX-hle-pipe", "the shared pipeline cache removed (%lld bytes): one per "
                    "title now", (long long)sb.st_size);
        }
    }
    VkPipelineCacheCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = data ? len : 0, .pInitialData = data };
    if (T.vkCreatePipelineCache(g_hvk.dev, &ci, NULL, &g_pc.cache) != VK_SUCCESS && data) {
        ci.initialDataSize = 0;     /* a blob the driver refuses: start empty */
        ci.pInitialData = NULL;
        T.vkCreatePipelineCache(g_hvk.dev, &ci, NULL, &g_pc.cache);
    }
    hle_log("hakuX-hle-pipe", "pipeline cache %s: %zu bytes", g_pc.blob ? g_pc.blob : "(none)",
            data ? (size_t)len : 0);
    g_free(data);
    g_pc.last_save = g_get_monotonic_time();
}

VkPipelineCache hle_pcache_get(void)
{
    return g_pc.cache;
}

void hle_pcache_compiled(void)
{
    hle_atomic_inc(&g_pc.added);       /* the render thread and the pipeline workers */
}

static void write_blob(void)
{
    if (!g_pc.blob) {
        return;
    }
    /* a copy to serialize (Mesa's merge takes references to the entries):
     * the live cache is locked for the merge alone */
    const int64_t t0 = g_get_monotonic_time();
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    VkPipelineCache copy = VK_NULL_HANDLE;
    if (T.vkCreatePipelineCache(g_hvk.dev, &ci, NULL, &copy) != VK_SUCCESS) {
        return;
    }
    const bool merged = T.vkMergePipelineCaches(g_hvk.dev, copy, 1, &g_pc.cache) == VK_SUCCESS;
    const int64_t t1 = g_get_monotonic_time();
    size_t n = 0;
    void *buf = NULL;
    if (merged && T.vkGetPipelineCacheData(g_hvk.dev, copy, &n, NULL) == VK_SUCCESS && n) {
        buf = g_malloc(n);
        if (T.vkGetPipelineCacheData(g_hvk.dev, copy, &n, buf) != VK_SUCCESS) {
            g_free(buf);
            buf = NULL;
        }
    }
    T.vkDestroyPipelineCache(g_hvk.dev, copy, NULL);
    if (buf) {
        const int64_t t2 = g_get_monotonic_time();
        g_file_set_contents(g_pc.blob, buf, n, NULL);   /* temp file + rename */
        hle_log("hakuX-hle-pipe", "pipeline cache saved: %zu bytes (merged in %.1f ms, "
                "serialized in %.1f ms, written in %.1f ms), spirv hits=%llu misses=%llu", n,
                (t1 - t0) / 1000.0, (t2 - t1) / 1000.0, (g_get_monotonic_time() - t2) / 1000.0,
                (unsigned long long)g_pc.spv_hits, (unsigned long long)g_pc.spv_misses);
    }
    g_free(buf);
}

/* A write asked for is made before a stop too: destroy's forced write counts
 * it as done (g_pc.saved). */
static void *save_thread(void *opaque)
{
    for (;;) {
        hle_sem_wait(&g_pc.go);
        if (hle_atomic_read(&g_pc.busy)) {
            write_blob();
            hle_atomic_set(&g_pc.busy, false);
        }
        if (hle_atomic_read(&g_pc.stop)) {
            return NULL;
        }
    }
}

/* Render thread (swaps): a write when pipelines were added since the last
 * one, at most every SAVE_EVERY_US, on the worker; force: now, here, after
 * the worker is done (destroy). */
void hle_pcache_save(bool force)
{
    int64_t now = g_get_monotonic_time();
    if (!g_pc.cache || g_pc.added == g_pc.saved ||
        (!force && now - g_pc.last_save < SAVE_EVERY_US)) {
        return;
    }
    if (force) {
        write_blob();
    } else {
        if (hle_atomic_read(&g_pc.busy)) {
            return;                     /* the previous write is still going */
        }
        if (!g_pc.started) {
            hle_sem_init(&g_pc.go, 0);
            hle_thread_create(&g_pc.th, "hle.pcache", save_thread, NULL,
                               HLE_THREAD_JOINABLE);
            g_pc.started = true;
        }
        hle_atomic_set(&g_pc.busy, true);
        hle_sem_post(&g_pc.go);
    }
    g_pc.saved = g_pc.added;
    g_pc.last_save = now;
}

/* The render thread and the pipeline workers joined, the device idle: the
 * worker stopped after the write it may be in, what was compiled since
 * written here, and everything as before the init (the next may be another
 * title's, on another device). */
void hle_pcache_destroy(void)
{
    if (g_pc.started) {
        hle_atomic_set(&g_pc.stop, true);
        hle_sem_post(&g_pc.go);
        hle_thread_join(&g_pc.th);
        hle_sem_destroy(&g_pc.go);
        g_pc.started = false;
    }
    hle_pcache_save(true);
    if (g_pc.cache) {
        T.vkDestroyPipelineCache(g_hvk.dev, g_pc.cache, NULL);
    }
    g_free(g_pc.dir);
    g_free(g_pc.blob);
    memset(&g_pc, 0, sizeof(g_pc));     /* stop and busy too: the next save starts a worker */
}

typedef struct SpvHeader {
    uint32_t magic, src_len;
    uint64_t check;                 /* second hash of the source */
    uint32_t nwords, pad;
} SpvHeader;

/* src's SPIR-V from the disk cache, or NULL (*path: the file, g_free'd by the caller) */
static uint32_t *spv_read(HleShaderStage stage, const char *src, size_t *nwords, char **path,
                          uint64_t *check_out)
{
    size_t len = strlen(src);
    uint64_t key = fnv64(src, len, 0xcbf29ce484222325ull) ^ (uint64_t)stage;
    uint64_t check = fnv64(src, len, 0x84222325cbf29ce4ull);
    *check_out = check;
    *path = g_pc.dir ? g_strdup_printf("%s/%016llx-%d.spv", g_pc.dir,
                                       (unsigned long long)key, (int)stage) : NULL;
    gchar *data = NULL;
    gsize n = 0;
    if (*path && g_file_get_contents(*path, &data, &n, NULL) && n >= sizeof(SpvHeader)) {
        SpvHeader h;
        memcpy(&h, data, sizeof(h));
        if (h.magic == SPV_MAGIC && h.src_len == len && h.check == check &&
            n == sizeof(h) + 4ull * h.nwords && h.nwords) {
            uint32_t *spv = g_malloc(4ull * h.nwords);
            memcpy(spv, data + sizeof(h), 4ull * h.nwords);
            g_free(data);
            *nwords = h.nwords;
            hle_atomic_inc(&g_pc.spv_hits);
            return spv;
        }
    }
    g_free(data);
    return NULL;
}

uint32_t *hle_spirv_peek(HleShaderStage stage, const char *src, size_t *nwords)
{
    char *path;
    uint64_t check;
    uint32_t *spv = spv_read(stage, src, nwords, &path, &check);
    g_free(path);
    return spv;
}

uint32_t *hle_spirv_get(HleShaderStage stage, const char *src, size_t *nwords)
{
    char *path;
    uint64_t check;
    uint32_t *spv = spv_read(stage, src, nwords, &path, &check);
    if (spv) {
        g_free(path);
        return spv;
    }
    size_t len = strlen(src);
    /* any thread: the render thread, the pipeline workers, the prebuild.
     * glslang compiles in parallel (16.x: a pool allocator per thread, its
     * built-in tables made under its own lock), as the other callers of
     * hle_glsl_compile always did */
    spv = hle_glsl_compile(stage, src, nwords);
    hle_atomic_inc(&g_pc.spv_misses);
    if (spv && path) {
        SpvHeader h = { SPV_MAGIC, (uint32_t)len, check, (uint32_t)*nwords, 0 };
        size_t total = sizeof(h) + 4 * *nwords;
        uint8_t *out = g_malloc(total);
        memcpy(out, &h, sizeof(h));
        memcpy(out + sizeof(h), spv, 4 * *nwords);
        g_file_set_contents(path, (const gchar *)out, total, NULL);
        g_free(out);
    }
    g_free(path);
    return spv;
}

