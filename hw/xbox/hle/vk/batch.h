/*
 * hakuX HLE GPU — strip batching (see batch.c).
 */
#ifndef HW_XBOX_HLE_VK_BATCH_H
#define HW_XBOX_HLE_VK_BATCH_H

#include "hw/xbox/hle/vk/vkdev.h"

#include <stdbool.h>
#include <stdint.h>

/* Routes the table's recording commands through the batch (vkdev.c, once
 * the device's entry points are loaded). */
void hle_batch_install(struct VolkDeviceTable *t);

/* The index buffer, once the device is up; off (HAKUX_HLE_BATCH=0 or
 * debug.hakux.batch=0) records every draw as it comes. */
void hle_batch_init(void);
void hle_batch_shutdown(void);

/* A command buffer starts for frame slot `slot`, whose previous recording
 * has completed: its share of the index buffer is free again. */
void hle_batch_slot(unsigned slot);

/* A non-indexed strip or fan draw: true when the batch holds it (recorded
 * later, with the strips that follow it), false to record it now. */
bool hle_batch_draw(VkCommandBuffer cb, VkPrimitiveTopology prim, uint32_t first, uint32_t count);

/* The run held on cb, recorded now: a run binds its own index buffer, so
 * the draw path records it before relying on the one it bound last. */
void hle_batch_flush(VkCommandBuffer cb);

/* Draws recorded and the draws they replaced, since start. */
void hle_batch_stats(uint64_t *batches, uint64_t *merged);

#endif
