/*
 * Geforce NV2A PGRAPH Vulkan Renderer
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

#ifndef HW_XBOX_NV2A_PGRAPH_VK_RENDERER_H
#define HW_XBOX_NV2A_PGRAPH_VK_RENDERER_H

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/queue.h"
#include "qemu/lru.h"
#include "hw/hw.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "hw/xbox/nv2a/nv2a_regs.h"
#include "hw/xbox/nv2a/pgraph/prim_rewrite.h"
#include "hw/xbox/nv2a/pgraph/surface.h"
#include "hw/xbox/nv2a/pgraph/texture.h"
#include "hw/xbox/nv2a/pgraph/glsl/shaders.h"

#include <vulkan/vulkan.h>
#include <glslang/Include/glslang_c_interface.h>
#include <volk.h>
#include <spirv_reflect.h>
#include <vk_mem_alloc.h>

#include "debug.h"
#include "constants.h"
#include "glsl.h"

#define HAVE_EXTERNAL_MEMORY 1

#define OPT_DISPLAY_DOUBLE_BUFFER 1
#define NUM_DISPLAY_IMAGES (OPT_DISPLAY_DOUBLE_BUFFER ? 2 : 1)

#define OPT_N_BUFFERED_SUBMIT   1
#define OPT_DEFERRED_FENCES     1
#define OPT_DYNAMIC_STATES      1
#define OPT_DYNAMIC_BLEND       1
#define OPT_LOAD_OPS            1
#define OPT_CLEAR_REFACTOR      1
#define OPT_COMPUTE_SWIZZLE     1
#define OPT_TEX_NONDRAW_CMD     1
#define OPT_SURF_BATCH_CREATE   1
#define OPT_SURF_BATCH_UPLOAD   1
#define OPT_TRIPLE_BUFFERING    1
#define OPT_LARGER_POOLS        1
#define NUM_GFX_DESCRIPTOR_SETS (OPT_LARGER_POOLS ? 8192 : 1024)
#define OPT_ALWAYS_DEFERRED_FENCES 1
#define OPT_PRECISE_BARRIERS    1
#define OPT_SYNC_EARLY_EXIT     1
#define OPT_UNIFORM_SKIP        1
#define OPT_MULTI_DRAW          1
#define OPT_SUPER_FAST_PATH     1
#define OPT_LARGER_STAGING      1
#define OPT_VTX_ATTR_CACHE      1
#define OPT_DYNAMIC_DEPTH_STENCIL 1
#define OPT_DESC_REBIND_SKIP    1
#define OPT_PIPELINE_EARLY_EXIT 1
#define OPT_MEDIUM_FAST_PATH    1
#define OPT_DRAW_MERGING        0
#define OPT_INDEXED_DRAW_MERGING 0
#define OPT_DRAW_MERGE_MAX      128
#define OPT_VALIDATE_GEN_COUNTERS 0
#define OPT_REORDER_SAFE_WINDOWS 1
#define REORDER_WINDOW_MAX       64
#define OPT_DYNAMIC_REG_FILTER   1
#define OPT_BINDLESS_TEXTURES    1
#define MAX_BINDLESS_TEXTURES    1024
#define OPT_ASYNC_COMPILE        1

struct OptBisectStats {
    int super_fast_hits;
    int super_fast_misses;
    int sfp_miss_clearing;
    int sfp_miss_no_pipeline;
    int sfp_miss_no_cmdbuf;
    int sfp_miss_no_rp;
    int sfp_miss_no_fb;
    int sfp_miss_fb_dirty;
    int sfp_miss_shader_changed;
    int sfp_miss_pipe_dirty;
    int sfp_miss_desc_rebind;
    int sfp_miss_uniforms;
    int sfp_miss_no_desc;
    int sfp_miss_tex_gen;
    int sfp_miss_reg_gen;
    int sfp_miss_prog_dirty;
    int sfp_miss_vtx_gen;
    int sfp_miss_tex_vram;
    int bindless_tex_fast;
    int push_tex_fast;
    int sfp_uniform_fast;
    int vtx_attr_fast;
    int pipeline_early_hits;
    int pipeline_early_misses;
    int vtx_cache_hits;
    int vtx_cache_misses;
    int desc_rebind_skips;
    int desc_rebind_full;
    int multi_draw_indirect;
    int multi_draw_loop;
    int reorder_windows_flushed;
    int reorder_draws_reordered;
    int reorder_pipeline_switches_saved;
    int reorder_safe_draws;
    int reorder_reject_blend;
    int reorder_reject_no_depth;
    int reorder_reject_no_zwrite;
    int reorder_reject_stencil;
    int reorder_reject_alpha;
    int reorder_reject_alphakill;
    int reorder_reject_rtt;
    int reorder_reject_zfunc;
    int reorder_reject_no_color_write;
    int reorder_reject_fb_dirty;
    int reorder_reject_zpass;
    int reorder_safe_zfunc_less;
    int reorder_safe_zfunc_lequal;
    int draws_skipped_pending;
    int draws_skipped_frameskip;
    int tex_pool_hits;
    int tex_pool_misses;
    int sync_range_skip;
    int sync_early_exit;
};
extern struct OptBisectStats g_opt_stats;
#if NV2A_PERF_LOG
#define OPT_STAT_INC(field) (g_opt_stats.field++)
#else
#define OPT_STAT_INC(field) do { } while (0)
#endif
#define OPT_SURF_TO_TEX_INLINE  1
/*
 * OPT_SYNC_RANGE_SKIP: skip sync_vertex_ram_buffer when the element range is
 * already covered by a previous sync within the same command buffer AND a
 * read-only scan of the DIRTY_MEMORY_NV2A bitmap confirms no new CPU writes.
 * The dirty bitmap check (has_dirty_vertex_pages) prevents skipping when the
 * CPU has written new vertex data to the same VRAM address with the same
 * layout, which would not bump vertex_attr_gen.
 */
#define OPT_SYNC_RANGE_SKIP     1

#if OPT_ALWAYS_DEFERRED_FENCES
_Static_assert(OPT_TRIPLE_BUFFERING && OPT_N_BUFFERED_SUBMIT && OPT_DEFERRED_FENCES,
               "OPT_ALWAYS_DEFERRED_FENCES requires triple buffering and deferred fences");
#endif

typedef struct QueueFamilyIndices {
    int queue_family;
} QueueFamilyIndices;

typedef struct MemorySyncRequirement {
    hwaddr addr, size;
} MemorySyncRequirement;

typedef struct RenderPassState {
    VkFormat color_format;
    VkFormat zeta_format;
    VkAttachmentLoadOp color_load_op;
    VkAttachmentLoadOp zeta_load_op;
    VkAttachmentLoadOp stencil_load_op;
} RenderPassState;

typedef struct RenderPass {
    RenderPassState state;
    VkRenderPass render_pass;
} RenderPass;

typedef struct PipelineKey {
    bool clear;
    RenderPassState render_pass_state;
    ShaderState shader_state;
    uint32_t regs[OPT_DYNAMIC_STATES ? 6 : 9];
    VkVertexInputBindingDescription binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkVertexInputAttributeDescription attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
} PipelineKey;

typedef struct PipelineBinding {
    LruNode node;
    PipelineKey key;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkRenderPass render_pass;
    unsigned int draw_time;
    bool has_dynamic_line_width;
#if OPT_ASYNC_COMPILE
    bool pending;
#endif
} PipelineBinding;

enum Buffer {
    BUFFER_STAGING_DST,
    BUFFER_STAGING_SRC,
    BUFFER_COMPUTE_DST,
    BUFFER_COMPUTE_SRC,
    BUFFER_INDEX,
    BUFFER_INDEX_STAGING,
    BUFFER_VERTEX_RAM,
    BUFFER_VERTEX_INLINE,
    BUFFER_VERTEX_INLINE_STAGING,
    BUFFER_UNIFORM,
    BUFFER_UNIFORM_STAGING,
    BUFFER_COUNT
};

typedef struct StorageBuffer {
    VkBuffer buffer;
    VkBufferUsageFlags usage;
    VmaAllocationCreateInfo alloc_info;
    VmaAllocation allocation;
    VkMemoryPropertyFlags properties;
    size_t buffer_offset;
    size_t buffer_size;
    uint8_t *mapped;
} StorageBuffer;

#if OPT_ALWAYS_DEFERRED_FENCES
typedef struct FrameStagingState {
    StorageBuffer index_staging;
    StorageBuffer vertex_inline_staging;
    StorageBuffer uniform_staging;
    StorageBuffer staging_src;
    StorageBuffer vertex_ram;
    VkDeviceSize vertex_ram_flush_min;
    VkDeviceSize vertex_ram_flush_max;
    VkDeviceSize vertex_ram_propagate_min;
    VkDeviceSize vertex_ram_propagate_max;
    bool vertex_ram_initialized;
    unsigned long *uploaded_bitmap;
} FrameStagingState;
#endif

typedef struct SurfaceBinding {
    QTAILQ_ENTRY(SurfaceBinding) entry;
    MemAccessCallback *access_cb;

    hwaddr vram_addr;

    SurfaceShape shape;
    uintptr_t dma_addr;
    uintptr_t dma_len;
    bool color;
    bool swizzle;

    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    size_t size;

    bool cleared;
    int frame_time;
    int draw_time;
    bool draw_dirty;
    bool download_pending;
    bool upload_pending;

    BasicSurfaceFormatInfo fmt;
    SurfaceFormatInfo host_fmt;

    VkImage image;
    VkImageView image_view;
    VmaAllocation allocation;

    // Used for scaling
    VkImage image_scratch;
    VkImageLayout image_scratch_current_layout;
    VmaAllocation allocation_scratch;

    bool initialized;
    int invalidation_frame;
} SurfaceBinding;

#define MAX_DEFERRED_DOWNLOADS 16

typedef struct DeferredSurfaceDownload {
    VkDeviceSize staging_offset;
    size_t download_size;
    uint8_t *dest_ptr;
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    int bytes_per_pixel;
    bool swizzle;
    bool color;
    SurfaceFormatInfo host_fmt;
    BasicSurfaceFormatInfo fmt;
    bool use_compute_to_swizzle;
} DeferredSurfaceDownload;

typedef struct DownloadCallbackData {
    DeferredSurfaceDownload downloads[MAX_DEFERRED_DOWNLOADS];
    int num_downloads;
    struct {
        hwaddr vram_addr;
        size_t dirty_size;
    } dirty_regions[MAX_DEFERRED_DOWNLOADS];
    MemoryRegion *vram;
    QemuEvent *downloads_complete;
} DownloadCallbackData;

typedef struct ShaderModuleInfo {
    int refcnt;
    char *glsl;
    GByteArray *spirv;
    VkShaderModule module;
    SpvReflectShaderModule reflect_module;
    SpvReflectDescriptorSet **descriptor_sets;
    ShaderUniformLayout uniforms;
    ShaderUniformLayout push_constants;
} ShaderModuleInfo;

typedef struct ShaderModuleCacheKey {
    VkShaderStageFlagBits kind;
    union {
        struct {
            VshState state;
            GenVshGlslOptions glsl_opts;
        } vsh;
        struct {
            GeomState state;
            GenGeomGlslOptions glsl_opts;
        } geom;
        struct {
            PshState state;
            GenPshGlslOptions glsl_opts;
        } psh;
    };
} ShaderModuleCacheKey;

typedef struct ShaderModuleCacheEntry {
    LruNode node;
    ShaderModuleCacheKey key;
    ShaderModuleInfo *module_info;
#if OPT_ASYNC_COMPILE
    bool ready;
#endif
} ShaderModuleCacheEntry;

typedef struct ShaderBinding {
    LruNode node;
    ShaderState state;
#if OPT_ASYNC_COMPILE
    bool ready;
    struct ShaderModuleCacheEntry *pending_vsh_entry;
    struct ShaderModuleCacheEntry *pending_geom_entry;
    struct ShaderModuleCacheEntry *pending_psh_entry;
#endif
    struct {
        ShaderModuleInfo *module_info;
        VshUniformLocs uniform_locs;
    } vsh;
    struct {
        ShaderModuleInfo *module_info;
    } geom;
    struct {
        ShaderModuleInfo *module_info;
        PshUniformLocs uniform_locs;
    } psh;
} ShaderBinding;

#if OPT_ASYNC_COMPILE

typedef struct PipelineCreateParams {
    VkDevice device;
    VkPipelineCache vk_pipeline_cache;

    VkPipelineShaderStageCreateInfo shader_stages[3];
    int num_shader_stages;

    VkVertexInputBindingDescription binding_descs[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkVertexInputAttributeDescription attr_descs[NV2A_VERTEXSHADER_ATTRIBUTES];
    uint32_t num_binding_descs;
    uint32_t num_attr_descs;

    VkPrimitiveTopology topology;

    VkPipelineRasterizationStateCreateInfo rasterizer;
    VkPipelineDepthStencilStateCreateInfo depth_stencil;
    bool has_zeta;

    VkPipelineColorBlendAttachmentState color_blend_attachment;
    bool has_color;
    float blend_constants[4];

    VkDynamicState dynamic_states[20];
    int num_dynamic_states;

    bool has_dynamic_line_width;

    VkPipelineLayout layout;
    VkRenderPass render_pass;
} PipelineCreateParams;

typedef enum {
    COMPILE_JOB_SHADER_MODULE,
    COMPILE_JOB_PIPELINE,
} CompileJobType;

typedef struct CompileJob {
    CompileJobType type;
    QSIMPLEQ_ENTRY(CompileJob) entry;
    union {
        struct {
            ShaderModuleCacheEntry *target;
            ShaderModuleCacheKey key;
        } shader_module;
        struct {
            PipelineBinding *target;
            PipelineCreateParams params;
        } pipeline;
    };
} CompileJob;

#endif /* OPT_ASYNC_COMPILE */

typedef struct SubmitJob SubmitJob;
typedef void (*SubmitPostFenceCallback)(struct PGRAPHVkState *r,
                                        SubmitJob *job,
                                        void *opaque);

struct SubmitJob {
    VkCommandBuffer aux_command_buffer;
    VkCommandBuffer command_buffer;
    VkSemaphore semaphore;
    VkPipelineStageFlags wait_stage;
    VkFence fence;
    int frame_index;
    bool deferred;
    SubmitPostFenceCallback post_fence_cb;
    void *post_fence_opaque;
    QSIMPLEQ_ENTRY(SubmitJob) entry;
};

typedef struct SubmitWorker {
    QemuThread thread;
    QemuMutex lock;
    QemuCond cond;
    QemuEvent complete_event;
    QSIMPLEQ_HEAD(, SubmitJob) queue;
    SubmitJob *active_job;
    bool shutdown;
    int queue_depth;
} SubmitWorker;

/*
 * RenderCommandSnapshot: captures a point-in-time copy of PGRAPHState fields
 * needed by the draw setup path (begin_pre_draw, begin_draw, create_pipeline,
 * bind_textures, update_shader_uniforms). Enables the render thread to execute
 * draws without reading PGRAPHState directly.
 */
typedef struct RenderCommandSnapshot {
    uint32_t regs[0x2000];

    uint32_t shader_state_gen;
    uint32_t pipeline_state_gen;
    uint32_t texture_state_gen;
    uint32_t vertex_attr_gen;
    uint32_t non_dynamic_reg_gen;
    uint32_t any_reg_gen;

    bool program_data_dirty;
    uint32_t program_data[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH][VSH_TOKEN_SIZE];

    VertexAttribute vertex_attributes[NV2A_VERTEXSHADER_ATTRIBUTES];
    uint16_t compressed_attrs;
    uint16_t uniform_attrs;
    uint16_t swizzle_attrs;

    SurfaceShape surface_shape;
    struct {
        int clip_x, clip_width;
        int clip_y, clip_height;
        int width, height;
    } surface_binding_dim;
    unsigned int surface_scale_factor;

    uint32_t primitive_mode;
    bool clearing;
    bool texture_matrix_enable[NV2A_MAX_TEXTURES];
    bool texture_dirty[NV2A_MAX_TEXTURES];

    hwaddr dma_a, dma_b;
    hwaddr dma_vertex_a, dma_vertex_b;
    hwaddr dma_color, dma_zeta;

    int frame_time;
    int draw_time;

    uint32_t vsh_constants[NV2A_VERTEXSHADER_CONSTANTS][4];
    bool vsh_constants_dirty[NV2A_VERTEXSHADER_CONSTANTS];
    bool vsh_constants_any_dirty;

    uint32_t ltctxa[NV2A_LTCTXA_COUNT][4];
    bool ltctxa_dirty[NV2A_LTCTXA_COUNT];
    bool ltctxa_any_dirty;
    uint32_t ltctxb[NV2A_LTCTXB_COUNT][4];
    bool ltctxb_dirty[NV2A_LTCTXB_COUNT];
    bool ltctxb_any_dirty;
    uint32_t ltc1[NV2A_LTC1_COUNT][4];
    bool ltc1_dirty[NV2A_LTC1_COUNT];
    bool ltc1_any_dirty;

    float material_alpha;
    float specular_power;
    float specular_power_back;
    float specular_params[6];
    float specular_params_back[6];
    float point_params[8];

    float light_infinite_half_vector[NV2A_MAX_LIGHTS][3];
    float light_infinite_direction[NV2A_MAX_LIGHTS][3];
    float light_local_position[NV2A_MAX_LIGHTS][3];
    float light_local_attenuation[NV2A_MAX_LIGHTS][3];

    unsigned int inline_array_length;
    unsigned int inline_elements_length;
    unsigned int inline_buffer_length;
    unsigned int draw_arrays_length;
    unsigned int draw_arrays_min_start;
    unsigned int draw_arrays_max_count;

    bool zpass_pixel_count_enable;
} RenderCommandSnapshot;

static inline uint32_t snapshot_reg_r(const RenderCommandSnapshot *snap,
                                      unsigned int r)
{
    assert(r < 0x2000 && r % 4 == 0);
    return snap->regs[r];
}

/* pgraph_vk_reg_r defined after PGRAPHVkState */

typedef enum FinishReason {
    VK_FINISH_REASON_VERTEX_BUFFER_DIRTY,
    VK_FINISH_REASON_SURFACE_CREATE,
    VK_FINISH_REASON_SURFACE_DOWN,
    VK_FINISH_REASON_NEED_BUFFER_SPACE,
    VK_FINISH_REASON_FRAMEBUFFER_DIRTY,
    VK_FINISH_REASON_PRESENTING,
    VK_FINISH_REASON_FLIP_STALL,
    VK_FINISH_REASON_FLUSH,
    VK_FINISH_REASON_STALLED,
} FinishReason;

typedef enum {
    RCMD_DRAW,
    RCMD_CLEAR_SURFACE,
    RCMD_IMAGE_BLIT,
    RCMD_FINISH,
    RCMD_SURFACE_UPDATE,
    RCMD_PROCESS_DOWNLOADS,
    RCMD_SYNC_DISPLAY,
    RCMD_FLUSH,
    RCMD_VERTEX_RAM_UPDATE,
    RCMD_SHUTDOWN,
} RenderCommandType;

typedef struct RenderCommand {
    RenderCommandType type;

    union {
        struct {
            RenderCommandSnapshot *snap;
            int32_t *draw_arrays_start;
            int32_t *draw_arrays_count;
            uint32_t *inline_elements;
            uint32_t *inline_array;
            int draw_arrays_length;
            int inline_elements_length;
            int inline_array_length;
            int inline_buffer_length;
            bool draw_arrays_prevent_connect;
        } draw;
        struct {
            uint32_t parameter;
        } clear;
        struct {
            FinishReason reason;
            QemuEvent *completion;
            VkCommandBuffer aux_command_buffer;
            VkCommandBuffer command_buffer;
            VkSemaphore semaphore;
            VkPipelineStageFlags wait_stage;
            VkFence fence;
            int frame_index;
            bool deferred;
            SubmitPostFenceCallback post_fence_cb;
            void *post_fence_opaque;
        } finish;
        struct {
            hwaddr offset;
            VkDeviceSize size;
            void *data;
        } vertex_ram;
        struct {
            bool dirty_surfaces;
            QemuEvent *completion;
        } download;
        struct {
            QemuEvent *completion;
        } sync;
        struct {
            QemuEvent *completion;
        } flush_op;
    };
    QSIMPLEQ_ENTRY(RenderCommand) entry;
} RenderCommand;

typedef struct RenderThread {
    QemuThread thread;
    QemuMutex lock;
    QemuCond cond;
    QemuEvent idle_event;
    QSIMPLEQ_HEAD(, RenderCommand) queue;
    bool shutdown;
    int queue_depth;
} RenderThread;

typedef struct TextureKey {
    TextureShape state;
    hwaddr texture_vram_offset;
    hwaddr texture_length;
    hwaddr palette_vram_offset;
    hwaddr palette_length;
    float scale;
    uint32_t filter;
    uint32_t address;
    uint32_t border_color;
    uint32_t max_anisotropy;
} TextureKey;

#define IMAGE_POOL_MAX_SIZE 32

typedef struct TextureImageConfig {
    VkFormat format;
    VkImageType image_type;
    uint32_t width, height, depth;
    uint32_t mip_levels;
    uint32_t array_layers;
    VkImageCreateFlags flags;
} TextureImageConfig;

typedef struct PooledImage {
    QTAILQ_ENTRY(PooledImage) entry;
    TextureImageConfig config;
    VkImage image;
    VmaAllocation allocation;
} PooledImage;

typedef struct TextureBinding {
    LruNode node;
    QTAILQ_ENTRY(TextureBinding) active_entry;
    bool in_active_list;
    TextureKey key;
    TextureImageConfig image_config;
    VkImage image;
    VkImageLayout current_layout;
    VkImageView image_view;
    VmaAllocation allocation;
    VkSampler sampler;
    bool possibly_dirty;
    uint64_t hash;
    unsigned int draw_time;
    uint32_t submit_time;
    unsigned int dirty_check_frame;
    bool dirty_check_result;
#if OPT_BINDLESS_TEXTURES
    uint32_t bindless_slot;
    uint32_t bindless_binding;
#endif
} TextureBinding;

typedef struct QueryReport {
    QSIMPLEQ_ENTRY(QueryReport) entry;
    bool clear;
    uint32_t parameter;
    unsigned int query_count;
} QueryReport;

typedef struct PvideoState {
    bool enabled;
    hwaddr base;
    hwaddr limit;
    hwaddr offset;

    int pitch;
    int format;

    int in_width;
    int in_height;
    int out_width;
    int out_height;

    int in_s;
    int in_t;
    int out_x;
    int out_y;

    float scale_x;
    float scale_y;

    bool color_key_enabled;
    uint32_t color_key;
} PvideoState;

typedef struct DisplayImage {
    VkImage image;
    VkImageView image_view;
    VkDeviceMemory memory;
    VkFramebuffer framebuffer;
    VkFence fence;
    bool fence_submitted;
    bool valid;
    VkCommandBuffer cmd_buffer;
#if HAVE_EXTERNAL_MEMORY
#ifdef __ANDROID__
    struct AHardwareBuffer *ahb;
    void *egl_image;
#elif defined(WIN32)
    HANDLE handle;
#else
    int fd;
#endif
    GLuint gl_texture_id;
#ifndef __ANDROID__
    GLuint gl_memory_obj;
#endif
#endif
} DisplayImage;

typedef struct PGRAPHVkDisplayState {
    ShaderModuleInfo *display_frag;

    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_sets[NUM_DISPLAY_IMAGES];

    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;

    VkRenderPass render_pass;
    VkSampler sampler;

    DisplayImage images[NUM_DISPLAY_IMAGES];
    int render_idx;
    int display_idx;

    struct {
        PvideoState state;
        int width, height;
        VkImage image;
        VkImageView image_view;
        VmaAllocation allocation;
        VkSampler sampler;
    } pvideo;

    int width, height;
    int draw_time;
    bool use_external_memory;

    VkImage blend_prev_image;
    VkImageView blend_prev_view;
    VmaAllocation blend_prev_alloc;
    bool blend_prev_valid;
    bool blend_active;
} PGRAPHVkDisplayState;

typedef enum {
    COMPUTE_TYPE_DEPTH_STENCIL = 0,
    COMPUTE_TYPE_SWIZZLE = 1,
    COMPUTE_TYPE_UNSWIZZLE = 2,
} ComputeType;

typedef struct ComputePipelineKey {
    VkFormat host_fmt;
    bool pack;
    int workgroup_size;
    ComputeType compute_type;
} ComputePipelineKey;

typedef struct ComputePipeline {
    LruNode node;
    ComputePipelineKey key;
    VkPipeline pipeline;
} ComputePipeline;

typedef struct PGRAPHVkComputeState {
    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
#if OPT_LARGER_POOLS
    VkDescriptorSet descriptor_sets[2048];
#else
    VkDescriptorSet descriptor_sets[1024];
#endif
    int descriptor_set_index;
    VkPipelineLayout pipeline_layout;
    Lru pipeline_cache;
    ComputePipeline *pipeline_cache_entries;
} PGRAPHVkComputeState;

#if OPT_DRAW_MERGING
#define DRAW_QUEUE_MAX 128
#define INDEX_QUEUE_MAX (64 * 1024)

typedef struct DrawQueueEntry {
    int32_t first_vertex;
    int32_t vertex_count;
    uint32_t index_offset;
    uint32_t index_count;
    size_t uniform_offsets[2];
} DrawQueueEntry;

typedef struct DrawQueue {
    DrawQueueEntry entries[DRAW_QUEUE_MAX];
    int count;
    bool active;
    bool indexed;
    bool has_uniform_changes;

    uint32_t shader_state_gen;
    uint32_t pipeline_state_gen;
    uint32_t texture_state_gen;
    uint32_t any_reg_gen;
    uint32_t vertex_attr_gen;
    uint32_t texture_vram_gen;
    int primitive_mode;

    uint32_t min_start;
    uint32_t max_end;

    uint32_t min_element;
    uint32_t max_element;
    uint32_t total_indices;
    uint32_t *index_buf;
} DrawQueue;
#endif

#if OPT_REORDER_SAFE_WINDOWS

typedef enum ReorderDrawMode {
    RW_DRAW_INDEXED,
    RW_DRAW_INDIRECT,
    RW_DRAW_DIRECT,
} ReorderDrawMode;

typedef struct ReorderWindowEntry {
    PipelineBinding *pipeline_binding;

    VkDescriptorSet descriptor_set;
    uint32_t dynamic_offsets[2];

    VkBuffer vertex_buffers[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkDeviceSize vertex_offsets[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_vertex_bindings;

    ReorderDrawMode draw_mode;
    VkDeviceSize index_indirect_offset;
    uint32_t draw_count;
    uint32_t vertex_count;
    int32_t first_vertex;

    uint32_t dyn_setupraster;
    uint32_t dyn_blendcolor;
    uint32_t dyn_control_0;
    uint32_t dyn_control_1;
    uint32_t dyn_control_2;
#if OPT_DYNAMIC_BLEND
    uint32_t dyn_blend;
    uint32_t dyn_color_write_control_0;
#endif
    bool has_dynamic_line_width;
    float line_width;

    VkViewport viewport;
    VkRect2D scissor;

    float push_values[NV2A_VERTEXSHADER_ATTRIBUTES * 4];
    int num_push_values;
    bool use_push_constants;
    VkPipelineLayout layout;

#if OPT_BINDLESS_TEXTURES
    uint32_t tex_indices[NV2A_MAX_TEXTURES];
#endif
    VkDescriptorImageInfo rw_push_tex_infos[NV2A_MAX_TEXTURES];
    bool rw_use_push_descriptors;

    bool pre_draw_skipped;

    int sequence_number;
    int group_order;

    bool color_write;
    bool depth_test;
    bool stencil_test;
} ReorderWindowEntry;

#define REORDER_MAX_PIPELINES 32

typedef struct ReorderWindow {
    ReorderWindowEntry entries[REORDER_WINDOW_MAX];
    int count;
    bool active;

    PipelineBinding *seen_pipelines[REORDER_MAX_PIPELINES];
    int seen_pipeline_group[REORDER_MAX_PIPELINES];
    int num_seen_pipelines;
    int next_group;
} ReorderWindow;

#endif

typedef struct PGRAPHVkState {
    NV2AState *nv2a;
    const RenderCommandSnapshot *active_snap;
    bool is_render_thread_context;

    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    int debug_depth;

    bool debug_utils_extension_enabled;
    bool custom_border_color_extension_enabled;
    bool memory_budget_extension_enabled;
    bool extended_dynamic_state_supported;
#if OPT_DYNAMIC_BLEND
    bool eds3_blend_supported;
#endif
#if OPT_BINDLESS_TEXTURES
    bool bindless_textures_supported;
    uint32_t tex_push_offset;
    int max_vertex_push_attrs;
#endif
    bool push_descriptors_supported;
    VkDescriptorSetLayout push_tex_set_layout;
    VkDescriptorSetLayout push_ubo_set_layout;
    VkDescriptorPool push_ubo_pool;
    VkDescriptorSet *push_ubo_sets;
    int push_ubo_set_count;
    int push_ubo_set_index;
    VkDescriptorImageInfo push_tex_infos[NV2A_MAX_TEXTURES];
    bool push_tex_dirty;

    VkPhysicalDevice physical_device;
    VkPhysicalDeviceFeatures enabled_physical_device_features;
    VkPhysicalDeviceProperties device_props;
    VkDevice device;
    VmaAllocator allocator;
    uint32_t allocator_last_submit_index;

#define NUM_SUBMIT_FRAMES (OPT_N_BUFFERED_SUBMIT ? (OPT_TRIPLE_BUFFERING ? 3 : 2) : 1)
    int num_active_frames;
    VkQueue queue;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffers[NUM_SUBMIT_FRAMES * 2];
    VkSemaphore frame_semaphores[NUM_SUBMIT_FRAMES];
    VkFence frame_fences[NUM_SUBMIT_FRAMES];
    bool frame_submitted[NUM_SUBMIT_FRAMES];
    int current_frame;

    VkCommandBuffer command_buffer;
    VkSemaphore command_buffer_semaphore;
    VkFence command_buffer_fence;
    unsigned int command_buffer_start_time;
    bool in_command_buffer;
#if OPT_BINDLESS_TEXTURES
    bool bindless_set_bound;
#endif
    uint32_t submit_count;

    VkCommandBuffer aux_command_buffer;
    VkFence aux_fence;
    bool in_aux_command_buffer;

    VkFramebuffer framebuffers[50];
    int framebuffer_index;
    bool framebuffer_dirty;

#if OPT_DEFERRED_FENCES && OPT_N_BUFFERED_SUBMIT
    VkFramebuffer deferred_framebuffers[NUM_SUBMIT_FRAMES][50];
    int deferred_framebuffer_count[NUM_SUBMIT_FRAMES];
#endif

    VkRenderPass render_pass;
    VkRenderPass begin_render_pass;
    GArray *render_passes; // RenderPass
    bool in_render_pass;
    bool in_draw;
    bool color_drawn_in_cb;
    bool zeta_drawn_in_cb;

    Lru pipeline_cache;
    VkPipelineCache vk_pipeline_cache;
    PipelineBinding *pipeline_cache_entries;
    PipelineBinding *pipeline_binding;
    bool pipeline_binding_changed;
    bool pipeline_state_dirty;
    bool pre_draw_skipped;
#if OPT_ASYNC_COMPILE
    bool async_draw_skip;
#endif
    bool frame_skip_active;
    bool frame_was_skipped;
    bool blend_after_skip;
    int skip_counter;
    hwaddr frame_skip_last_good_addr;

#if OPT_DYNAMIC_STATES
    struct {
        uint32_t setupraster;
        uint32_t blendcolor;
        uint32_t control_0;
        uint32_t control_1;
        uint32_t control_2;
#if OPT_DYNAMIC_BLEND
        uint32_t blend;
        uint32_t color_write_control_0;
#endif
        bool valid;
    } dyn_state;
#endif

    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet *descriptor_sets;
    int descriptor_set_count;
    int descriptor_set_index;
    bool need_descriptor_rebind;

#if OPT_BINDLESS_TEXTURES
    VkDescriptorSetLayout bindless_set_layout;
    VkDescriptorPool bindless_descriptor_pool;
    VkDescriptorSet bindless_descriptor_set;
    VkDescriptorSetLayout ubo_set_layout;
    VkDescriptorPool ubo_descriptor_pool;
    VkDescriptorSet *ubo_descriptor_sets;
    int ubo_descriptor_set_count;
    int ubo_descriptor_set_index;
    uint64_t bindless_slot_bitmap[MAX_BINDLESS_TEXTURES / 64];
#endif

    StorageBuffer storage_buffers[BUFFER_COUNT];
    PrimRewriteBuf prim_rewrite_buf;

#if OPT_DRAW_MERGING
    DrawQueue draw_queue;
#endif

#if OPT_REORDER_SAFE_WINDOWS
    ReorderWindow reorder_window;
#endif

#if OPT_ALWAYS_DEFERRED_FENCES
    FrameStagingState frame_staging[NUM_SUBMIT_FRAMES];
#endif

    MemorySyncRequirement vertex_ram_buffer_syncs[NV2A_VERTEXSHADER_ATTRIBUTES];
    size_t num_vertex_ram_buffer_syncs;
    unsigned long *uploaded_bitmap;
    size_t bitmap_size;

    uint32_t last_vertex_attr_gen;
    uint32_t pipeline_vertex_attr_gen;
    int pipeline_num_active_attr_descs;
    int pipeline_num_active_bind_descs;
    uint32_t last_shader_state_gen;
    uint32_t last_pipeline_state_gen;
    uint32_t last_any_reg_gen;
    uint32_t last_non_dynamic_reg_gen;
    uint32_t sync_range_attr_gen;
    uint32_t sync_range_min;
    uint32_t sync_range_max;
    int cached_num_active_bindings;
    int cached_num_active_attrs;
    struct {
        hwaddr base_addr;
        size_t stride;
    } cached_attr_layout[NV2A_VERTEXSHADER_ATTRIBUTES];

#if OPT_VTX_ATTR_CACHE
    VkVertexInputAttributeDescription cached_attr_descs[NV2A_VERTEXSHADER_ATTRIBUTES];
    VkVertexInputBindingDescription cached_bind_descs[NV2A_VERTEXSHADER_ATTRIBUTES];
    int cached_attr_to_desc_loc[NV2A_VERTEXSHADER_ATTRIBUTES];
    hwaddr cached_attr_offsets[NV2A_VERTEXSHADER_ATTRIBUTES];
    uint32_t cached_compressed_attrs;
    uint32_t cached_uniform_attrs;
    uint32_t cached_swizzle_attrs;
#endif

    ram_addr_t vram_ram_addr;
    VkDeviceSize vertex_ram_flush_min;
    VkDeviceSize vertex_ram_flush_max;

    VkVertexInputAttributeDescription vertex_attribute_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    int vertex_attribute_to_description_location[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_active_vertex_attribute_descriptions;

    VkVertexInputBindingDescription vertex_binding_descriptions[NV2A_VERTEXSHADER_ATTRIBUTES];
    int num_active_vertex_binding_descriptions;
    hwaddr vertex_attribute_offsets[NV2A_VERTEXSHADER_ATTRIBUTES];

    QTAILQ_HEAD(, SurfaceBinding) surfaces;
    QTAILQ_HEAD(, SurfaceBinding) invalid_surfaces;
    QTAILQ_HEAD(, SurfaceBinding) shelved_surfaces;
    GHashTable *surface_addr_map;
    uint32_t surface_list_gen;
    SurfaceBinding *color_binding, *zeta_binding;
    bool downloads_pending;
    QemuEvent downloads_complete;
    bool download_dirty_surfaces_pending;
    QemuEvent dirty_surfaces_download_complete; // common

    DeferredSurfaceDownload deferred_downloads[MAX_DEFERRED_DOWNLOADS];
    int num_deferred_downloads;
    VkDeviceSize staging_dst_offset;

    Lru texture_cache;
    TextureBinding *texture_cache_entries;
    QTAILQ_HEAD(, TextureBinding) texture_active_list;
    TextureBinding *texture_bindings[NV2A_MAX_TEXTURES];
    TextureBinding dummy_texture;
    bool texture_bindings_changed;
    QTAILQ_HEAD(, PooledImage) image_pool;
    int image_pool_count;
    uint32_t last_texture_state_gen;
    uint32_t texture_vram_gen;
    uint32_t last_texture_vram_gen;

    struct {
        uint32_t surface_list_gen;
        hwaddr vram_addr;
        hwaddr length;
        bool had_overlap;
    } tex_surf_range_cache[NV2A_MAX_TEXTURES];

    struct {
        uint64_t key_hash;
        TextureBinding *binding;
    } tex_binding_cache[NV2A_MAX_TEXTURES];

    struct {
        uint32_t regs[8];
        uint32_t shaderprog_bits;
        bool valid;
    } tex_reg_cache[NV2A_MAX_TEXTURES];
    VkFormatProperties *texture_format_properties;

    Lru shader_cache;
    ShaderBinding *shader_cache_entries;
    ShaderBinding *shader_binding;
    ShaderModuleInfo *quad_vert_module, *solid_frag_module;
    bool shader_bindings_changed;
    bool use_push_constants_for_uniform_attrs;

    ShaderState cached_shader_state;
    uint32_t cached_shader_state_gen;
    bool cached_shader_state_valid;

    Lru shader_module_cache;
    ShaderModuleCacheEntry *shader_module_cache_entries;

    // FIXME: Merge these into a structure
    uint64_t uniform_buffer_hashes[2];
    size_t uniform_buffer_offsets[2];
    bool uniforms_changed;

    /* Cached uniform state for dirty tracking */
    float cached_material_alpha;
    float cached_specular_power;
    float cached_specular_power_back;
    float cached_point_params[8];
    float cached_light_infinite_half_vector[NV2A_MAX_LIGHTS][3];
    float cached_light_infinite_direction[NV2A_MAX_LIGHTS][3];
    float cached_light_local_position[NV2A_MAX_LIGHTS][3];
    float cached_light_local_attenuation[NV2A_MAX_LIGHTS][3];
    unsigned int cached_surface_width;
    unsigned int cached_surface_height;

#define UNIFORM_CACHED_REGS_MAX 66
    uint32_t cached_uniform_reg_values[UNIFORM_CACHED_REGS_MAX];
    bool uniform_reg_cache_valid;

    VkQueryPool query_pool;
    int max_queries_in_flight;
    int num_queries_in_flight;

#define GPU_TS_MAX_RENDER_PASSES 48
#define GPU_TS_QUERIES_PER_CB (2 + GPU_TS_MAX_RENDER_PASSES * 2)
    VkQueryPool gpu_ts_pool;
    bool gpu_ts_supported;
    float gpu_ts_period_ns;
    int gpu_ts_rp_index;
    int gpu_ts_rp_counts[NUM_SUBMIT_FRAMES];
    uint64_t gpu_ts_results[GPU_TS_QUERIES_PER_CB];
    bool new_query_needed;
    bool query_in_flight;
    bool queries_reset_in_cb;
    uint32_t zpass_pixel_count_result;
    QSIMPLEQ_HEAD(, QueryReport) report_queue;
    QSIMPLEQ_HEAD(, QueryReport) report_pool;
    QueryReport *report_pool_entries;
    uint64_t *query_results;

    SurfaceFormatInfo kelvin_surface_zeta_vk_map[3];

    uint32_t clear_parameter;

    PGRAPHVkDisplayState display;
    PGRAPHVkComputeState compute;

#if OPT_ASYNC_COMPILE
    struct {
        QemuThread thread;
        QemuMutex lock;
        QemuCond cond;
        QSIMPLEQ_HEAD(, CompileJob) queue;
        bool shutdown;
        int queue_depth;
    } compile_worker;
#endif

    SubmitWorker submit_worker;
    SubmitPostFenceCallback pending_post_fence_cb;
    void *pending_post_fence_opaque;

    RenderThread render_thread;
} PGRAPHVkState;

static inline StorageBuffer *get_staging_buffer(PGRAPHVkState *r, int buffer_id)
{
#if OPT_ALWAYS_DEFERRED_FENCES
    switch (buffer_id) {
    case BUFFER_INDEX_STAGING:
        return &r->frame_staging[r->current_frame].index_staging;
    case BUFFER_VERTEX_INLINE_STAGING:
        return &r->frame_staging[r->current_frame].vertex_inline_staging;
    case BUFFER_UNIFORM_STAGING:
        return &r->frame_staging[r->current_frame].uniform_staging;
    case BUFFER_STAGING_SRC:
        return &r->frame_staging[r->current_frame].staging_src;
    case BUFFER_VERTEX_RAM:
        return &r->frame_staging[r->current_frame].vertex_ram;
    default:
        return &r->storage_buffers[buffer_id];
    }
#else
    return &r->storage_buffers[buffer_id];
#endif
}

static inline unsigned long *get_uploaded_bitmap(PGRAPHVkState *r)
{
#if OPT_ALWAYS_DEFERRED_FENCES
    return r->frame_staging[r->current_frame].uploaded_bitmap;
#else
    return r->uploaded_bitmap;
#endif
}

/*
 * Snapshot-aware register read: uses the active snapshot if the render thread
 * has one set, otherwise falls through to the live PGRAPHState register file.
 */
static inline uint32_t pgraph_vk_reg_r(PGRAPHState *pg, unsigned int reg)
{
    PGRAPHVkState *r = pg->vk_renderer_state;
    if (r->active_snap) {
        return snapshot_reg_r(r->active_snap, reg);
    }
    return pgraph_reg_r(pg, reg);
}

// renderer.c
void pgraph_vk_check_memory_budget(PGRAPHState *pg);

// debug.c
#define RGBA_RED     (float[4]){1,0,0,1}
#define RGBA_YELLOW  (float[4]){1,1,0,1}
#define RGBA_GREEN   (float[4]){0,1,0,1}
#define RGBA_BLUE    (float[4]){0,0,1,1}
#define RGBA_PINK    (float[4]){1,0,1,1}
#define RGBA_DEFAULT (float[4]){0,0,0,0}

void pgraph_vk_debug_init(void);
void pgraph_vk_insert_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd,
                                   float color[4], const char *format, ...) G_GNUC_PRINTF(4, 5);
void pgraph_vk_begin_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd,
                                  float color[4], const char *format, ...) G_GNUC_PRINTF(4, 5);
void pgraph_vk_end_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd);

// instance.c
void pgraph_vk_init_instance(PGRAPHState *pg, Error **errp);
void pgraph_vk_finalize_instance(PGRAPHState *pg);
QueueFamilyIndices pgraph_vk_find_queue_families(VkPhysicalDevice device);
uint32_t pgraph_vk_get_memory_type(PGRAPHState *pg, uint32_t type_bits,
                                   VkMemoryPropertyFlags properties);

// glsl.c
void pgraph_vk_init_glsl_compiler(void);
void pgraph_vk_finalize_glsl_compiler(void);
void pgraph_vk_set_glslang_target(uint32_t device_api_version);
GByteArray *pgraph_vk_compile_glsl_to_spv(glslang_stage_t stage,
                                          const char *glsl_source);
VkShaderModule pgraph_vk_create_shader_module_from_spv(PGRAPHVkState *r,
                                                       GByteArray *spv);
ShaderModuleInfo *pgraph_vk_create_shader_module_from_glsl(
    PGRAPHVkState *r, VkShaderStageFlagBits stage, const char *glsl);
void pgraph_vk_ref_shader_module(ShaderModuleInfo *info);
void pgraph_vk_unref_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info);
void pgraph_vk_destroy_shader_module(PGRAPHVkState *r, ShaderModuleInfo *info);

// buffer.c
bool pgraph_vk_init_buffers(NV2AState *d, Error **errp);
void pgraph_vk_finalize_buffers(NV2AState *d);
bool pgraph_vk_buffer_has_space_for(PGRAPHState *pg, int index,
                                    VkDeviceSize size,
                                    VkDeviceAddress alignment);
VkDeviceSize pgraph_vk_append_to_buffer(PGRAPHState *pg, int index, void **data,
                                        VkDeviceSize *sizes, size_t count,
                                        VkDeviceAddress alignment);
VkDeviceSize pgraph_vk_staging_alloc(PGRAPHState *pg, VkDeviceSize size);
void pgraph_vk_staging_reset(PGRAPHState *pg);

// command.c
void pgraph_vk_init_command_buffers(PGRAPHState *pg);
void pgraph_vk_finalize_command_buffers(PGRAPHState *pg);
VkCommandBuffer pgraph_vk_begin_single_time_commands(PGRAPHState *pg);
void pgraph_vk_end_single_time_commands(PGRAPHState *pg, VkCommandBuffer cmd);
VkCommandBuffer pgraph_vk_ensure_nondraw_commands(PGRAPHState *pg);

// image.c
void pgraph_vk_transition_image_layout(PGRAPHState *pg, VkCommandBuffer cmd,
                                       VkImage image, VkFormat format,
                                       VkImageLayout oldLayout,
                                       VkImageLayout newLayout);

// vertex.c
void pgraph_vk_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                      unsigned int max_element,
                                      bool inline_data,
                                      unsigned int inline_stride,
                                      unsigned int provoking_element);
void pgraph_vk_bind_vertex_attributes_inline(NV2AState *d);
void pgraph_vk_update_vertex_ram_buffer(PGRAPHState *pg, hwaddr offset, void *data,
                                    VkDeviceSize size);
VkDeviceSize pgraph_vk_update_index_buffer(PGRAPHState *pg, void *data,
                                           VkDeviceSize size);
VkDeviceSize pgraph_vk_update_vertex_inline_buffer(PGRAPHState *pg, void **data,
                                                   VkDeviceSize *sizes,
                                                   size_t count);

// surface.c
void pgraph_vk_init_surfaces(PGRAPHState *pg);
void pgraph_vk_finalize_surfaces(PGRAPHState *pg);
void pgraph_vk_surface_flush(NV2AState *d);
void pgraph_vk_process_pending_downloads(NV2AState *d);
void pgraph_vk_surface_download_if_dirty(NV2AState *d, SurfaceBinding *surface);
SurfaceBinding *pgraph_vk_surface_get_within(NV2AState *d, hwaddr addr);
void pgraph_vk_wait_for_surface_download(SurfaceBinding *e);
void pgraph_vk_download_dirty_surfaces(NV2AState *d);
bool pgraph_vk_download_surfaces_in_range_if_dirty(PGRAPHState *pg, hwaddr start, hwaddr size);
void pgraph_vk_upload_surface_data(NV2AState *d, SurfaceBinding *surface,
                                   bool force);
void pgraph_vk_surface_update(NV2AState *d, bool upload, bool color_write,
                              bool zeta_write);
SurfaceBinding *pgraph_vk_surface_get(NV2AState *d, hwaddr addr);
void pgraph_vk_set_surface_dirty(PGRAPHState *pg, bool color, bool zeta);
void pgraph_vk_set_surface_scale_factor(NV2AState *d, unsigned int scale);
unsigned int pgraph_vk_get_surface_scale_factor(NV2AState *d);
void pgraph_vk_reload_surface_scale_factor(PGRAPHState *pg);

// surface-compute.c
void pgraph_vk_init_compute(PGRAPHState *pg);
bool pgraph_vk_compute_needs_finish(PGRAPHVkState *r);
void pgraph_vk_compute_finish_complete(PGRAPHVkState *r);
void pgraph_vk_finalize_compute(PGRAPHState *pg);
void pgraph_vk_pack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                  VkCommandBuffer cmd, VkBuffer src,
                                  VkBuffer dst, bool downscale);
void pgraph_vk_unpack_depth_stencil(PGRAPHState *pg, SurfaceBinding *surface,
                                    VkCommandBuffer cmd, VkBuffer src,
                                    VkBuffer dst);
void pgraph_vk_compute_swizzle(PGRAPHState *pg, VkCommandBuffer cmd,
                                VkBuffer src, size_t src_size,
                                VkBuffer dst, size_t dst_size,
                                unsigned int width, unsigned int height,
                                bool unswizzle);

// display.c
void pgraph_vk_init_display(PGRAPHState *pg);
void pgraph_vk_finalize_display(PGRAPHState *pg);
void pgraph_vk_render_display(PGRAPHState *pg);
bool pgraph_vk_gl_external_memory_available(void);
void pgraph_vk_gl_make_context_current(void);

// texture.c
void pgraph_vk_init_textures(PGRAPHState *pg);
void pgraph_vk_finalize_textures(PGRAPHState *pg);
void pgraph_vk_bind_textures(NV2AState *d);
bool pgraph_vk_check_textures_fast_skip(PGRAPHState *pg);
void pgraph_vk_mark_textures_possibly_dirty(NV2AState *d, hwaddr addr,
                                            hwaddr size);
void pgraph_vk_trim_texture_cache(PGRAPHState *pg);

// compile_worker.c
#if OPT_ASYNC_COMPILE
void pgraph_vk_compile_worker_init(PGRAPHVkState *r);
void pgraph_vk_compile_worker_shutdown(PGRAPHVkState *r);
void pgraph_vk_compile_worker_enqueue(PGRAPHVkState *r, CompileJob *job);
#endif

// submit_worker.c
void pgraph_vk_submit_worker_init(PGRAPHVkState *r);
void pgraph_vk_submit_worker_shutdown(PGRAPHVkState *r);
void pgraph_vk_submit_worker_enqueue(PGRAPHVkState *r, SubmitJob *job);
void pgraph_vk_submit_worker_wait_idle(PGRAPHVkState *r);

// render_thread.c
void pgraph_vk_render_thread_init(PGRAPHVkState *r);
void pgraph_vk_render_thread_shutdown(PGRAPHVkState *r);
void pgraph_vk_render_thread_enqueue(PGRAPHVkState *r, RenderCommand *cmd);
void pgraph_vk_render_thread_wait_idle(PGRAPHVkState *r);
void pgraph_vk_snapshot_state(PGRAPHState *pg, RenderCommandSnapshot *snap);

// shaders.c
void pgraph_vk_init_shaders(PGRAPHState *pg);
void pgraph_vk_finalize_shaders(PGRAPHState *pg);
void pgraph_vk_update_descriptor_sets(PGRAPHState *pg);
void pgraph_vk_bind_shaders(PGRAPHState *pg);
void pgraph_vk_update_shader_uniforms(PGRAPHState *pg);

// reports.c
void pgraph_vk_init_reports(PGRAPHState *pg);
void pgraph_vk_finalize_reports(PGRAPHState *pg);
void pgraph_vk_clear_report_value(NV2AState *d);
void pgraph_vk_get_report(NV2AState *d, uint32_t parameter);
void pgraph_vk_process_pending_reports(NV2AState *d);
void pgraph_vk_process_pending_reports_internal(NV2AState *d);

// draw.c
void pgraph_vk_init_pipelines(PGRAPHState *pg);
void pgraph_vk_finalize_pipelines(PGRAPHState *pg);
void pgraph_vk_clear_surface(NV2AState *d, uint32_t parameter);
void pgraph_vk_draw_begin(NV2AState *d);
void pgraph_vk_draw_end(NV2AState *d);
void pgraph_vk_finish(PGRAPHState *pg, FinishReason why);
#if OPT_ALWAYS_DEFERRED_FENCES
void pgraph_vk_flush_all_frames(PGRAPHState *pg);
#endif
void pgraph_vk_flush_draw(NV2AState *d);
void pgraph_vk_flush_draw_queue(NV2AState *d);
void pgraph_vk_flush_reorder_window(NV2AState *d);
void pgraph_vk_begin_command_buffer(PGRAPHState *pg);
void pgraph_vk_ensure_command_buffer(PGRAPHState *pg);
void pgraph_vk_ensure_not_in_render_pass(PGRAPHState *pg);

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg);
void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd);

// blit.c
void pgraph_vk_image_blit(NV2AState *d);

#endif
