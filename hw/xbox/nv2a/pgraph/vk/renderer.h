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
#define OPT_LOAD_OPS            1
#define OPT_CLEAR_REFACTOR      1
#define OPT_COMPUTE_SWIZZLE     1
#define OPT_TEX_NONDRAW_CMD     1
#define OPT_SURF_BATCH_CREATE   1
#define OPT_SURF_BATCH_UPLOAD   1
#define OPT_TRIPLE_BUFFERING    1
#define OPT_LARGER_POOLS        1
#define OPT_ALWAYS_DEFERRED_FENCES 1
#define OPT_PRECISE_BARRIERS    1
#define OPT_SYNC_EARLY_EXIT     1
#define OPT_UNIFORM_SKIP        0
#define OPT_MULTI_DRAW          1
#define OPT_SUPER_FAST_PATH     1
#define OPT_LARGER_STAGING      1
#define OPT_VTX_ATTR_CACHE      1
#define OPT_DYNAMIC_DEPTH_STENCIL 1
#define OPT_SURF_TO_TEX_INLINE  1

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
} ShaderModuleCacheEntry;

typedef struct ShaderBinding {
    LruNode node;
    ShaderState state;
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

typedef struct TextureBinding {
    LruNode node;
    QTAILQ_ENTRY(TextureBinding) active_entry;
    bool in_active_list;
    TextureKey key;
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

typedef struct PGRAPHVkState {
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    int debug_depth;

    bool debug_utils_extension_enabled;
    bool custom_border_color_extension_enabled;
    bool memory_budget_extension_enabled;

    VkPhysicalDevice physical_device;
    VkPhysicalDeviceFeatures enabled_physical_device_features;
    VkPhysicalDeviceProperties device_props;
    VkDevice device;
    VmaAllocator allocator;
    uint32_t allocator_last_submit_index;

#define NUM_SUBMIT_FRAMES (OPT_N_BUFFERED_SUBMIT ? (OPT_TRIPLE_BUFFERING ? 3 : 2) : 1)
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

#if OPT_DYNAMIC_STATES
    struct {
        uint32_t setupraster;
        uint32_t blendcolor;
        uint32_t control_0;
        uint32_t control_1;
        uint32_t control_2;
        bool valid;
    } dyn_state;
#endif

    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
#if OPT_LARGER_POOLS
    VkDescriptorSet descriptor_sets[2048];
#else
    VkDescriptorSet descriptor_sets[1024];
#endif
    int descriptor_set_index;
    bool need_descriptor_rebind;

    StorageBuffer storage_buffers[BUFFER_COUNT];
    PrimRewriteBuf prim_rewrite_buf;

#if OPT_ALWAYS_DEFERRED_FENCES
    FrameStagingState frame_staging[NUM_SUBMIT_FRAMES];
#endif

    MemorySyncRequirement vertex_ram_buffer_syncs[NV2A_VERTEXSHADER_ATTRIBUTES];
    size_t num_vertex_ram_buffer_syncs;
    unsigned long *uploaded_bitmap;
    size_t bitmap_size;

    uint32_t last_vertex_attr_gen;
    int cached_num_active_bindings;
    int cached_num_active_attrs;
    struct {
        hwaddr base_addr;
        size_t stride;
    } cached_attr_layout[NV2A_VERTEXSHADER_ATTRIBUTES];

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
    uint32_t last_texture_state_gen;

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
                                   float color[4], const char *format, ...) __attribute__ ((format (printf, 4, 5)));
void pgraph_vk_begin_debug_marker(PGRAPHVkState *r, VkCommandBuffer cmd,
                                  float color[4], const char *format, ...) __attribute__ ((format (printf, 4, 5)));
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

// shaders.c
void pgraph_vk_init_shaders(PGRAPHState *pg);
void pgraph_vk_finalize_shaders(PGRAPHState *pg);
void pgraph_vk_update_descriptor_sets(PGRAPHState *pg);
void pgraph_vk_bind_shaders(PGRAPHState *pg);

// reports.c
void pgraph_vk_init_reports(PGRAPHState *pg);
void pgraph_vk_finalize_reports(PGRAPHState *pg);
void pgraph_vk_clear_report_value(NV2AState *d);
void pgraph_vk_get_report(NV2AState *d, uint32_t parameter);
void pgraph_vk_process_pending_reports(NV2AState *d);
void pgraph_vk_process_pending_reports_internal(NV2AState *d);

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
void pgraph_vk_begin_command_buffer(PGRAPHState *pg);
void pgraph_vk_ensure_command_buffer(PGRAPHState *pg);
void pgraph_vk_ensure_not_in_render_pass(PGRAPHState *pg);

VkCommandBuffer pgraph_vk_begin_nondraw_commands(PGRAPHState *pg);
void pgraph_vk_end_nondraw_commands(PGRAPHState *pg, VkCommandBuffer cmd);

// blit.c
void pgraph_vk_image_blit(NV2AState *d);

#endif
