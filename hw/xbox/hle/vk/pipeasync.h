/*
 * hakuX HLE GPU — pipelines built off the render thread, and before the game
 * runs (pipeasync.c):
 * - Asynchronous builds (the app's "Asynchronous shader compilation"): a
 *   pipeline that would compile is built by worker threads while draws that
 *   need it are skipped (vk/draw.c decides which).
 * - The pipelines a title made, recorded per title and XBE
 *   (<base>/hle_pipelines/<title>-<timedate>.bin: their keys and vertex
 *   programs) and built again before the game runs ("Build cached shaders
 *   before starting").
 * The keys are draw.c's, opaque here: a size given at init and a build
 * function that may run on any thread.
 */
#ifndef HW_XBOX_HLE_VK_PIPEASYNC_H
#define HW_XBOX_HLE_VK_PIPEASYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <volk.h>

typedef enum { HLE_PB_OK, HLE_PB_FAILED, HLE_PB_COMPILE } HlePbResult;

/* Builds key's pipeline (tok, nslots: its vertex program's tokens, NULL for
 * none). cache_only: from the SPIR-V on disk and the driver's pipeline cache
 * only, HLE_PB_COMPILE when anything would compile. Any thread. */
typedef HlePbResult (*HlePipeBuildFn)(const void *key, const uint32_t *tok, uint32_t nslots,
                                      bool cache_only, VkPipeline *out);

/* The app's settings, before the back end starts. */
void hle_pipe_set_options(bool async, bool prebuild);
/* draw.c's init: the key size and the build function; the debug overrides
 * (debug.hakux.asyncpipe, debug.hakux.prebuild, debug.hakux.nocache) read.
 * variant: a suffix of the record's name for keys of another kind (the
 * vertex layout in them: "-vi"), so either kind keeps its own; "" none. */
void hle_pipe_init(size_t key_size, HlePipeBuildFn build, const char *variant);
/* The asynchronous branch is on. */
bool hle_pipe_async(void);
/* No disk caches this run: SPIR-V, the pipeline cache and the record are
 * neither read nor written (a first boot without deleting them). */
bool hle_pipe_nocache(void);

/* Render thread: key's build queued on the workers (vsh: its vertex
 * program's hash, for the record); false if the queue is full. */
bool hle_pipe_queue(uint64_t hash, const void *key, uint64_t vsh, const uint32_t *tok,
                    uint32_t nslots);
/* Render thread: a build finished since the last call (recorded when made);
 * false when none. */
bool hle_pipe_take(uint64_t *hash, VkPipeline *p, HlePbResult *r);
/* Builds queued or running. */
uint32_t hle_pipe_inflight(void);
/* The renderer stopping (M2): the workers stopped and joined (after the
 * build each may be in); then, the device idle, the jobs and the record's
 * state freed (pending records written, pipelines never taken destroyed),
 * the front end's settings kept. */
void hle_pipe_stop(void);
void hle_pipe_reset(void);

/* The title (back end start): its record read, kept compact, appended to. */
void hle_pipe_title(uint32_t title_id, uint32_t timedate);
/* Render thread: a pipeline made this session, recorded (vsh: its vertex
 * program's hash, 0 for none, its tokens with it). */
void hle_pipe_record(uint64_t hash, const void *key, uint64_t vsh, const uint32_t *tok,
                     uint32_t nslots);
/* Render thread: a prebuilt pipeline's first use this session. */
void hle_pipe_touch(uint64_t hash);
/* Render thread, at each swap: the new records appended to the file, at most
 * once a second (now: at once, a run about to exit). */
void hle_pipe_flush(bool now);

/* Before the game runs (the back end's start, on the boot thread): the
 * recorded pipelines built, the most recently used first and at most max,
 * each made handed to add on this thread. Nothing when the setting is off. */
void hle_pipe_prebuild(uint32_t max, void (*add)(uint64_t hash, VkPipeline p));
/* The prebuild's progress, any thread: pipelines built and to build (0 and 0
 * when none runs). */
void hle_pipe_progress(uint32_t *done, uint32_t *total);

#endif
