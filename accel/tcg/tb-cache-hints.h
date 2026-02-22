/*
 * Persistent TCG Translation Block Cache Hints
 *
 * Records which translation blocks are generated during gameplay and
 * saves them as "hints" to disk.  On subsequent launches the hints
 * are loaded and the blocks are pre-translated during loading, which
 * eliminates the JIT stutter that otherwise occurs during the first
 * few minutes of play.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TB_CACHE_HINTS_H
#define TB_CACHE_HINTS_H

#include "qemu/osdep.h"
#include "exec/translation-block.h"
#include "exec/cpu-common.h"

#ifndef XEMU_OPT_TB_CACHE_HINTS
#define XEMU_OPT_TB_CACHE_HINTS 1
#endif

#if defined(XBOX) && XEMU_OPT_TB_CACHE_HINTS

/*
 * A single translation-block hint (v2) -- the lookup key that
 * tb_gen_code() needs plus hotness metadata for tiered recompilation.
 */
typedef struct TBCacheHint {
    uint64_t pc;           /* Guest virtual PC */
    uint64_t cs_base;      /* Code segment base */
    uint32_t flags;        /* Architecture context flags */
    uint32_t cflags;       /* Compile flags (masked) */
    uint64_t phys_pc;      /* Physical / RAM page address */
    uint32_t exec_count;   /* Approximate execution count */
    uint8_t  tier;         /* 0 = Tier 0, 1 = Tier 1, 2 = superblock */
    uint8_t  is_superblock;/* 1 if this hint describes a merged superblock */
    uint8_t  pad[2];       /* Alignment padding */
    uint64_t pc_b;         /* Component B's PC (only if is_superblock) */
} TBCacheHint;

/*
 * Record a freshly-generated TB so its key can be persisted later.
 * Safe to call from the hot path; O(1) amortised.
 */
void tb_cache_record_hint(const TranslationBlock *tb);

/*
 * Save all recorded hints to |path|.
 * |game_hash| is an opaque identifier for the current game image
 * (e.g. CRC32 of MCPX + flash); the file is rejected on load if the
 * hash does not match.
 */
void tb_cache_save(const char *path, uint32_t game_hash);

/*
 * Load hints from |path|.  Returns the number of hints loaded,
 * or 0 on any error (missing file, hash mismatch, version mismatch).
 */
int  tb_cache_load(const char *path, uint32_t game_hash);

/*
 * Pre-translate all loaded hints.  Call once after the CPU is fully
 * realised and guest memory is mapped.
 */
void tb_cache_prewarm(CPUState *cpu);

/*
 * Compute a hash of the ROM files at the given paths.
 * Used as the game_hash parameter for save/load.
 */
uint32_t tb_cache_compute_game_hash(const char *bootrom_path,
                                    const char *flashrom_path);

/*
 * Hot-path counters -- declared extern so that the inline accessors
 * below compile to a single load/store without a function call.
 */
extern uint64_t tb_cache_stats_lookup_hits;
extern uint64_t tb_cache_stats_lookup_misses;
extern uint64_t tb_cache_stats_call_count;

/*
 * Log interval in miss-path calls.  tb_cache_maybe_log_stats() is now
 * only called on TB lookup misses, which are far less frequent than
 * the combined hit+miss count.  500K misses ≈ every few seconds during
 * active play.
 */
#define TB_CACHE_LOG_INTERVAL_CALLS 500000ULL

static inline void tb_cache_notify_lookup_hit(void)
{
    tb_cache_stats_lookup_hits++;
}

static inline void tb_cache_notify_lookup_miss(void)
{
    tb_cache_stats_lookup_misses++;
}

/*
 * Slow path for periodic logging -- only called when the call counter
 * wraps around.  Defined in tb-cache-hints.c.
 */
void tb_cache_do_log_stats(void);

/*
 * Fast inline check; the slow path fires roughly every ~5 seconds.
 * Call from the miss path only (not every iteration of the inner loop).
 */
static inline void tb_cache_maybe_log_stats(void)
{
    if (++tb_cache_stats_call_count < TB_CACHE_LOG_INTERVAL_CALLS) {
        return;
    }
    tb_cache_do_log_stats();
}

/*
 * Re-translate the most important recorded hints after a TB flush.
 * Called from tb_flush__exclusive_or_serial() to recover quickly
 * instead of waiting for on-demand retranslation stutter.
 */
void tb_cache_rewarm_after_flush(CPUState *cpu);

/*
 * Free internal state.  Called during shutdown.
 */
void tb_cache_cleanup(void);

#else /* !(XBOX && XEMU_OPT_TB_CACHE_HINTS) */

static inline void tb_cache_record_hint(const TranslationBlock *tb) {}
static inline void tb_cache_save(const char *path, uint32_t game_hash) {}
static inline int  tb_cache_load(const char *path, uint32_t game_hash) { return 0; }
static inline void tb_cache_prewarm(CPUState *cpu) {}
static inline uint32_t tb_cache_compute_game_hash(const char *a, const char *b) { return 0; }
static inline void tb_cache_notify_lookup_hit(void) {}
static inline void tb_cache_notify_lookup_miss(void) {}
static inline void tb_cache_maybe_log_stats(void) {}
static inline void tb_cache_rewarm_after_flush(CPUState *cpu) {}
static inline void tb_cache_cleanup(void) {}

#endif /* XBOX && XEMU_OPT_TB_CACHE_HINTS */

#endif /* TB_CACHE_HINTS_H */
