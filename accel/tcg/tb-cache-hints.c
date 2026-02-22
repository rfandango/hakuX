/*
 * Persistent TCG Translation Block Cache Hints
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#ifdef XBOX

#include "tb-cache-hints.h"
#include "exec/translation-block.h"
#include "accel/tcg/tb-cpu-state.h"
#include "accel/tcg/cpu-ops.h"
#include "exec/mmap-lock.h"
#include "exec/cpu-common.h"
#include "internal-common.h"
#include "qemu/log.h"
#include "qemu/crc32c.h"
#include <string.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

/* ------------------------------------------------------------------ */
/*  File format                                                        */
/* ------------------------------------------------------------------ */

#define TB_CACHE_MAGIC   0x54424843  /* "TBCH" */
#define TB_CACHE_VERSION 3

typedef struct TBCacheFileHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t game_hash;
    uint32_t hint_count;
} TBCacheFileHeader;

/* v1 hint struct for backward-compatible loading of old cache files. */
typedef struct TBCacheHintV1 {
    uint64_t pc;
    uint64_t cs_base;
    uint32_t flags;
    uint32_t cflags;
    uint64_t phys_pc;
} TBCacheHintV1;

/* v2 hint struct (no superblock fields). */
typedef struct TBCacheHintV2 {
    uint64_t pc;
    uint64_t cs_base;
    uint32_t flags;
    uint32_t cflags;
    uint64_t phys_pc;
    uint32_t exec_count;
    uint8_t  tier;
    uint8_t  pad[3];
} TBCacheHintV2;

/* ------------------------------------------------------------------ */
/*  Runtime state                                                      */
/* ------------------------------------------------------------------ */

#define TB_CACHE_MAX_HINTS    65536
#define TB_CACHE_HASH_BUCKETS 8192
#define TB_CACHE_HASH_MASK    (TB_CACHE_HASH_BUCKETS - 1)

/*
 * Simple open-addressing hash set used for deduplication during
 * recording.  Keyed on (pc ^ flags).
 */
static TBCacheHint *recorded_hints;
static int           recorded_count;
static int           recorded_capacity;

/* Dedup hash set -- stores indices+1 into recorded_hints (0 = empty). */
static uint32_t     *dedup_table;

/* Loaded hints ready for pre-warming. */
static TBCacheHint *loaded_hints;
static int           loaded_count;

/* ------------------------------------------------------------------ */
/*  Runtime metrics                                                    */
/* ------------------------------------------------------------------ */

/* These are declared extern in tb-cache-hints.h for inline accessors. */
uint64_t tb_cache_stats_lookup_hits;
uint64_t tb_cache_stats_lookup_misses;
uint64_t tb_cache_stats_call_count;

static uint64_t stats_prev_hits;
static uint64_t stats_prev_misses;
static int      stats_prewarmed_count;

/* ------------------------------------------------------------------ */
/*  Dedup helpers                                                      */
/* ------------------------------------------------------------------ */

static uint32_t hint_hash(uint64_t pc, uint32_t flags)
{
    uint64_t h = pc ^ ((uint64_t)flags << 17) ^ (pc >> 23);
    h ^= h >> 16;
    return (uint32_t)h & TB_CACHE_HASH_MASK;
}

static bool hint_eq(const TBCacheHint *a, const TBCacheHint *b)
{
    return a->pc      == b->pc
        && a->cs_base == b->cs_base
        && a->flags   == b->flags;
}

/* Returns true if the hint was already present. */
static bool dedup_contains_or_insert(const TBCacheHint *h, int idx)
{
    if (!dedup_table) {
        return false;
    }
    uint32_t bucket = hint_hash(h->pc, h->flags);
    for (int probe = 0; probe < 16; probe++) {
        uint32_t slot = (bucket + probe) & TB_CACHE_HASH_MASK;
        uint32_t val = dedup_table[slot];
        if (val == 0) {
            dedup_table[slot] = (uint32_t)(idx + 1);
            return false;
        }
        if (hint_eq(&recorded_hints[val - 1], h)) {
            return true;  /* duplicate */
        }
    }
    /* Hash table is too full; accept the duplicate rather than resize. */
    return false;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void tb_cache_record_hint(const TranslationBlock *tb)
{
    /* Skip one-shot TBs and invalid entries. */
    if (tb_page_addr0(tb) == (tb_page_addr_t)-1) {
        return;
    }
    if (tb->cflags & CF_INVALID) {
        return;
    }

    /* Lazy initialisation. */
    if (!recorded_hints) {
        recorded_capacity = 4096;
        recorded_hints = g_new(TBCacheHint, recorded_capacity);
        dedup_table    = g_new0(uint32_t, TB_CACHE_HASH_BUCKETS);
        recorded_count = 0;
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "tb-cache",
                            "recording started (max %d hints)",
                            TB_CACHE_MAX_HINTS);
#endif
    }

    if (recorded_count >= TB_CACHE_MAX_HINTS) {
        return;  /* cap reached */
    }

    TBCacheHint h = {
        .pc            = tb->pc,
        .cs_base       = tb->cs_base,
        .flags         = tb->flags,
        .cflags        = tb->cflags & ~(CF_COUNT_MASK | CF_INVALID | CF_TIER1 | CF_SUPERBLOCK),
        .phys_pc       = tb_page_addr0(tb),
        .exec_count    = tb->exec_count,
        .tier          = tb->tier,
        .is_superblock = (tb->superblock != NULL) ? 1 : 0,
        .pc_b          = (tb->superblock != NULL) ? tb->superblock->pc_b : 0,
    };

    /*
     * On dedup collision, update exec_count/tier to max so the hottest
     * observation is preserved across re-translations.
     */
    if (dedup_contains_or_insert(&h, recorded_count)) {
        /* Find the existing hint and update its hotness data. */
        uint32_t bucket = hint_hash(h.pc, h.flags);
        for (int probe = 0; probe < 16; probe++) {
            uint32_t slot = (bucket + probe) & TB_CACHE_HASH_MASK;
            uint32_t val = dedup_table ? dedup_table[slot] : 0;
            if (val && hint_eq(&recorded_hints[val - 1], &h)) {
                TBCacheHint *existing = &recorded_hints[val - 1];
                if (h.exec_count > existing->exec_count) {
                    existing->exec_count = h.exec_count;
                }
                if (h.tier > existing->tier) {
                    existing->tier = h.tier;
                }
                break;
            }
        }
        return;
    }

    /* Grow array if needed. */
    if (recorded_count >= recorded_capacity) {
        recorded_capacity = MIN(recorded_capacity * 2, TB_CACHE_MAX_HINTS);
        recorded_hints = g_renew(TBCacheHint, recorded_hints, recorded_capacity);
    }

    recorded_hints[recorded_count++] = h;
}

void tb_cache_save(const char *path, uint32_t game_hash)
{
    if (!recorded_hints || recorded_count < 100) {
        qemu_log("tb_cache_save: too few hints (%d), skipping\n",
                 recorded_count);
        return;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        qemu_log("tb_cache_save: cannot open %s for writing\n", path);
        return;
    }

    TBCacheFileHeader hdr = {
        .magic      = TB_CACHE_MAGIC,
        .version    = TB_CACHE_VERSION,
        .game_hash  = game_hash,
        .hint_count = (uint32_t)recorded_count,
    };

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        goto fail;
    }
    if (fwrite(recorded_hints, sizeof(TBCacheHint),
               recorded_count, f) != (size_t)recorded_count) {
        goto fail;
    }

    fclose(f);
    qemu_log("tb_cache_save: wrote %d hints to %s\n", recorded_count, path);
    return;

fail:
    fclose(f);
    remove(path);
    qemu_log("tb_cache_save: write failed, removed %s\n", path);
}

int tb_cache_load(const char *path, uint32_t game_hash)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }

    TBCacheFileHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        goto fail;
    }

    if (hdr.magic != TB_CACHE_MAGIC) {
        qemu_log("tb_cache_load: bad magic in %s\n", path);
        goto fail;
    }
    if (hdr.version < 1 || hdr.version > 3) {
        qemu_log("tb_cache_load: unsupported version %u in %s\n",
                 hdr.version, path);
        goto fail;
    }
    if (hdr.game_hash != game_hash) {
        qemu_log("tb_cache_load: game hash mismatch in %s\n", path);
        goto fail;
    }
    if (hdr.hint_count == 0 || hdr.hint_count > TB_CACHE_MAX_HINTS) {
        qemu_log("tb_cache_load: bad hint_count %u in %s\n",
                 hdr.hint_count, path);
        goto fail;
    }

    loaded_hints = g_new(TBCacheHint, hdr.hint_count);

    if (hdr.version == 1) {
        /* Read v1 hints and convert to v3. */
        TBCacheHintV1 *v1 = g_new(TBCacheHintV1, hdr.hint_count);
        if (fread(v1, sizeof(TBCacheHintV1),
                  hdr.hint_count, f) != hdr.hint_count) {
            g_free(v1);
            g_free(loaded_hints);
            loaded_hints = NULL;
            goto fail;
        }
        for (uint32_t i = 0; i < hdr.hint_count; i++) {
            memset(&loaded_hints[i], 0, sizeof(TBCacheHint));
            loaded_hints[i].pc         = v1[i].pc;
            loaded_hints[i].cs_base    = v1[i].cs_base;
            loaded_hints[i].flags      = v1[i].flags;
            loaded_hints[i].cflags     = v1[i].cflags;
            loaded_hints[i].phys_pc    = v1[i].phys_pc;
        }
        g_free(v1);
        qemu_log("tb_cache_load: upgraded %u v1 hints from %s\n",
                 hdr.hint_count, path);
    } else if (hdr.version == 2) {
        /* Read v2 hints (no superblock fields) and convert to v3. */
        TBCacheHintV2 *v2 = g_new(TBCacheHintV2, hdr.hint_count);
        if (fread(v2, sizeof(TBCacheHintV2),
                  hdr.hint_count, f) != hdr.hint_count) {
            g_free(v2);
            g_free(loaded_hints);
            loaded_hints = NULL;
            goto fail;
        }
        for (uint32_t i = 0; i < hdr.hint_count; i++) {
            memset(&loaded_hints[i], 0, sizeof(TBCacheHint));
            loaded_hints[i].pc         = v2[i].pc;
            loaded_hints[i].cs_base    = v2[i].cs_base;
            loaded_hints[i].flags      = v2[i].flags;
            loaded_hints[i].cflags     = v2[i].cflags;
            loaded_hints[i].phys_pc    = v2[i].phys_pc;
            loaded_hints[i].exec_count = v2[i].exec_count;
            loaded_hints[i].tier       = v2[i].tier;
        }
        g_free(v2);
        qemu_log("tb_cache_load: upgraded %u v2 hints from %s\n",
                 hdr.hint_count, path);
    } else {
        /* v3: read directly. */
        if (fread(loaded_hints, sizeof(TBCacheHint),
                  hdr.hint_count, f) != hdr.hint_count) {
            g_free(loaded_hints);
            loaded_hints = NULL;
            goto fail;
        }
    }

    loaded_count = (int)hdr.hint_count;
    fclose(f);
    qemu_log("tb_cache_load: loaded %d hints (v%u) from %s\n",
             loaded_count, hdr.version, path);
    return loaded_count;

fail:
    fclose(f);
    return 0;
}

/* Comparator: sort Tier 1 hints before Tier 0 for pre-warming priority. */
static int hint_tier_cmp(const void *a, const void *b)
{
    const TBCacheHint *ha = a;
    const TBCacheHint *hb = b;
    /* Higher tier first, then higher exec_count first. */
    if (ha->tier != hb->tier) {
        return (int)hb->tier - (int)ha->tier;
    }
    if (ha->exec_count != hb->exec_count) {
        return (ha->exec_count > hb->exec_count) ? -1 : 1;
    }
    return 0;
}

void tb_cache_prewarm(CPUState *cpu)
{
    if (!loaded_hints || loaded_count == 0) {
        return;
    }

    /* Sort: Tier 1 (hot) hints first, then by exec_count descending. */
    qsort(loaded_hints, loaded_count, sizeof(TBCacheHint), hint_tier_cmp);

    int translated = 0;
    int tier1_count = 0;
    int skipped = 0;

    qemu_log("tb_cache_prewarm: pre-translating %d blocks...\n", loaded_count);

    for (int i = 0; i < loaded_count; i++) {
        const TBCacheHint *h = &loaded_hints[i];

        TCGTBCPUState s = {
            .pc      = (vaddr)h->pc,
            .cs_base = h->cs_base,
            .flags   = h->flags,
            .cflags  = h->cflags | (h->tier >= 1 ? CF_TIER1 : 0),
        };

        mmap_lock();
        TranslationBlock *tb = tb_gen_code(cpu, s);
        mmap_unlock();

        if (tb) {
            if (h->tier >= 1) {
                /* Strip CF_TIER1 from stored cflags so lookup works. */
                tb->cflags &= ~CF_TIER1;
                tb->tier = 1;
                tb->exec_count = h->exec_count;
                tier1_count++;
            }
            translated++;
        } else {
            skipped++;
        }
    }

    stats_prewarmed_count = translated;

    qemu_log("tb_cache_prewarm: translated %d (tier1=%d), skipped %d\n",
             translated, tier1_count, skipped);

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "tb-cache",
                        "prewarm: translated %d (tier1=%d), skipped %d",
                        translated, tier1_count, skipped);
#endif

    /*
     * Note: Superblock hints (is_superblock=1) are not re-formed during
     * prewarm because the component TBs may not yet have sufficient
     * chain_count data.  They will be re-formed naturally as the
     * hot loops re-execute and the tier1_maybe_form_superblock() trigger
     * fires.  The component TBs (A and B) are already pre-warmed above.
     */

    /* Free loaded hints -- no longer needed. */
    g_free(loaded_hints);
    loaded_hints = NULL;
    loaded_count = 0;
}

static uint32_t hash_file(const char *path, uint32_t crc)
{
    if (!path || path[0] == '\0') {
        return crc;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return crc;
    }
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        crc = crc32c(crc, buf, (unsigned int)n);
    }
    fclose(f);
    return crc;
}

uint32_t tb_cache_compute_game_hash(const char *bootrom_path,
                                    const char *flashrom_path)
{
    uint32_t crc = 0xFFFFFFFF;
    crc = hash_file(bootrom_path, crc);
    crc = hash_file(flashrom_path, crc);
    return crc;
}

/*
 * Slow path for periodic logging -- called from the inline
 * tb_cache_maybe_log_stats() wrapper in tb-cache-hints.h when the
 * call counter wraps around.
 */
void tb_cache_do_log_stats(void)
{
    tb_cache_stats_call_count = 0;

    uint64_t delta_hits   = tb_cache_stats_lookup_hits   - stats_prev_hits;
    uint64_t delta_misses = tb_cache_stats_lookup_misses - stats_prev_misses;
    uint64_t delta_total  = delta_hits + delta_misses;
    int hit_pct = delta_total > 0
                  ? (int)(delta_hits * 100 / delta_total)
                  : 0;

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "tb-cache",
                        "hits=%" PRIu64 " misses=%" PRIu64
                        " rate=%d%%  |  total: recorded=%d"
                        " prewarmed=%d lookups=%" PRIu64,
                        delta_hits, delta_misses, hit_pct,
                        recorded_count, stats_prewarmed_count,
                        tb_cache_stats_lookup_hits + tb_cache_stats_lookup_misses);
#else
    fprintf(stderr,
            "[tb-cache] hits=%" PRIu64 " misses=%" PRIu64
            " rate=%d%%  |  total: hints_recorded=%d"
            " hints_prewarmed=%d lookups=%" PRIu64 "\n",
            delta_hits, delta_misses, hit_pct,
            recorded_count, stats_prewarmed_count,
            tb_cache_stats_lookup_hits + tb_cache_stats_lookup_misses);
#endif

    stats_prev_hits   = tb_cache_stats_lookup_hits;
    stats_prev_misses = tb_cache_stats_lookup_misses;
}

static bool rewarm_in_progress;

void tb_cache_rewarm_after_flush(CPUState *cpu)
{
    if (!recorded_hints || recorded_count == 0) {
        return;
    }

    /* Guard against recursive flush -> rewarm -> flush cycles. */
    if (rewarm_in_progress) {
        return;
    }
    rewarm_in_progress = true;

    int translated = 0;
    int tier1_count = 0;
    int total = recorded_count;

    for (int i = 0; i < total; i++) {
        const TBCacheHint *h = &recorded_hints[i];

        TCGTBCPUState s = {
            .pc      = (vaddr)h->pc,
            .cs_base = h->cs_base,
            .flags   = h->flags,
            .cflags  = h->cflags | (h->tier >= 1 ? CF_TIER1 : 0),
        };

        mmap_lock();
        TranslationBlock *tb = tb_gen_code(cpu, s);
        mmap_unlock();

        if (tb) {
            if (h->tier >= 1) {
                tb->cflags &= ~CF_TIER1;
                tb->tier = 1;
                tb->exec_count = h->exec_count;
                tier1_count++;
            }
            translated++;
        }
    }

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "tb-cache",
                        "post-flush rewarm: re-translated %d/%d blocks (tier1=%d)",
                        translated, total, tier1_count);
#endif

    rewarm_in_progress = false;
}

void tb_cache_cleanup(void)
{
    g_free(recorded_hints);
    recorded_hints = NULL;
    recorded_count = 0;
    recorded_capacity = 0;

    g_free(dedup_table);
    dedup_table = NULL;

    g_free(loaded_hints);
    loaded_hints = NULL;
    loaded_count = 0;
}

#endif /* XBOX */
