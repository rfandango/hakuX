/*
 * Geforce NV2A PGRAPH Vulkan Renderer - Async Compile Worker
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
#include "qemu/fast-hash.h"
#include "qemu/mstring.h"

#if OPT_ASYNC_COMPILE

extern bool xemu_get_async_compile(void);
extern void shader_module_key_persist(const ShaderModuleCacheKey *key);

static void process_shader_module_job(PGRAPHVkState *r, CompileJob *job)
{
    ShaderModuleCacheEntry *target = job->shader_module.target;
    ShaderModuleCacheKey *key = &job->shader_module.key;
    MString *code;

    switch (key->kind) {
    case VK_SHADER_STAGE_VERTEX_BIT:
        code = pgraph_glsl_gen_vsh(&key->vsh.state, key->vsh.glsl_opts);
        break;
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        code = pgraph_glsl_gen_geom(&key->geom.state, key->geom.glsl_opts);
        break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        code = pgraph_glsl_gen_psh(&key->psh.state, key->psh.glsl_opts);
        break;
    default:
        assert(!"Invalid shader module kind");
        code = NULL;
    }

    ShaderModuleInfo *info = pgraph_vk_create_shader_module_from_glsl(
        r, key->kind, mstring_get_str(code));
    mstring_unref(code);

    if (info) {
        pgraph_vk_ref_shader_module(info);
        shader_module_key_persist(key);
    }

    qatomic_set(&target->module_info, info);
    qatomic_set(&target->ready, true);
}

static void process_pipeline_job(PGRAPHVkState *r, CompileJob *job)
{
    PipelineBinding *target = job->pipeline.target;
    PipelineCreateParams *p = &job->pipeline.params;

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = p->num_binding_descs,
        .pVertexBindingDescriptions = p->binding_descs,
        .vertexAttributeDescriptionCount = p->num_attr_descs,
        .pVertexAttributeDescriptions = p->attr_descs,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = p->topology,
        .primitiveRestartEnable = VK_FALSE,
    };

    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = p->has_color ? 1 : 0,
        .pAttachments = p->has_color ? &p->color_blend_attachment : NULL,
        .blendConstants[0] = p->blend_constants[0],
        .blendConstants[1] = p->blend_constants[1],
        .blendConstants[2] = p->blend_constants[2],
        .blendConstants[3] = p->blend_constants[3],
    };

    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = p->num_dynamic_states,
        .pDynamicStates = p->dynamic_states,
    };

    VkGraphicsPipelineCreateInfo pipeline_create_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = p->num_shader_stages,
        .pStages = p->shader_stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &p->rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = p->has_zeta ? &p->depth_stencil : NULL,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = p->layout,
        .renderPass = p->render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };

    VkPipeline pipeline;
    VkResult result = vkCreateGraphicsPipelines(
        p->device, p->vk_pipeline_cache, 1, &pipeline_create_info, NULL,
        &pipeline);

    if (result == VK_SUCCESS) {
        target->pipeline = pipeline;
        target->layout = p->layout;
        target->render_pass = p->render_pass;
        target->has_dynamic_line_width = p->has_dynamic_line_width;
    }
    qatomic_set(&target->pending, false);
}

static void *compile_worker_func(void *opaque)
{
    PGRAPHVkState *r = opaque;

    while (true) {
        qemu_mutex_lock(&r->compile_worker.lock);

        while (QSIMPLEQ_EMPTY(&r->compile_worker.queue) &&
               !r->compile_worker.shutdown) {
            qemu_cond_wait(&r->compile_worker.cond, &r->compile_worker.lock);
        }

        if (r->compile_worker.shutdown &&
            QSIMPLEQ_EMPTY(&r->compile_worker.queue)) {
            qemu_mutex_unlock(&r->compile_worker.lock);
            break;
        }

        CompileJob *job = QSIMPLEQ_FIRST(&r->compile_worker.queue);
        QSIMPLEQ_REMOVE_HEAD(&r->compile_worker.queue, entry);
        r->compile_worker.queue_depth--;

        qemu_mutex_unlock(&r->compile_worker.lock);

        switch (job->type) {
        case COMPILE_JOB_SHADER_MODULE:
            process_shader_module_job(r, job);
            break;
        case COMPILE_JOB_PIPELINE:
            process_pipeline_job(r, job);
            break;
        }

        g_free(job);
    }

    return NULL;
}

void pgraph_vk_compile_worker_enqueue(PGRAPHVkState *r, CompileJob *job)
{
    qemu_mutex_lock(&r->compile_worker.lock);
    QSIMPLEQ_INSERT_TAIL(&r->compile_worker.queue, job, entry);
    r->compile_worker.queue_depth++;
    qemu_cond_signal(&r->compile_worker.cond);
    qemu_mutex_unlock(&r->compile_worker.lock);
}

void pgraph_vk_compile_worker_init(PGRAPHVkState *r)
{
    qemu_mutex_init(&r->compile_worker.lock);
    qemu_cond_init(&r->compile_worker.cond);
    QSIMPLEQ_INIT(&r->compile_worker.queue);
    r->compile_worker.shutdown = false;
    r->compile_worker.queue_depth = 0;
    qemu_thread_create(&r->compile_worker.thread, "pgraph.vk.compile",
                       compile_worker_func, r, QEMU_THREAD_JOINABLE);
}

void pgraph_vk_compile_worker_shutdown(PGRAPHVkState *r)
{
    qemu_mutex_lock(&r->compile_worker.lock);
    r->compile_worker.shutdown = true;
    qemu_cond_signal(&r->compile_worker.cond);
    qemu_mutex_unlock(&r->compile_worker.lock);

    qemu_thread_join(&r->compile_worker.thread);

    CompileJob *job;
    while ((job = QSIMPLEQ_FIRST(&r->compile_worker.queue)) != NULL) {
        QSIMPLEQ_REMOVE_HEAD(&r->compile_worker.queue, entry);
        g_free(job);
    }

    qemu_mutex_destroy(&r->compile_worker.lock);
    qemu_cond_destroy(&r->compile_worker.cond);
}

#endif /* OPT_ASYNC_COMPILE */
