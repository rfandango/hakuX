/*
 * Geforce NV2A PGRAPH Vulkan Renderer - Async Submit Worker
 *
 * Offloads vkQueueSubmit and vkWaitForFences from the PFIFO thread
 * to a dedicated worker thread.
 *
 * Copyright (c) 2024-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "renderer.h"

static void *submit_worker_func(void *opaque)
{
    PGRAPHVkState *r = opaque;

    while (true) {
        qemu_mutex_lock(&r->submit_worker.lock);

        while (QSIMPLEQ_EMPTY(&r->submit_worker.queue) &&
               !r->submit_worker.shutdown) {
            qemu_cond_wait(&r->submit_worker.cond, &r->submit_worker.lock);
        }

        if (r->submit_worker.shutdown &&
            QSIMPLEQ_EMPTY(&r->submit_worker.queue)) {
            qemu_mutex_unlock(&r->submit_worker.lock);
            break;
        }

        SubmitJob *job = QSIMPLEQ_FIRST(&r->submit_worker.queue);
        QSIMPLEQ_REMOVE_HEAD(&r->submit_worker.queue, entry);
        r->submit_worker.queue_depth--;
        r->submit_worker.active_job = job;

        qemu_mutex_unlock(&r->submit_worker.lock);

        VkSubmitInfo submit_infos[] = {
            {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &job->aux_command_buffer,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &job->semaphore,
            },
            {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .commandBufferCount = 1,
                .pCommandBuffers = &job->command_buffer,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &job->semaphore,
                .pWaitDstStageMask = &job->wait_stage,
            }
        };

        vkResetFences(r->device, 1, &job->fence);
        VK_CHECK(vkQueueSubmit(r->queue, ARRAY_SIZE(submit_infos),
                               submit_infos, job->fence));
        qatomic_set(&r->frame_submitted[job->frame_index], true);
        qatomic_inc(&r->submit_count);

        if (!job->deferred) {
            VK_CHECK(vkWaitForFences(r->device, 1, &job->fence,
                                     VK_TRUE, UINT64_MAX));
            if (job->post_fence_cb) {
                job->post_fence_cb(r, job, job->post_fence_opaque);
            }
        } else if (job->post_fence_cb) {
            VK_CHECK(vkWaitForFences(r->device, 1, &job->fence,
                                     VK_TRUE, UINT64_MAX));
            job->post_fence_cb(r, job, job->post_fence_opaque);
        }

        qemu_mutex_lock(&r->submit_worker.lock);
        r->submit_worker.active_job = NULL;
        qemu_mutex_unlock(&r->submit_worker.lock);

        qemu_event_set(&r->submit_worker.complete_event);

        g_free(job);
    }

    return NULL;
}

void pgraph_vk_submit_worker_enqueue(PGRAPHVkState *r, SubmitJob *job)
{
    qemu_event_reset(&r->submit_worker.complete_event);

    qemu_mutex_lock(&r->submit_worker.lock);
    QSIMPLEQ_INSERT_TAIL(&r->submit_worker.queue, job, entry);
    r->submit_worker.queue_depth++;
    qemu_cond_signal(&r->submit_worker.cond);
    qemu_mutex_unlock(&r->submit_worker.lock);
}

void pgraph_vk_submit_worker_wait_idle(PGRAPHVkState *r)
{
    while (true) {
        qemu_mutex_lock(&r->submit_worker.lock);
        bool idle = QSIMPLEQ_EMPTY(&r->submit_worker.queue) &&
                    r->submit_worker.active_job == NULL;
        qemu_mutex_unlock(&r->submit_worker.lock);

        if (idle) {
            return;
        }

        qemu_event_wait(&r->submit_worker.complete_event);
        qemu_event_reset(&r->submit_worker.complete_event);
    }
}

void pgraph_vk_submit_worker_init(PGRAPHVkState *r)
{
    qemu_mutex_init(&r->submit_worker.lock);
    qemu_cond_init(&r->submit_worker.cond);
    qemu_event_init(&r->submit_worker.complete_event, false);
    QSIMPLEQ_INIT(&r->submit_worker.queue);
    r->submit_worker.shutdown = false;
    r->submit_worker.queue_depth = 0;
    r->submit_worker.active_job = NULL;
    qemu_thread_create(&r->submit_worker.thread, "pgraph.vk.submit",
                       submit_worker_func, r, QEMU_THREAD_JOINABLE);
}

void pgraph_vk_submit_worker_shutdown(PGRAPHVkState *r)
{
    pgraph_vk_submit_worker_wait_idle(r);

    qemu_mutex_lock(&r->submit_worker.lock);
    r->submit_worker.shutdown = true;
    qemu_cond_signal(&r->submit_worker.cond);
    qemu_mutex_unlock(&r->submit_worker.lock);

    qemu_thread_join(&r->submit_worker.thread);

    SubmitJob *job;
    while ((job = QSIMPLEQ_FIRST(&r->submit_worker.queue)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&r->submit_worker.queue, entry);
        g_free(job);
    }

    qemu_mutex_destroy(&r->submit_worker.lock);
    qemu_cond_destroy(&r->submit_worker.cond);
    qemu_event_destroy(&r->submit_worker.complete_event);
}
