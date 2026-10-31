/*
 * hakuX HLE GPU — back-end draws (P3, first slice): bindless descriptors,
 * texture images uploaded from the arena, samplers from texture-stage
 * state, fixed-function pipelines (shader/ff_gen.c), vertex programs
 * (shader/vsh.c; constants in the arena, set 1 dynamic uniform buffer) and
 * draw recording. Render thread only.
 */
#include "hw/xbox/hle/base.h"
#include <math.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/prop.h"
#include "hw/xbox/hle/hitch.h"
#include "hw/xbox/hle/tex/xdk.h"
#include "hw/xbox/hle/tex/xdk_res.h"
#include "hw/xbox/hle/mem/guestmem.h"
#include "hw/xbox/hle/shader/ff.h"
#include "hw/xbox/hle/shader/ffhw.h"
#include "hw/xbox/hle/shader/glsl_compile.h"
#include "hw/xbox/hle/shader/psh.h"
#include "hw/xbox/hle/shader/vattr.h"
#include "hw/xbox/hle/shader/vsh.h"
#include "hw/xbox/hle/vk/back_int.h"
#include "hw/xbox/hle/vk/batch.h"
#include "hw/xbox/hle/vk/quirks.h"
#include "hw/xbox/hle/vk/guest_ram.h"
#include "hw/xbox/hle/vk/pcache.h"
#include "hw/xbox/hle/vk/pipeasync.h"

#define T (g_hvk.t)
#define HLE_MAX_TEX      2048
#define HLE_MAX_PIPES    8192   /* kept for the session: Halo 2 makes ~600 in one level; the
                                   prebuild fills at most half (hle_draw_prebuild) */
#define HLE_MAX_PROGS    1024
#define HLE_CONST_BYTES  ((HLE_VSH_CONSTANTS + HLE_VSH_CUR_REGS) * 16)  /* c[], vcur[] */

/* 5659 render-state indices (D3DRENDERSTATETYPE) */
enum {
    RS_ZFUNC = 57, RS_ALPHAFUNC = 58, RS_ALPHABLENDENABLE = 59, RS_ALPHATESTENABLE = 60,
    RS_ALPHAREF = 61, RS_SRCBLEND = 62, RS_DESTBLEND = 63, RS_ZWRITEENABLE = 64,
    RS_SHADEMODE = 66, RS_COLORWRITEENABLE = 67, RS_STENCILZFAIL = 68,
    RS_STENCILPASS = 69, RS_STENCILFUNC = 70, RS_STENCILREF = 71, RS_STENCILMASK = 72,
    RS_STENCILWRITEMASK = 73, RS_BLENDOP = 74, RS_BLENDCOLOR = 75,
    RS_FOGENABLE = 92, RS_FOGTABLEMODE = 93, RS_FOGSTART = 94, RS_FOGEND = 95,
    RS_FOGDENSITY = 96, RS_RANGEFOGENABLE = 97, RS_LIGHTING = 102, RS_SPECULARENABLE = 103,
    RS_LOCALVIEWER = 104, RS_COLORVERTEX = 105, RS_SPECULARMATERIALSOURCE = 110,
    RS_DIFFUSEMATERIALSOURCE = 111, RS_AMBIENTMATERIALSOURCE = 112,
    RS_EMISSIVEMATERIALSOURCE = 113, RS_POINTSPRITEENABLE = 118, RS_POINTSCALEENABLE = 119,
    RS_VERTEXBLEND = 137,
    RS_NORMALIZENORMALS = 142, RS_FOGCOLOR = 138, RS_ZENABLE = 143, RS_STENCILENABLE = 144,
    RS_STENCILFAIL = 145, RS_FRONTFACE = 146, RS_CULLMODE = 147, RS_TEXTUREFACTOR = 148,
    RS_SHADOWFUNC = 156,
    /* polygon offset (ZBias writes these: slope -0.25 * bias, offset -bias) */
    RS_ZSLOPESCALE = 77, RS_ZOFFSET = 78, RS_POINTOFFSETENABLE = 79,
    RS_WIREFRAMEOFFSETENABLE = 80, RS_SOLIDOFFSETENABLE = 81,
};
/* texture-stage state indices */
enum {
    TSS_ADDRESSU = 0, TSS_ADDRESSV, TSS_ADDRESSW, TSS_MAGFILTER, TSS_MINFILTER,
    TSS_MIPFILTER, TSS_MIPMAPLODBIAS, TSS_MAXMIPLEVEL, TSS_MAXANISOTROPY,
    TSS_COLORKEYOP, TSS_COLORSIGN, TSS_ALPHAKILL, TSS_COLOROP = 12, TSS_COLORARG0, TSS_COLORARG1, TSS_COLORARG2, TSS_ALPHAOP,
    TSS_ALPHAARG0, TSS_ALPHAARG1, TSS_ALPHAARG2, TSS_RESULTARG, TSS_TEXTURETRANSFORMFLAGS,
    TSS_BUMPENVMAT00 = 22,      /* 00, 01, 10, 11: SetTextureState_BumpEnv pushes types
                                   22..25 to BUMP_ENV_MAT 0..3 (Otogi 0xf5c50) */
    TSS_BUMPENVLSCALE = 26, TSS_BUMPENVLOFFSET,
    TSS_TEXCOORDINDEX = 28, TSS_BORDERCOLOR = 29,
};

/* Two floats (bits) as IEEE halves, a in the low 16 bits (GLSL
 * unpackHalf2x16): rounded to nearest, out of range to infinity. */
static uint32_t half1(uint32_t f)
{
    uint32_t s = (f >> 16) & 0x8000, e = (f >> 23) & 0xFF, m = f & 0x7FFFFF;
    int32_t he = (int32_t)e - 127 + 15;
    if (e == 0xFF) {
        return s | 0x7C00 | (m ? 0x200 : 0);
    }
    if (he >= 31) {
        return s | 0x7C00;
    }
    if (he <= 0) {                              /* a half denormal, or 0 */
        if (he < -10) {
            return s;
        }
        uint32_t shift = 14 - he;
        m |= 0x800000;
        return s | ((m >> shift) + ((m >> (shift - 1)) & 1));
    }
    return (s | ((uint32_t)he << 10) | (m >> 13)) + ((m >> 12) & 1);
}

static uint32_t half2(uint32_t a, uint32_t b)
{
    return half1(a) | half1(b) << 16;
}

typedef struct HleVkTex {
    uint32_t id;                /* front-end object id; 0 = free */
    uint32_t xfmt, w, h, d, levels, faces;
    bool vol;                   /* a volume: view is 3D (binding 3) */
    VkFormat fmt;
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkImageView cube_view;      /* cubes: the six faces as a cube (binding 2) */
    uint32_t slot;              /* sampled_images[] index */
    bool linear;                /* Xbox linear format: texel coordinates */
} HleVkTex;

/* A pipeline table entry's state: made (VK_NULL_HANDLE: it failed, not tried
 * again), being built by the workers (its draws skipped), or made by the
 * prebuild and not used yet this session (its first use is recorded). */
enum { PIPE_READY, PIPE_PENDING, PIPE_PREBUILT };

typedef struct HlePipe {
    uint64_t hash;
    VkPipeline pipe;
    uint8_t state;
} HlePipe;

/* The state commands a draw may record, counted for the per-second log: what
 * the GPU does between draws. */
enum { EM_PIPE, EM_SET1, EM_PC, EM_VI, EM_VB, EM_VP, EM_SC, EM_DYN, EM_FB, EM_N };
static const char *const k_em_names[EM_N] = { "pipe", "set1", "pc", "vi", "vb", "vp", "sc", "dyn", "fb" };

/* Without descriptor indexing (caps.bindless off): a slot as the bindless set
 * would hold it (write_slot_layout3): its 2D, cube and 3D views (the white
 * textures where it has none) and their layouts */
typedef struct FixedImg {
    VkImageView v[3];
    uint32_t layout[3];
} FixedImg;

/* A draw's own set (fixed_bind): its stages' slots and samplers */
typedef struct FixedKey {
    FixedImg img[HLE_FF_STAGES];
    VkSampler smp[HLE_FF_STAGES];
} FixedKey;

#define FX_POOLS 16                     /* a command buffer's pools of draw sets */
#define FX_SETS  1024                   /* sets a pool */

static struct {
    bool ready;
    VkDescriptorSetLayout dsl;          /* caps.bindless off: the draws' own sets' */
    VkDescriptorPool pool;
    VkDescriptorSet set;
    /* caps.bindless off: the slots (write_slot_layout3), and the draws' sets,
     * made from pools per command buffer in flight (hle_draw_slot) */
    FixedImg *mirror;
    struct {
        VkDescriptorPool pool[FX_POOLS];
        unsigned npool, cur;
        GHashTable *sets;               /* FixedKey -> VkDescriptorSet */
    } fx[HLE_FRAMES];
    unsigned fxslot;
    uint64_t fx_sets;                   /* made */
    VkPipelineLayout layout;
    uint32_t next_slot;
    HleVkTex tex[HLE_MAX_TEX];
    HleVkTex white, white_cube, white_3d;
    struct { uint64_t key, key2; VkSampler s; } smp[HLE_MAX_SAMPLERS];
    uint32_t nsmp;
    struct { uint32_t id, ord; VkImageView view, cube; uint64_t gen; uint32_t slot; } sslot[256];
    uint32_t nsslot;
    HlePipe pipes[HLE_MAX_PIPES];
    uint32_t npipes;
    HleCmdState st;
    bool st_valid;
    /* vertex programs: set 1 = the constant file (dynamic uniform buffer) */
    VkDescriptorSetLayout cdsl;
    VkDescriptorSetLayout fbdsl;        /* set 2: the input attachment (push descriptor) */
    uint64_t fb_draws;
    VkDescriptorPool cpool;
    VkDescriptorSet cset;
    VkDescriptorSet fset;               /* set 1 for FF draws: the FF uniform block */
    /* Push constants smaller than the block (caps.push off: Xclipse): set 3,
     * the blocks in a host-visible ring of storage buffer windows, a part per
     * command buffer in flight (hle_draw_slot); set 2 empty without fbfetch */
    VkDescriptorSetLayout pdsl, edsl;
    VkDescriptorPool ppool;
    VkDescriptorSet pset;
    VkBuffer pbuf;
    VkDeviceMemory pmem;
    uint8_t *pmap;
    uint32_t pbase, ppos;               /* the current part's start, its next free byte */
    /* fbfetch without push descriptors (caps.push_desc off, or Mali's driver:
     * HLE_QUIRK_NO_PUSH_DESC): set 2 from a pool per command buffer in flight */
    bool fb_pooled;
    VkDescriptorPool fbpool[HLE_FRAMES];
    unsigned fbslot;
    struct { VkImageView rt, z; VkImageLayout rl, zl; VkDescriptorSet set; } fblast;
    struct { uint64_t hash; uint32_t nslots; uint32_t *tok; } prog[HLE_MAX_PROGS];
    uint32_t nprog;
    int8_t vfmt[256];                   /* vertex-buffer support: 0 unknown, 1 yes, -1 no */
    uint64_t draws, pipe_fail, compiles, prog_draws, layout_skips;
    uint64_t verts;                     /* vertices (indices) drawn */
    uint64_t idx_same, idx_contig;      /* indexed merge potential (hle_draw_ff_diag) */
    uint64_t emits[EM_N];               /* state commands recorded, by EM_* kind */
    uint64_t mergeable;                 /* draws that could have joined the previous one */
    uint64_t nomerge[5];                /* why not: strip/fan, indexed, state, other work, vertex gap */
    uint64_t drops[6];                  /* draws dropped, by DROP_* reason */
    /* asynchronous builds (pipeasync.h): queued to the workers, built by them,
     * built in place (every build with async off; else for a target not
     * redrawn), made from the caches at once; prebuilt pipelines made and
     * first used */
    uint64_t async_queued, async_built, inplace, cache_hits, prebuilt, prebuilt_used;
    uint64_t signed_blends;             /* draws with a signed blend equation, in two */
    /* FF transformed draws by the FF features their state asks for */
    uint64_t ff3d, ff_lit, ff_fog, ff_texgen, ff_texxform, ff_blend, ff_psprite, prog_fog;
} g_dr;

/* HAKUX_HLE_PSLOG=1: log combiner programs and sampled pixel-shader draws. */
static bool pslog_on(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("HAKUX_HLE_PSLOG") != NULL;
    }
    return on;
}

/* ---- formats ------------------------------------------------------------------- */

typedef struct TexFmt {
    VkFormat fmt;
    VkComponentMapping sw;
} TexFmt;

#define SW(r, g, b, a) { VK_COMPONENT_SWIZZLE_##r, VK_COMPONENT_SWIZZLE_##g, \
                         VK_COMPONENT_SWIZZLE_##b, VK_COMPONENT_SWIZZLE_##a }

static bool tex_format(uint32_t x, TexFmt *o)
{
    static const VkComponentMapping id = SW(IDENTITY, IDENTITY, IDENTITY, IDENTITY);
    o->sw = id;
    switch (x) {
    case HLE_XFMT_A8R8G8B8: case HLE_XFMT_LIN_A8R8G8B8:
        o->fmt = VK_FORMAT_B8G8R8A8_UNORM; return true;
    case HLE_XFMT_X8R8G8B8: case HLE_XFMT_LIN_X8R8G8B8:
        o->fmt = VK_FORMAT_B8G8R8A8_UNORM; o->sw.a = VK_COMPONENT_SWIZZLE_ONE; return true;
    /* the other 8888 orders (names MSB first): A8B8G8R8 is bytes R G B A,
     * B8G8R8A8 bytes A R G B, R8G8B8A8 bytes A B G R (Conker's frontend) */
    case HLE_XFMT_A8B8G8R8: case HLE_XFMT_LIN_A8B8G8R8:
        o->fmt = VK_FORMAT_R8G8B8A8_UNORM; return true;
    case HLE_XFMT_B8G8R8A8: case HLE_XFMT_LIN_B8G8R8A8: {
        VkComponentMapping m = SW(G, B, A, R);
        o->fmt = VK_FORMAT_R8G8B8A8_UNORM; o->sw = m; return true;
    }
    case HLE_XFMT_R8G8B8A8: case HLE_XFMT_LIN_R8G8B8A8: {
        VkComponentMapping m = SW(A, B, G, R);
        o->fmt = VK_FORMAT_R8G8B8A8_UNORM; o->sw = m; return true;
    }
    case HLE_XFMT_R5G6B5: case HLE_XFMT_LIN_R5G6B5:
        o->fmt = VK_FORMAT_R5G6B5_UNORM_PACK16; return true;
    case HLE_XFMT_A1R5G5B5: case HLE_XFMT_LIN_A1R5G5B5:
        o->fmt = VK_FORMAT_A1R5G5B5_UNORM_PACK16; return true;
    case HLE_XFMT_X1R5G5B5: case HLE_XFMT_LIN_X1R5G5B5:
        o->fmt = VK_FORMAT_A1R5G5B5_UNORM_PACK16; o->sw.a = VK_COMPONENT_SWIZZLE_ONE;
        return true;
    case HLE_XFMT_A4R4G4B4: case HLE_XFMT_LIN_A4R4G4B4:
        /* else the front end sends them as A8R8G8B8 (pb/pbres.c) */
        o->fmt = VK_FORMAT_A4R4G4B4_UNORM_PACK16_EXT; return g_hvk.caps.x4444;
    case HLE_XFMT_L8: case HLE_XFMT_LIN_L8: {
        VkComponentMapping m = SW(R, R, R, ONE);
        o->fmt = VK_FORMAT_R8_UNORM; o->sw = m; return true;
    }
    case HLE_XFMT_AL8: case HLE_XFMT_LIN_AL8: {
        VkComponentMapping m = SW(R, R, R, R);
        o->fmt = VK_FORMAT_R8_UNORM; o->sw = m; return true;
    }
    case HLE_XFMT_A8: case HLE_XFMT_LIN_A8: {
        VkComponentMapping m = SW(ONE, ONE, ONE, R);   /* xemu: A8 -> (1, 1, 1, a) */
        o->fmt = VK_FORMAT_R8_UNORM; o->sw = m; return true;
    }
    case HLE_XFMT_A8L8: case HLE_XFMT_LIN_A8L8: {
        VkComponentMapping m = SW(R, R, R, G);
        o->fmt = VK_FORMAT_R8G8_UNORM; o->sw = m; return true;
    }
    /* Two 8-bit channels: G8B8 is D3D's V8U8 (bump maps: the low byte B = du,
     * the high byte G = dv), R8B8 the low byte B and the high byte R. The
     * other channel of each repeats as in LLE's map. */
    case HLE_XFMT_G8B8: case HLE_XFMT_LIN_G8B8: {
        VkComponentMapping m = SW(R, G, R, G);
        o->fmt = VK_FORMAT_R8G8_UNORM; o->sw = m; return true;
    }
    case HLE_XFMT_R8B8: case HLE_XFMT_LIN_R8B8: {
        VkComponentMapping m = SW(G, R, R, G);
        o->fmt = VK_FORMAT_R8G8_UNORM; o->sw = m; return true;
    }
    case HLE_XFMT_DXT1: o->fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; return true;
    case HLE_XFMT_DXT3: o->fmt = VK_FORMAT_BC2_UNORM_BLOCK; return true;
    case HLE_XFMT_DXT5: o->fmt = VK_FORMAT_BC3_UNORM_BLOCK; return true;
    default:
        return false;
    }
}

/* ---- descriptors ----------------------------------------------------------------- */

/* Slot of binding 0 (2D views), binding 2 (cube views) and binding 3 (3D
 * views): a texture shows the white texture of the other kinds there, so
 * every slot a shader can index is written. */
static void write_slot_layout3(uint32_t slot, VkImageView view, VkImageView cube,
                               VkImageView vol, VkImageLayout layout)
{
    const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo ii[3] = {
        { .imageView = view ? view : g_dr.white.view, .imageLayout = view ? layout : ro },
        { .imageView = cube ? cube : g_dr.white_cube.cube_view,
          .imageLayout = cube ? layout : ro },
        { .imageView = vol ? vol : g_dr.white_3d.view, .imageLayout = vol ? layout : ro },
    };
    if (!g_hvk.caps.bindless) {         /* the draws' sets take them from here (fixed_bind) */
        for (unsigned i = 0; i < 3 && slot < HLE_MAX_IMAGES; i++) {
            g_dr.mirror[slot].v[i] = ii[i].imageView;
            g_dr.mirror[slot].layout[i] = ii[i].imageLayout;
        }
        return;
    }
    static const uint32_t k_binding[3] = { 0, 2, 3 };
    VkWriteDescriptorSet w[3];
    for (unsigned i = 0; i < 3; i++) {
        w[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_dr.set,
            .dstBinding = k_binding[i], .dstArrayElement = slot, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii[i] };
    }
    T.vkUpdateDescriptorSets(g_hvk.dev, 3, w, 0, NULL);
}

static void write_slot_layout(uint32_t slot, VkImageView view, VkImageView cube,
                              VkImageLayout layout)
{
    write_slot_layout3(slot, view, cube, VK_NULL_HANDLE, layout);
}

static void write_slot(uint32_t slot, VkImageView view, VkImageView cube)
{
    write_slot_layout(slot, view, cube, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static void write_tex_slot(const HleVkTex *t)
{
    if (t->vol) {
        write_slot_layout3(t->slot, VK_NULL_HANDLE, VK_NULL_HANDLE, t->view,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else {
        write_slot(t->slot, t->view, t->cube_view);
    }
}

static void write_image(uint32_t slot, VkImageView view)
{
    write_slot(slot, view, VK_NULL_HANDLE);
}

static bool make_image(HleVkTex *t, const TexFmt *tf, VkImageUsageFlags usage)
{
    bool cube = t->faces == 6;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = t->vol ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        .format = tf->fmt, .extent = { t->w, t->h, t->vol ? MAX(t->d, 1u) : 1 },
        .mipLevels = t->levels,
        .arrayLayers = t->faces, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (T.vkCreateImage(g_hvk.dev, &ici, NULL, &t->img) != VK_SUCCESS) {
        t->img = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req;
    T.vkGetImageMemoryRequirements(g_hvk.dev, t->img, &req);
    int mt = hle_vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &t->mem) != VK_SUCCESS) {
        T.vkDestroyImage(g_hvk.dev, t->img, NULL);
        t->img = VK_NULL_HANDLE;
        return false;
    }
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img,
        .viewType = t->vol ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D,
        .format = tf->fmt, .components = tf->sw,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, t->levels, 0, 1 } };
    t->view = VK_NULL_HANDLE;
    t->cube_view = VK_NULL_HANDLE;
    bool view = false;
    bool ok = hle_vk_check(T.vkBindImageMemory(g_hvk.dev, t->img, t->mem, 0), "vkBindImageMemory (texture)") &&
              (view = hle_vk_check(T.vkCreateImageView(g_hvk.dev, &vci, NULL, &t->view),
                                   "vkCreateImageView (texture)"));
    if (ok && cube) {
        vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        vci.subresourceRange.layerCount = 6;
        ok = hle_vk_check(T.vkCreateImageView(g_hvk.dev, &vci, NULL, &t->cube_view), "vkCreateImageView (cube)");
    }
    if (!ok) {
        /* no image rather than one without views (a null view bound) */
        if (view) {
            T.vkDestroyImageView(g_hvk.dev, t->view, NULL);
        }
        t->view = VK_NULL_HANDLE;
        T.vkDestroyImage(g_hvk.dev, t->img, NULL);
        T.vkFreeMemory(g_hvk.dev, t->mem, NULL);
        t->img = VK_NULL_HANDLE;
        t->mem = VK_NULL_HANDLE;
        t->cube_view = VK_NULL_HANDLE;
    }
    return ok;
}

static void pipe_async_init(void);

#define PUSH_PART_BYTES (4u << 20)      /* a command buffer's push blocks: 16384 */
#define FB_SETS         1024            /* a command buffer's pooled fbfetch sets */

/* caps.push off: the push blocks' set (3) and ring (hle_draw_push) */
static bool push_ring_init(void)
{
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       NULL };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &b };
    VkDescriptorSetLayoutCreateInfo eci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1 };
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
        .poolSizeCount = 1, .pPoolSizes = &ps };
    if (T.vkCreateDescriptorSetLayout(g_hvk.dev, &lci, NULL, &g_dr.pdsl) != VK_SUCCESS ||
        T.vkCreateDescriptorSetLayout(g_hvk.dev, &eci, NULL, &g_dr.edsl) != VK_SUCCESS ||
        T.vkCreateDescriptorPool(g_hvk.dev, &pci, NULL, &g_dr.ppool) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g_dr.ppool, .descriptorSetCount = 1, .pSetLayouts = &g_dr.pdsl };
    const VkDeviceSize size = (VkDeviceSize)HLE_FRAMES * PUSH_PART_BYTES;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (T.vkAllocateDescriptorSets(g_hvk.dev, &ai, &g_dr.pset) != VK_SUCCESS ||
        T.vkCreateBuffer(g_hvk.dev, &bci, NULL, &g_dr.pbuf) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    T.vkGetBufferMemoryRequirements(g_hvk.dev, g_dr.pbuf, &req);
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int mt = hle_vk_memtype(req.memoryTypeBits, hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (mt < 0) {
        mt = hle_vk_memtype(req.memoryTypeBits, hv, 0);
    }
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &g_dr.pmem) != VK_SUCCESS) {
        return false;
    }
    T.vkBindBufferMemory(g_hvk.dev, g_dr.pbuf, g_dr.pmem, 0);
    T.vkMapMemory(g_hvk.dev, g_dr.pmem, 0, size, 0, (void **)&g_dr.pmap);
    VkDescriptorBufferInfo bi = { g_dr.pbuf, 0, HLE_PUSH_BYTES };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_dr.pset, .dstBinding = 0,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
        .pBufferInfo = &bi };
    T.vkUpdateDescriptorSets(g_hvk.dev, 1, &w, 0, NULL);
    hle_log("hakuX-hle-back", "push constants %u bytes < %u: the draws' block in a storage "
            "buffer (%u KiB a command buffer)", g_hvk.props.limits.maxPushConstantsSize,
            HLE_PUSH_BYTES, PUSH_PART_BYTES >> 10);
    return g_dr.pmap != NULL;
}

/* A draw's push block in the ring, bound as set 3 (caps.push off). */
static void push_block(VkCommandBuffer cb, const HleFfPush *pc)
{
    if (g_dr.ppos + HLE_PUSH_BYTES > PUSH_PART_BYTES) {
        static bool s_said;
        if (!s_said) {
            s_said = true;
            hle_log("hakuX-hle-back", "push blocks: a command buffer's %u KiB are used up: "
                    "its later draws keep the last block (logged once)", PUSH_PART_BYTES >> 10);
        }
        return;
    }
    uint32_t off = g_dr.pbase + g_dr.ppos;
    memcpy(g_dr.pmap + off, pc, sizeof(*pc));
    g_dr.ppos += HLE_PUSH_BYTES;
    T.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dr.layout, 3, 1, &g_dr.pset,
                              1, &off);
}

static guint fx_hash(gconstpointer k)
{
    const uint8_t *b = k;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(FixedKey); i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

static gboolean fx_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, sizeof(FixedKey)) == 0;
}

/* caps.bindless off: set 0 is a draw's own (fixed_bind): its stages' images
 * at bindings 0, 2 and 3 (2D, cube, 3D, as the bindless set's), binding 1 the
 * first sampler and the stages' */
static bool fixed_init(void)
{
    VkDescriptorSetLayoutBinding b[4] = {
        { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, HLE_FF_STAGES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1 + HLE_FF_STAGES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, HLE_FF_STAGES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, HLE_FF_STAGES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4, .pBindings = b };
    if (T.vkCreateDescriptorSetLayout(g_hvk.dev, &dci, NULL, &g_dr.dsl) != VK_SUCCESS) {
        return false;
    }
    g_dr.mirror = g_new0(FixedImg, HLE_MAX_IMAGES);
    for (unsigned i = 0; i < HLE_FRAMES; i++) {
        g_dr.fx[i].sets = g_hash_table_new_full(fx_hash, fx_equal, g_free, NULL);
    }
    hle_log("hakuX-hle-back", "no descriptor indexing: each draw's textures in a set of its "
            "own (%u a pool, up to %u pools a command buffer)", FX_SETS, FX_POOLS);
    return true;
}

/* k's set in the current command buffer's pools, made the first time */
static VkDescriptorSet fixed_set(const FixedKey *k)
{
    __typeof__(g_dr.fx[0]) *f = &g_dr.fx[g_dr.fxslot];
    VkDescriptorSet set = (VkDescriptorSet)g_hash_table_lookup(f->sets, k);
    if (set) {
        return set;
    }
    for (;;) {
        if (f->cur == f->npool) {
            VkDescriptorPoolSize ps[2] = {
                { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * HLE_FF_STAGES * FX_SETS },
                { VK_DESCRIPTOR_TYPE_SAMPLER, (1 + HLE_FF_STAGES) * FX_SETS },
            };
            VkDescriptorPoolCreateInfo pci = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = FX_SETS,
                .poolSizeCount = 2, .pPoolSizes = ps };
            if (f->npool == FX_POOLS ||
                T.vkCreateDescriptorPool(g_hvk.dev, &pci, NULL, &f->pool[f->npool]) != VK_SUCCESS) {
                static bool s_said;
                if (!s_said) {
                    s_said = true;
                    hle_log("hakuX-hle-back", "draw sets: a command buffer's %u are used up: its "
                            "later draws keep the last textures (logged once)", FX_POOLS * FX_SETS);
                }
                return VK_NULL_HANDLE;
            }
            f->npool++;
        }
        VkDescriptorSetAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = f->pool[f->cur], .descriptorSetCount = 1, .pSetLayouts = &g_dr.dsl };
        VkResult r = T.vkAllocateDescriptorSets(g_hvk.dev, &ai, &set);
        if (r == VK_SUCCESS) {
            break;
        }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            hle_vk_check(r, "vkAllocateDescriptorSets (draw set)");
            return VK_NULL_HANDLE;
        }
        f->cur++;                       /* this pool is full: the next */
    }
    VkDescriptorImageInfo ii[3][HLE_FF_STAGES], si[1 + HLE_FF_STAGES];
    for (unsigned s = 0; s < HLE_FF_STAGES; s++) {
        for (unsigned v = 0; v < 3; v++) {
            ii[v][s] = (VkDescriptorImageInfo){ .imageView = k->img[s].v[v],
                                                .imageLayout = (VkImageLayout)k->img[s].layout[v] };
        }
        si[1 + s] = (VkDescriptorImageInfo){ .sampler = k->smp[s] };
    }
    si[0] = (VkDescriptorImageInfo){ .sampler = g_dr.smp[0].s };
    static const uint32_t k_binding[3] = { 0, 2, 3 };
    VkWriteDescriptorSet w[4];
    for (unsigned v = 0; v < 3; v++) {
        w[v] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
            .dstBinding = k_binding[v], .descriptorCount = HLE_FF_STAGES,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = ii[v] };
    }
    w[3] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1,
        .descriptorCount = 1 + HLE_FF_STAGES, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
        .pImageInfo = si };
    T.vkUpdateDescriptorSets(g_hvk.dev, 4, w, 0, NULL);
    g_hash_table_insert(f->sets, g_memdup2(k, sizeof(*k)), set);
    g_dr.fx_sets++;
    return set;
}

void hle_draw_slot(unsigned slot)
{
    g_dr.pbase = (slot % HLE_FRAMES) * PUSH_PART_BYTES;
    g_dr.ppos = 0;
    g_dr.fbslot = slot % HLE_FRAMES;
    memset(&g_dr.fblast, 0, sizeof(g_dr.fblast));
    if (g_dr.fb_pooled && g_dr.fbpool[g_dr.fbslot]) {
        T.vkResetDescriptorPool(g_hvk.dev, g_dr.fbpool[g_dr.fbslot], 0);
    }
    if (!g_hvk.caps.bindless && g_dr.mirror) {  /* its previous recording has completed */
        __typeof__(g_dr.fx[0]) *f = &g_dr.fx[g_dr.fxslot = slot % HLE_FRAMES];
        g_hash_table_remove_all(f->sets);
        for (unsigned i = 0; i < f->npool; i++) {
            T.vkResetDescriptorPool(g_hvk.dev, f->pool[i], 0);
        }
        f->cur = 0;
    }
}

int hle_draw_init(void)
{
    VkDescriptorSetLayoutBinding b[4] = {
        { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, HLE_MAX_IMAGES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_SAMPLER, HLE_MAX_SAMPLERS, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, HLE_MAX_IMAGES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, HLE_MAX_IMAGES, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorBindingFlags bf[4] = {
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
    };
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = 4, .pBindingFlags = bf };
    VkDescriptorSetLayoutCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &bfi,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = 4, .pBindings = b };
    VkDescriptorPoolSize ps[2] = {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * HLE_MAX_IMAGES },
        { VK_DESCRIPTOR_TYPE_SAMPLER, HLE_MAX_SAMPLERS },
    };
    VkDescriptorPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, .maxSets = 1,
        .poolSizeCount = 2, .pPoolSizes = ps };
    if (!g_hvk.caps.bindless) {         /* each draw's textures a set of its own */
        if (!fixed_init()) {
            return -1;
        }
    } else {
        if (T.vkCreateDescriptorSetLayout(g_hvk.dev, &dci, NULL, &g_dr.dsl) != VK_SUCCESS) {
            return -1;
        }
        if (T.vkCreateDescriptorPool(g_hvk.dev, &pci, NULL, &g_dr.pool) != VK_SUCCESS) {
            return -1;
        }
        VkDescriptorSetAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = g_dr.pool, .descriptorSetCount = 1, .pSetLayouts = &g_dr.dsl };
        if (T.vkAllocateDescriptorSets(g_hvk.dev, &ai, &g_dr.set) != VK_SUCCESS) {
            return -1;
        }
    }
    /* set 1: vertex-program constants, a dynamic window on the arena */
    VkDescriptorSetLayoutBinding cb = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                                        VK_SHADER_STAGE_VERTEX_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo cci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &cb };
    VkDescriptorPoolSize cps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 };
    VkDescriptorPoolCreateInfo cpi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2,
        .poolSizeCount = 1, .pPoolSizes = &cps };
    if (T.vkCreateDescriptorSetLayout(g_hvk.dev, &cci, NULL, &g_dr.cdsl) != VK_SUCCESS ||
        T.vkCreateDescriptorPool(g_hvk.dev, &cpi, NULL, &g_dr.cpool) != VK_SUCCESS) {
        return -1;
    }
    VkDescriptorSetAllocateInfo cai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g_dr.cpool, .descriptorSetCount = 1, .pSetLayouts = &g_dr.cdsl };
    if (T.vkAllocateDescriptorSets(g_hvk.dev, &cai, &g_dr.cset) != VK_SUCCESS ||
        T.vkAllocateDescriptorSets(g_hvk.dev, &cai, &g_dr.fset) != VK_SUCCESS) {
        return -1;
    }
    VkDescriptorBufferInfo cbi[2] = { { g_harena.buf, 0, HLE_CONST_BYTES },
                                      { g_harena.buf, 0, HLE_FFU_COUNT * 16 } };
    VkWriteDescriptorSet cw[2];
    for (unsigned i = 0; i < 2; i++) {
        cw[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = i ? g_dr.fset : g_dr.cset,
            .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &cbi[i] };
    }
    T.vkUpdateDescriptorSets(g_hvk.dev, 2, cw, 0, NULL);
    /* HLE_PUSH_GLSL's std430 offsets */
    HLE_BUILD_BUG_ON(offsetof(HleFfPush, bump3) != 72 || offsetof(HleFfPush, tex) != 80 ||
                      offsetof(HleFfPush, bumpl) != 100 || offsetof(HleFfPush, alpha_ref) != 112 ||
                      offsetof(HleFfPush, fog) != 128 || offsetof(HleFfPush, bump) != 240 ||
                      sizeof(HleFfPush) != 256);
    HLE_BUILD_BUG_ON(sizeof(HleFfPush) != HLE_PUSH_BYTES);
    g_hle_push_ssbo = !g_hvk.caps.push;     /* before any shader is made */
    g_hle_glsl_zw = hle_quirk(HLE_QUIRK_Z_EQ_W);
    g_hle_glsl_zclamp = !g_hvk.caps.depth_clamp;
    g_hle_glsl_fixed = !g_hvk.caps.bindless;
    if (g_hle_push_ssbo && !push_ring_init()) {
        hle_log("hakuX-hle-back", "push blocks: no storage buffer ring");
        return -1;
    }
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(HleFfPush) };
    /* set 2 (devices with fbfetch): the colour target and the depth buffer
     * as input attachments, pushed, or from a pool where push descriptors
     * can't be used (Arm's driver crashes in them: ARMSX2) */
    g_dr.fb_pooled = !g_hvk.caps.push_desc || hle_quirk(HLE_QUIRK_NO_PUSH_DESC);
    if (g_hvk.caps.fbfetch) {
        VkDescriptorSetLayoutBinding fb[2] = {
            { 0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
            { 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
        VkDescriptorSetLayoutCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = g_dr.fb_pooled ? 0 : VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
            .bindingCount = 2, .pBindings = fb };
        if (T.vkCreateDescriptorSetLayout(g_hvk.dev, &fci, NULL, &g_dr.fbdsl) != VK_SUCCESS) {
            g_hvk.caps.fbfetch = false;
        }
        VkDescriptorPoolSize fps = { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 2 * FB_SETS };
        VkDescriptorPoolCreateInfo fpi = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = FB_SETS,
            .poolSizeCount = 1, .pPoolSizes = &fps };
        for (unsigned i = 0; g_hvk.caps.fbfetch && g_dr.fb_pooled && i < HLE_FRAMES; i++) {
            if (T.vkCreateDescriptorPool(g_hvk.dev, &fpi, NULL, &g_dr.fbpool[i]) != VK_SUCCESS) {
                g_hvk.caps.fbfetch = false;
            }
        }
        if (g_hvk.caps.fbfetch && g_dr.fb_pooled) {
            hle_log("hakuX-hle-back", "fbfetch: input attachments from pooled sets (%s)",
                    g_hvk.caps.push_desc ? "the driver's push descriptors are avoided" :
                    "no push descriptors");
        }
    }
    VkDescriptorSetLayout sets[4] = { g_dr.dsl, g_dr.cdsl,
                                      g_hvk.caps.fbfetch ? g_dr.fbdsl : g_dr.edsl, g_dr.pdsl };
    VkPipelineLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = g_hle_push_ssbo ? 4 : g_hvk.caps.fbfetch ? 3 : 2,
        .pSetLayouts = sets, .pushConstantRangeCount = g_hle_push_ssbo ? 0 : 1,
        .pPushConstantRanges = &pr };
    if (T.vkCreatePipelineLayout(g_hvk.dev, &lci, NULL, &g_dr.layout) != VK_SUCCESS) {
        return -1;
    }
    /* slot 0: a 1x1 white texture; sampler 0: the default (point, wrap) */
    g_dr.white = (HleVkTex){ .w = 1, .h = 1, .levels = 1, .faces = 1 };
    TexFmt wf = { VK_FORMAT_B8G8R8A8_UNORM, SW(IDENTITY, IDENTITY, IDENTITY, IDENTITY) };
    if (!make_image(&g_dr.white, &wf, VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        return -1;
    }
    g_dr.white_cube = (HleVkTex){ .w = 1, .h = 1, .levels = 1, .faces = 6 };
    if (!make_image(&g_dr.white_cube, &wf, VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        return -1;
    }
    g_dr.white_3d = (HleVkTex){ .w = 1, .h = 1, .d = 1, .levels = 1, .faces = 1, .vol = true };
    if (!make_image(&g_dr.white_3d, &wf, VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        return -1;
    }
    for (uint32_t i = 0; g_dr.mirror && i < HLE_MAX_IMAGES; i++) {
        /* every slot the white textures until written (all a set's must be) */
        g_dr.mirror[i] = (FixedImg){
            { g_dr.white.view, g_dr.white_cube.cube_view, g_dr.white_3d.view },
            { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
    }
    {   /* DXT in 3D images (volumes are decoded on the CPU otherwise) */
        VkImageFormatProperties fp;
        VkResult r = vkGetPhysicalDeviceImageFormatProperties(
            g_hvk.phys, VK_FORMAT_BC3_UNORM_BLOCK, VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &fp);
        hle_log("hakuX-hle-back", "BC3 3D images: %s (max %ux%ux%u)",
                r == VK_SUCCESS ? "supported" : "not supported", fp.maxExtent.width,
                fp.maxExtent.height, fp.maxExtent.depth);
    }
    g_dr.next_slot = 1;
    hle_glsl_init();
    pipe_async_init();                  /* before the caches: they read its nocache switch */
    hle_pcache_init();
    /* glslang builds its per-stage built-in tables on first use (~120 ms on
     * the Odin 3): pay it here, at device creation, not at the first draw */
    static const char k_wvs[] = "#version 450\nvoid main() { gl_Position = vec4(0.0); }\n";
    static const char k_wfs[] = "#version 450\nlayout(location = 0) out vec4 o;\n"
                                "void main() { o = vec4(1.0); }\n";
    size_t nw;
    g_free(hle_glsl_compile(HLE_SHADER_VERTEX, k_wvs, &nw));
    g_free(hle_glsl_compile(HLE_SHADER_FRAGMENT, k_wfs, &nw));
    g_dr.ready = true;
    return 0;
}

/* The white texture's contents and layout, recorded once into the first
 * command buffer, before any pass (a clear and layout changes are not
 * allowed in one; at the first draw they were, the validation layer said). */
static void white_init(VkCommandBuffer cb)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    VkClearColorValue c = { .float32 = { 1, 1, 1, 1 } };
    VkImage imgs[3] = { g_dr.white.img, g_dr.white_cube.img, g_dr.white_3d.img };
    for (unsigned i = 0; i < 3; i++) {
        VkImageMemoryBarrier2 b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = imgs[i],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, i == 1 ? 6 : 1 } };
        VkDependencyInfo di = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b };
        T.vkCmdPipelineBarrier2(cb, &di);
        T.vkCmdClearColorImage(cb, imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1,
                               &b.subresourceRange);
        b.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        T.vkCmdPipelineBarrier2(cb, &di);
    }
    write_image(0, g_dr.white.view);
}

/* ---- textures ------------------------------------------------------------------ */

/* Lookup by id (many per draw): a per-id hint, checked on use, and misses
 * remembered until a texture is created (g_tex_gen). */
#define TEX_IDS 8193
static uint16_t g_tex_hint[TEX_IDS];
static uint32_t g_tex_miss[TEX_IDS], g_tex_gen = 1;

static HleVkTex *tex_find(uint32_t id)
{
    if (!id) {
        return NULL;
    }
    if (id < TEX_IDS) {
        unsigned h = g_tex_hint[id];
        if (h && g_dr.tex[h - 1].id == id) {
            return &g_dr.tex[h - 1];
        }
        if (g_tex_miss[id] == g_tex_gen) {
            return NULL;
        }
    }
    for (unsigned i = 0; i < HLE_MAX_TEX; i++) {
        if (g_dr.tex[i].id == id) {
            if (id < TEX_IDS) {
                g_tex_hint[id] = i + 1;
            }
            return &g_dr.tex[i];
        }
    }
    if (id < TEX_IDS) {
        g_tex_miss[id] = g_tex_gen;
    }
    return NULL;
}

/* Retired textures: the image, its views and memory are destroyed and the
 * bindless slot reused once the timeline passes the work that may sample
 * them (HLE_SURF_PARKED: the submission that closes the open recording). */
#define TEX_RETIRE_MAX 2048
static struct {
    VkImage img;
    VkImageView view, cube_view;
    VkDeviceMemory mem;
    uint32_t slot;
    uint64_t after;
} g_retire[TEX_RETIRE_MAX];
static unsigned g_nretire;
static uint32_t g_free_slot[HLE_MAX_IMAGES];
static unsigned g_nfree_slot;
static uint64_t g_tex_retired;

void hle_draw_tex_reap(uint64_t completed)
{
    unsigned j = 0;
    for (unsigned i = 0; i < g_nretire; i++) {
        if (g_retire[i].after == HLE_SURF_PARKED || g_retire[i].after > completed) {
            g_retire[j++] = g_retire[i];
            continue;
        }
        if (g_retire[i].cube_view) {
            T.vkDestroyImageView(g_hvk.dev, g_retire[i].cube_view, NULL);
        }
        T.vkDestroyImageView(g_hvk.dev, g_retire[i].view, NULL);
        T.vkDestroyImage(g_hvk.dev, g_retire[i].img, NULL);
        T.vkFreeMemory(g_hvk.dev, g_retire[i].mem, NULL);
        if (g_retire[i].slot && g_nfree_slot < ARRAY_SIZE(g_free_slot)) {
            g_free_slot[g_nfree_slot++] = g_retire[i].slot;
        }
        g_tex_retired++;
    }
    g_nretire = j;
}

void hle_draw_tex_stamp(uint64_t value)
{
    for (unsigned i = 0; i < g_nretire; i++) {
        if (g_retire[i].after == HLE_SURF_PARKED) {
            g_retire[i].after = value;
        }
    }
}

static void tex_free(HleVkTex *t)
{
    if (t->img) {
        if (g_nretire == TEX_RETIRE_MAX) {
            hle_tl_wait(hle_tl_submitted());    /* full: everything before is done */
            hle_draw_tex_reap(hle_tl_completed());
        }
        g_retire[g_nretire++] = (typeof(g_retire[0])){
            .img = t->img, .view = t->view, .cube_view = t->cube_view, .mem = t->mem,
            .slot = t->slot, .after = HLE_SURF_PARKED };
    }
    memset(t, 0, sizeof(*t));
}

static uint32_t take_slot(void)
{
    if (g_nfree_slot) {
        return g_free_slot[--g_nfree_slot];
    }
    return g_dr.next_slot < HLE_MAX_IMAGES ? g_dr.next_slot++ : 0;
}

static uint32_t level_bytes(uint32_t xfmt, uint32_t w, uint32_t h, uint32_t *row)
{
    const HleFmtInfo *fi = hle_fmt_info(xfmt);
    if (fi->flags & HLE_FI_DXT) {
        *row = (w + 3) / 4 * fi->bpp * 2;
        return *row * ((h + 3) / 4);
    }
    *row = w * fi->bpp / 8;
    return *row * h;
}

void hle_draw_cmd_tex(const HleCmdTex *c, VkCommandBuffer cb)
{
    TexFmt tf;
    if (g_dr.ready && !tex_format(c->xfmt, &tf)) {
        static uint64_t s_logged[4];            /* each format once (codes < 256) */
        if (c->xfmt < 256 && !(s_logged[c->xfmt >> 6] & (1ull << (c->xfmt & 63)))) {
            s_logged[c->xfmt >> 6] |= 1ull << (c->xfmt & 63);
            hle_log("hakuX-hle-back", "texture %u: format 0x%x has no Vulkan format: not "
                    "drawn (%ux%u)", c->id, c->xfmt, c->w, c->h);
        }
        return;
    }
    if (!g_dr.ready || (c->faces != 1 && c->faces != 6)) {
        return;
    }
    HleVkTex *t = tex_find(c->id);
    uint32_t cd = c->faces == 1 && c->d > 1 ? c->d : 1;
    if (t && (t->xfmt != c->xfmt || t->w != c->w || t->h != c->h || t->levels != c->levels ||
              t->faces != c->faces || t->d != cd)) {
        tex_free(t);
        t = NULL;
    }
    if (!t) {
        for (unsigned i = 0; i < HLE_MAX_TEX && !t; i++) {
            if (!g_dr.tex[i].id) {
                t = &g_dr.tex[i];
            }
        }
        if (!t || (!g_nfree_slot && g_dr.next_slot >= HLE_MAX_IMAGES)) {
            static unsigned s_full;
            if (s_full++ < 8) {
                hle_log("hakuX-hle-back", "texture %u (%ux%u fmt 0x%x): not created, "
                        "%s full (sampled white)", c->id, c->w, c->h, c->xfmt,
                        t ? "bindless slots" : "texture table");
            }
            return;
        }
        *t = (HleVkTex){ .id = c->id, .xfmt = c->xfmt, .w = c->w, .h = c->h, .d = cd,
                         .levels = c->levels, .faces = c->faces, .fmt = tf.fmt,
                         .vol = cd > 1 };
        g_tex_gen++;                    /* remembered misses may now hit */
        if (!make_image(t, &tf, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            static unsigned s_fail;
            if (s_fail++ < 8) {
                hle_log("hakuX-hle-back", "texture %u (%ux%u fmt 0x%x): image not created "
                        "(sampled white)", c->id, c->w, c->h, c->xfmt);
            }
            t->id = 0;
            return;
        }
        t->slot = take_slot();
        t->linear = (hle_fmt_info(c->xfmt)->flags & HLE_FI_LIN) != 0;
        write_tex_slot(t);
    }
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = t->img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, t->levels, 0, t->faces } };
    VkDependencyInfo di = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                            .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b };
    T.vkCmdPipelineBarrier2(cb, &di);
    /* arena: face-major, then levels (front end, HleCmdTex) */
    VkBufferImageCopy r[6 * 16];
    uint32_t pos = c->arena_off, n = MIN(c->levels, 16u), nr = 0;
    for (uint32_t f = 0; f < t->faces; f++) {
        for (uint32_t l = 0; l < c->levels; l++) {
            uint32_t w = MAX(c->w >> l, 1u), h = MAX(c->h >> l, 1u), row;
            uint32_t dd = MAX(cd >> l, 1u), bytes = level_bytes(c->xfmt, w, h, &row) * dd;
            if (l < n) {
                r[nr++] = (VkBufferImageCopy){
                    .bufferOffset = pos,
                    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, f, 1 },
                    .imageExtent = { w, h, dd } };
            }
            pos += bytes;
        }
    }
    T.vkCmdCopyBufferToImage(cb, g_harena.buf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             nr, r);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    T.vkCmdPipelineBarrier2(cb, &di);
}

/* A render-target surface sampled as a texture: its own bindless slot,
 * rewritten when the surface's views were made again (by generation: a view
 * made after the old one was destroyed can get its handle, and the slot then
 * kept the dead image's descriptor; Conker's bloom read its back buffer black
 * every other frame once the device's frame buffers were redefined). A
 * texture whose levels or faces are rendered samples all of them (cube:
 * binding 2). ord: a 32-bit texture of another byte order samples the
 * target's texels reordered (hle_surf_order_view), through a slot of its own. */
static uint32_t surf_slot(HleVkSurf *s, uint32_t ord)
{
    VkImageView view = ord ? hle_surf_order_view(s, ord) : VK_NULL_HANDLE, cube = VK_NULL_HANDLE;
    uint64_t gen = s->oview_gen;
    VkImageLayout layout = s->general ? VK_IMAGE_LAYOUT_GENERAL
                                      : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!view) {
        ord = 0;
        view = s->view;
        if (s->general || !(s->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ||
            s->fmt == VK_FORMAT_R8_UNORM || s->fmt == VK_FORMAT_R16_UNORM ||
            s->fmt == VK_FORMAT_R8G8_UNORM || hle_surf_x_alpha(s)) {
            /* depth: the depth aspect alone; B8, L16: the luminance swizzle; G8B8 (b, g, b, g);
             * X8R8G8B8 and X1R5G5B5: alpha 1 */
            hle_surf_sample_views(s, &view, &cube);
        }
        gen = s->view_gen;
    }
    for (uint32_t i = 0; i < g_dr.nsslot; i++) {
        if (g_dr.sslot[i].id == s->id && g_dr.sslot[i].ord == ord) {
            if (g_dr.sslot[i].view != view || g_dr.sslot[i].cube != cube ||
                g_dr.sslot[i].gen != gen) {
                g_dr.sslot[i].view = view;
                g_dr.sslot[i].cube = cube;
                g_dr.sslot[i].gen = gen;
                write_slot_layout(g_dr.sslot[i].slot, view, cube, layout);
            }
            return g_dr.sslot[i].slot;
        }
    }
    uint32_t slot = g_dr.nsslot < ARRAY_SIZE(g_dr.sslot) ? take_slot() : 0;
    if (!slot) {
        return 0;
    }
    uint32_t i = g_dr.nsslot++;
    g_dr.sslot[i].id = s->id;
    g_dr.sslot[i].ord = ord;
    g_dr.sslot[i].view = view;
    g_dr.sslot[i].cube = cube;
    g_dr.sslot[i].gen = gen;
    g_dr.sslot[i].slot = slot;
    write_slot_layout(slot, view, cube, layout);
    return slot;
}

/* A surface object goes: its slots (one per byte order it was sampled in)
 * are reused once recordings that may sample it complete. */
void hle_draw_forget_surf(uint32_t id)
{
    for (uint32_t i = 0; i < g_dr.nsslot;) {
        if (g_dr.sslot[i].id != id) {
            i++;
            continue;
        }
        if (g_nretire == TEX_RETIRE_MAX) {
            hle_tl_wait(hle_tl_submitted());
            hle_draw_tex_reap(hle_tl_completed());
        }
        g_retire[g_nretire++] = (typeof(g_retire[0])){
            .slot = g_dr.sslot[i].slot, .after = HLE_SURF_PARKED };
        g_dr.sslot[i] = g_dr.sslot[--g_dr.nsslot];
    }
}

/* id == rt_id: a texture whose other levels are sampled while one renders
 * (mip chains); other surfaces never sample themselves. Depth surfaces are
 * shadow buffers. */
static HleVkSurf *tex_surface(uint32_t id, uint32_t rt_id)
{
    if (!id || tex_find(id)) {
        return NULL;
    }
    HleVkSurf *s = hle_surf_get(id);
    if (!s || !s->img || s->d.parent || (id == rt_id && !s->general)) {
        return NULL;
    }
    return s;
}

/* Shadow-buffer depth range of the stage's texture (HlePshKey.shadow_fmt):
 * 0 not a depth surface. */
static unsigned shadow_fmt(uint32_t id)
{
    HleVkSurf *s = id && !tex_find(id) ? hle_surf_get(id) : NULL;
    if (!s || !s->img || (s->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
        return 0;
    }
    switch (s->d.xfmt) {
    case HLE_XFMT_D16: case HLE_XFMT_LIN_D16:   return 2;
    case HLE_XFMT_F24S8: case HLE_XFMT_LIN_F24S8:
    case HLE_XFMT_F16: case HLE_XFMT_LIN_F16:   return 3;
    default:                                    return 1;
    }
}

/* The draw blends with a signed equation the shader does (sblend): 1
 * ADDSIGNED, 2 REVSUBTRACTSIGNED; 0 none (or factors other than ONE / ONE,
 * drawn unsigned). */
static unsigned signed_blend(const HleCmdState *s)
{
    uint32_t op = s->rs[RS_BLENDOP];
    if (!g_hvk.caps.fbfetch || !s->rs[RS_ALPHABLENDENABLE] || (op != 0xF005 && op != 0xF006) ||
        s->rs[RS_SRCBLEND] != 1 || s->rs[RS_DESTBLEND] != 1) {
        return 0;
    }
    return op == 0xF006 ? 1 : 2;
}

static bool is_colour_rt(uint32_t rt_id)
{
    HleVkSurf *rt = hle_surf_get(rt_id);
    return rt && (rt->aspect & VK_IMAGE_ASPECT_COLOR_BIT);
}

/*
 * Fixed-function stages (the key's: from the cascade's first) whose texture
 * is the colour target the draw renders: they read its pixel (fbfetch), as
 * the NV2A's texture unit reads the memory being rendered. Splinter Cell's
 * Palm draws its persistence quad last, the front buffer as its texture at
 * a fixed address, which on every other frame is the back buffer it draws
 * into; refused as a texture, it sampled white, a white flash fading with
 * the effect's alpha after each move in the menus.
 */
static unsigned ff_fb_stages(const HleCmdDraw *c, uint32_t rt_id)
{
    const HleCmdState *s = &g_dr.st;
    if (!g_hvk.caps.fbfetch || !rt_id || !g_dr.st_valid || s->psh) {
        return 0;
    }
    unsigned first = s->rs[RS_POINTSPRITEENABLE] ? HLE_FF_SPRITE_STAGE : 0, m = 0;
    for (unsigned st = first; st < HLE_FF_STAGES && s->tss[st][TSS_COLOROP] > 1; st++) {
        if (c->tex[st] == rt_id) {
            m |= 1u << (st - first);
        }
    }
    return m && !tex_find(rt_id) && is_colour_rt(rt_id) ? m : 0;
}

/* Several times a draw: the target is looked up only when a stage samples it. */
unsigned hle_draw_fb_stages(const HleCmdDraw *c, uint32_t rt_id)
{
    if (!g_hvk.caps.fbfetch || !rt_id || !g_dr.st_valid) {
        return 0;
    }
    if (signed_blend(&g_dr.st)) {
        return is_colour_rt(rt_id) ? 0x10 : 0;
    }
    if (!g_dr.st.psh) {
        return ff_fb_stages(c, rt_id);
    }
    unsigned m = 0;
    for (unsigned st = 0; st < HLE_TSS_STAGES; st++) {
        unsigned mode = (g_dr.st.rs[HLE_PS_TEXMODES] >> (5 * st)) & 0x1F;
        if (c->tex[st] == rt_id && (mode == HLE_PST_PROJECT2D || mode == HLE_PST_PROJECT3D)) {
            m |= 1u << st;
        }
    }
    return m && is_colour_rt(rt_id) ? m : 0;
}

bool g_hle_fbbar_off;
int g_hle_gpucut;

void hle_draw_counts(uint64_t *verts, uint64_t *pipes)
{
    *verts = g_dr.verts;
    *pipes = g_dr.emits[EM_PIPE];
}

void hle_draw_rs_brief(uint32_t out[9])
{
    const uint32_t *rs = g_dr.st.rs;
    out[0] = rs[RS_ALPHATESTENABLE];
    out[1] = rs[RS_ALPHAFUNC];
    out[2] = rs[RS_ALPHAREF];
    out[3] = rs[RS_ALPHABLENDENABLE];
    out[4] = rs[RS_SRCBLEND] << 16 | (rs[RS_DESTBLEND] & 0xFFFF);
    out[5] = rs[RS_ZENABLE] << 16 | rs[RS_ZWRITEENABLE];
    out[6] = rs[RS_ZFUNC];
    out[7] = rs[RS_COLORWRITEENABLE];
    out[8] = rs[RS_BLENDCOLOR];
}

/*
 * EQUAL passes that do not write (eq_tol in hle_draw_cmd_draw) test
 * LESSEQUAL a step toward the eye, which also passes every fragment in front
 * of the stored depth. Chaos Theory draws its foliage's depth alpha-tested,
 * then its colour over it EQUAL with the alpha test off: the whole cards
 * passed over the sky and terrain behind the leaves, grey quads. Their
 * shaders read the stored depth (an input attachment) and discard what is
 * more than a few steps in front of it. Programs and FF draws on D24 / D16
 * buffers without W-buffering or a polygon offset (gl_FragCoord.z is the
 * depth without them); debug.hakux.eqtol=1 leaves the one-sided test.
 */
/* debug.hakux.eqtol=0: EQUAL depth tests exact; 1: one-sided (LESSEQUAL a step
 * toward the eye, no shader test) */
static bool g_eqtol_off, g_zeq_off;
/* debug.hakux.snap=trunc: vertex positions truncated to 1/16 pixel each, as
 * before the constant subpixel shift (ff.h HLE_SUBPIXEL_GLSL), for A/B */
static bool g_snap_trunc;
/* New at each hle_draw_psframe_poll that reads the properties (every 3
 * swaps): the draw path reads its own (skipvsh, vtok, psdbg) at its next draw */
static unsigned g_prop_gen;
unsigned g_hle_zeq;

unsigned hle_draw_zeq(const HleCmdDraw *c, uint32_t ds_id)
{
    const uint32_t *rs = g_dr.st.rs;
    if (!g_hvk.caps.fbfetch || !g_dr.st_valid || !hle_quirk(HLE_QUIRK_EQ_DEPTH_STEP) || g_eqtol_off ||
        g_zeq_off || !ds_id ||
        rs[RS_ZENABLE] != 1 || rs[RS_ZWRITEENABLE] || rs[RS_ZFUNC] != 0x202 ||
        rs[RS_SOLIDOFFSETENABLE] || c->prim < VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
        return 0;
    }
    HleVkSurf *ds = hle_surf_get(ds_id);
    if (!ds || ds->d.parent || !(ds->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        return 0;
    }
    switch (ds->fmt) {
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32:
        return HLE_FB_ZEQ24;
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_D16_UNORM_S8_UINT:
        return HLE_FB_ZEQ16;
    default:
        return 0;
    }
}

/* fbfetch's set 2 without push descriptors: a set from the command buffer's
 * pool, the last one again while the attachments are the same */
static void fb_bind_pooled(VkCommandBuffer cb, HleVkSurf *rt, VkImageView zv,
                           const VkDescriptorImageInfo *ii, VkWriteDescriptorSet *w, unsigned nw)
{
    VkImageView rv = rt ? ii[0].imageView : VK_NULL_HANDLE;
    VkImageLayout rl = rt ? ii[0].imageLayout : VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout zl = zv ? ii[nw - 1].imageLayout : VK_IMAGE_LAYOUT_UNDEFINED;
    VkDescriptorSet set = g_dr.fblast.set;
    if (!set || g_dr.fblast.rt != rv || g_dr.fblast.z != zv || g_dr.fblast.rl != rl ||
        g_dr.fblast.zl != zl) {
        VkDescriptorSetAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = g_dr.fbpool[g_dr.fbslot], .descriptorSetCount = 1,
            .pSetLayouts = &g_dr.fbdsl };
        if (T.vkAllocateDescriptorSets(g_hvk.dev, &ai, &set) != VK_SUCCESS) {
            static bool s_said;
            if (!s_said) {
                s_said = true;
                hle_log("hakuX-hle-back", "fbfetch: a command buffer's %u sets are used up: its "
                        "later draws keep the last (logged once)", FB_SETS);
            }
            return;
        }
        for (unsigned i = 0; i < nw; i++) {
            w[i].dstSet = set;
        }
        T.vkUpdateDescriptorSets(g_hvk.dev, nw, w, 0, NULL);
        g_dr.fblast.rt = rv;
        g_dr.fblast.z = zv;
        g_dr.fblast.rl = rl;
        g_dr.fblast.zl = zl;
        g_dr.fblast.set = set;
    }
    T.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dr.layout, 2, 1, &set, 0, NULL);
}

void hle_draw_fb_bind(VkCommandBuffer cb, HleVkSurf *rt, HleVkSurf *ds, VkImageView zv,
                      bool zbar)
{
    /* by region: each fragment reads its own pixel, as the earlier draws
     * left it */
    VkMemoryBarrier2 mb[2];
    unsigned nb = 0;
    if (rt) {
        mb[nb++] = (VkMemoryBarrier2){
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT };
    }
    if (zv && zbar) {
        mb[nb++] = (VkMemoryBarrier2){
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT };
    }
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
        .memoryBarrierCount = nb, .pMemoryBarriers = mb };
    if (nb && !g_hle_fbbar_off) {
        T.vkCmdPipelineBarrier2(cb, &di);
    }
    VkDescriptorImageInfo ii[2];
    VkWriteDescriptorSet w[2];
    unsigned nw = 0;
    for (unsigned b = 0; b < 2; b++) {
        if (b ? !zv : !rt) {
            continue;
        }
        ii[nw] = b ? (VkDescriptorImageInfo){ .imageView = zv, .imageLayout = ds->layout }
                   : (VkDescriptorImageInfo){ .imageView = rt->view, .imageLayout = rt->layout };
        w[nw] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b,
            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
            .pImageInfo = &ii[nw] };
        nw++;
    }
    g_dr.fb_draws += rt != NULL;
    if (g_dr.fb_pooled) {
        fb_bind_pooled(cb, rt, zv, ii, w, nw);
    } else {
        T.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dr.layout, 2, nw, w);
    }
    g_dr.emits[EM_FB]++;
}

bool hle_draw_tex_needs_prep(const HleCmdDraw *c, uint32_t rt_id)
{
    for (unsigned st = 0; st < HLE_TSS_STAGES; st++) {
        HleVkSurf *s = tex_surface(c->tex[st], rt_id);
        if (s && (s->general ? hle_surf_read_pending(s, rt_id)
                             : s->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
            return true;
        }
    }
    return false;
}

/* Outside a pass: sampled surfaces go to SHADER_READ_ONLY (open_pass turns
 * them back into attachments when they are rendered to again). */
void hle_draw_tex_prep(const HleCmdDraw *c, uint32_t rt_id, VkCommandBuffer cb)
{
    for (unsigned st = 0; st < HLE_TSS_STAGES; st++) {
        HleVkSurf *s = tex_surface(c->tex[st], rt_id);
        if (s) {
            if (s->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                /* its texels are undefined: nothing rendered into it yet */
                static unsigned s_logged;
                if (s_logged++ < 16) {
                    hle_log("hakuX-hle-back", "surface %u (fmt 0x%x %ux%u pa %08x) sampled before "
                            "anything rendered into it (stage %u, target %u; first 16 logged)",
                            s->id, s->d.xfmt, s->d.w, s->d.h, s->d.pa, st, rt_id);
                }
            }
            hle_surf_transition(cb, s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
}

/*
 * Combiner stages reading the colour target their draw renders at a
 * coordinate of their own (dependent reads: DOT_ST, the bump-env modes ...)
 * sample a copy of it made just before the draw, as the NV2A's texture unit
 * reads the memory the draw has not yet written (fbfetch serves only the
 * fragment's own pixel). Chaos Theory's night and thermal visions turn the
 * scene into luminance in the back buffer's alpha that way (stage 3 DOT_ST
 * on the back buffer); refused as a texture it sampled white, and every
 * vision mode washed out white. A copy per (format, size), reused.
 */
#define SNAP_MAX 4
static struct {
    VkFormat fmt;
    uint32_t w, h, swz;                 /* swz: hle_surf_sample_views' swizzle (1 B8, 2 X8, 3 G8B8) */
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkImageLayout layout;
    uint32_t slot;
} g_snap[SNAP_MAX];
static unsigned g_nsnap;
static int g_snap_cur = -1;             /* the copy of target g_snap_rt the next draw samples */
static uint32_t g_snap_rt;

static bool samples(unsigned mode)
{
    return mode != HLE_PST_NONE && mode != HLE_PST_PASSTHRU && mode != HLE_PST_CLIPPLANE &&
           mode != HLE_PST_DOTPRODUCT;
}

bool hle_draw_snap_needed(const HleCmdDraw *c, uint32_t rt_id)
{
    if (!rt_id || !g_dr.st_valid || !g_dr.st.psh) {
        return false;
    }
    unsigned m = 0;
    for (unsigned st = 0; st < HLE_TSS_STAGES; st++) {
        unsigned mode = (g_dr.st.rs[HLE_PS_TEXMODES] >> (5 * st)) & 0x1F;
        if (c->tex[st] == rt_id && samples(mode)) {
            m |= 1u << st;
        }
    }
    HleVkSurf *rt = m && !tex_find(rt_id) ? hle_surf_get(rt_id) : NULL;
    if (!rt || !rt->img || rt->general || rt->d.parent ||
        !(rt->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
        return false;
    }
    return (m & ~hle_draw_fb_stages(c, rt_id)) != 0;
}

static int snap_get(const HleVkSurf *rt)
{
    uint32_t swz = rt->fmt == VK_FORMAT_R8_UNORM ? 1 : hle_surf_x_alpha(rt) ? 2 :
                   rt->fmt == VK_FORMAT_R8G8_UNORM ? 3 : 0;
    for (unsigned i = 0; i < g_nsnap; i++) {
        if (g_snap[i].fmt == rt->fmt && g_snap[i].w == rt->w && g_snap[i].h == rt->h &&
            g_snap[i].swz == swz) {
            return (int)i;
        }
    }
    uint32_t slot = g_nsnap < SNAP_MAX ? take_slot() : 0;
    if (!slot) {
        return -1;
    }
    typeof(g_snap[0]) *s = &g_snap[g_nsnap];
    *s = (typeof(g_snap[0])){ .fmt = rt->fmt, .w = rt->w, .h = rt->h, .swz = swz,
                              .layout = VK_IMAGE_LAYOUT_UNDEFINED, .slot = slot };
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = rt->fmt, .extent = { rt->w, rt->h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryRequirements req;
    int mt = -1;
    if (T.vkCreateImage(g_hvk.dev, &ici, NULL, &s->img) == VK_SUCCESS) {
        T.vkGetImageMemoryRequirements(g_hvk.dev, s->img, &req);
        mt = hle_vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mt >= 0 ? req.size : 0,
                                 .memoryTypeIndex = (uint32_t)mt };
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = rt->fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    if (swz == 1) {
        vci.components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                               VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE };
    } else if (swz == 2) {
        vci.components.a = VK_COMPONENT_SWIZZLE_ONE;
    } else if (swz == 3) {
        vci.components = (VkComponentMapping){ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                                               VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G };
    }
    if (mt < 0 || T.vkAllocateMemory(g_hvk.dev, &mai, NULL, &s->mem) != VK_SUCCESS ||
        T.vkBindImageMemory(g_hvk.dev, s->img, s->mem, 0) != VK_SUCCESS ||
        T.vkCreateImageView(g_hvk.dev, &vci, NULL, &s->view) != VK_SUCCESS) {
        hle_log("hakuX-hle-back", "target copy %ux%u fmt %d: no image", rt->w, rt->h,
                (int)rt->fmt);
        if (s->mem) {
            T.vkFreeMemory(g_hvk.dev, s->mem, NULL);
        }
        if (s->img) {
            T.vkDestroyImage(g_hvk.dev, s->img, NULL);
        }
        g_free_slot[g_nfree_slot++] = slot;
        return -1;
    }
    write_image(slot, s->view);
    hle_log("hakuX-hle-back", "target copy %u: %ux%u fmt %d (slot %u)", g_nsnap, rt->w, rt->h,
            (int)rt->fmt, slot);
    return (int)g_nsnap++;
}

void hle_draw_snap(VkCommandBuffer cb, HleVkSurf *rt)
{
    int i = snap_get(rt);
    g_snap_cur = i;
    g_snap_rt = rt->id;
    if (i < 0) {
        return;
    }
    typeof(g_snap[0]) *s = &g_snap[i];
    hle_surf_transition(cb, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    hle_img_barrier(cb, s->img, VK_IMAGE_ASPECT_COLOR_BIT, s->layout,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy r = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                      .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                      .extent = { s->w, s->h, 1 } };
    T.vkCmdCopyImage(cb, rt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->img,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    hle_img_barrier(cb, s->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    s->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void hle_draw_snap_end(void)
{
    g_snap_cur = -1;
}

/* A texture object that becomes a render-target surface: its texels now
 * live in the surface image, so the uploaded copy goes. */
void hle_draw_forget_tex(uint32_t id)
{
    HleVkTex *t = tex_find(id);
    if (t) {
        tex_free(t);
    }
}

/* ---- samplers ------------------------------------------------------------------ */

static VkSamplerAddressMode addr_mode(uint32_t a)
{
    switch (a) {
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;    /* D3DTADDRESS_CLAMP */
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;    /* CLAMPTOEDGE */
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

/* sampler_index's lod_down: how many levels finer the stage must sample (the
 * source surface's resolution scale over the target's, as log2 in 1/16ths). */
static uint32_t sampler_index(const uint32_t *tss, uint32_t lod_down)
{
    uint32_t base = MIN(tss[TSS_MAXMIPLEVEL], 15u);
    uint64_t key = (uint64_t)(tss[TSS_ADDRESSU] & 7) | (uint64_t)(tss[TSS_ADDRESSV] & 7) << 3 |
                   (uint64_t)(tss[TSS_MAGFILTER] & 7) << 6 |
                   (uint64_t)(tss[TSS_MINFILTER] & 7) << 9 |
                   (uint64_t)(tss[TSS_MIPFILTER] & 7) << 12 |
                   (uint64_t)(tss[TSS_ADDRESSW] & 7) << 15 | (uint64_t)base << 18 |
                   (uint64_t)tss[TSS_BORDERCOLOR] << 32;
    uint64_t key2 = tss[TSS_MIPMAPLODBIAS] | (uint64_t)lod_down << 32;
    /* one per stage a draw: the index last found for the key's hash, before the scan */
    static uint16_t hot[64];
    unsigned h = (unsigned)(((key ^ key2 * 0x9E3779B97F4A7C15ull) * 0xFF51AFD7ED558CCDull) >> 58);
    uint32_t i = hot[h];
    if (i < g_dr.nsmp && g_dr.smp[i].key == key && g_dr.smp[i].key2 == key2) {
        return i;
    }
    for (i = 0; i < g_dr.nsmp; i++) {
        if (g_dr.smp[i].key == key && g_dr.smp[i].key2 == key2) {
            hot[h] = (uint16_t)i;
            return i;
        }
    }
    if (g_dr.nsmp >= HLE_MAX_SAMPLERS) {
        return 0;
    }
    uint32_t bc = tss[TSS_BORDERCOLOR];
    VkSamplerCustomBorderColorCreateInfoEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
        .customBorderColor.float32 = { ((bc >> 16) & 0xFF) / 255.0f, ((bc >> 8) & 0xFF) / 255.0f,
                                       (bc & 0xFF) / 255.0f, (bc >> 24) / 255.0f },
        .format = VK_FORMAT_UNDEFINED };
    bool lin_mag = (tss[TSS_MAGFILTER] & 7) >= 2, lin_min = (tss[TSS_MINFILTER] & 7) >= 2;
    uint32_t mip = tss[TSS_MIPFILTER] & 7;
    /* MAXMIPLEVEL is the most detailed level used (the base level: mip
     * chains are built by sampling level n-1 into level n with mipmapping
     * off); the LOD bias is a float. */
    float bias;
    uint32_t bias_bits = (uint32_t)key2;
    memcpy(&bias, &bias_bits, 4);
    bias = isfinite(bias) ? MAX(MIN(bias, 15.0f), -15.0f) : 0.0f;
    bias -= lod_down / 16.0f;
    const float lim = g_hvk.caps.max_lod_bias;          /* 15 on PowerVR */
    bias = MAX(MIN(bias, lim), -lim);
    /* Without format-less custom border colours for every sampler (PowerVR's
     * B-Series), or any (Mali's r26): the nearest of Vulkan's three */
    VkBorderColor border = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    if (!g_hvk.caps.cbc_any) {
        const float *f = cbc.customBorderColor.float32;
        border = f[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK :
                 f[0] + f[1] + f[2] < 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK :
                                             VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = g_hvk.caps.cbc_any ? &cbc : NULL,
        .magFilter = lin_mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = lin_min ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mip >= 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = addr_mode(tss[TSS_ADDRESSU]),
        .addressModeV = addr_mode(tss[TSS_ADDRESSV]),
        .addressModeW = addr_mode(tss[TSS_ADDRESSW]),
        .mipLodBias = bias, .minLod = (float)base,
        .maxLod = mip ? VK_LOD_CLAMP_NONE : base + 0.25f,
        .borderColor = border };
    VkSampler s;
    if (T.vkCreateSampler(g_hvk.dev, &sci, NULL, &s) != VK_SUCCESS) {
        return 0;
    }
    i = g_dr.nsmp++;
    hot[h] = (uint16_t)i;
    g_dr.smp[i].key = key;
    g_dr.smp[i].key2 = key2;
    g_dr.smp[i].s = s;
    if (g_hvk.caps.bindless) {          /* else the draws' sets take it from smp[] */
        VkDescriptorImageInfo ii = { .sampler = s };
        VkWriteDescriptorSet w = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_dr.set,
            .dstBinding = 1, .dstArrayElement = i, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &ii };
        T.vkUpdateDescriptorSets(g_hvk.dev, 1, &w, 0, NULL);
    }
    return i;
}

/* ---- pipelines ----------------------------------------------------------------- */

static VkBlendFactor blend_factor(uint32_t f)
{
    switch (f) {
    case 0:      return VK_BLEND_FACTOR_ZERO;
    case 1:      return VK_BLEND_FACTOR_ONE;
    case 0x300:  return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302:  return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304:  return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306:  return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307:  return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308:  return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t o)
{
    switch (o) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default:     return VK_BLEND_OP_ADD;
    }
}

/* src's shader module; cache_only: from the SPIR-V disk cache only (NULL
 * with *compile when glslang would compile it). Any thread. */
static VkShaderModule module(HleShaderStage stage, const char *src, bool cache_only,
                             bool *compile)
{
    size_t nw = 0;
    uint32_t *spv = cache_only ? hle_spirv_peek(stage, src, &nw) : hle_spirv_get(stage, src, &nw);
    if (!spv && cache_only) {
        *compile = true;
        return VK_NULL_HANDLE;
    }
    if (!spv) {
        hle_log("hakuX-hle-pipe", "compile failed:\n%s", src);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = nw * 4, .pCode = spv };
    VkShaderModule m = VK_NULL_HANDLE;
    T.vkCreateShaderModule(g_hvk.dev, &mci, NULL, &m);
    g_free(spv);
    return m;
}

/* ---- vertex programs -------------------------------------------------------------- */

void hle_draw_cmd_vsh(const HleCmdVsh *c)
{
    for (uint32_t i = 0; i < g_dr.nprog; i++) {
        if (g_dr.prog[i].hash == c->hash) {
            return;
        }
    }
    if (g_dr.nprog >= HLE_MAX_PROGS || c->nslots > HLE_VSH_MAX_SLOTS) {
        hle_log("hakuX-hle-pipe", "vertex program %016llx dropped (%u programs, %u slots)",
                (unsigned long long)c->hash, g_dr.nprog, c->nslots);
        return;
    }
    g_dr.prog[g_dr.nprog].hash = c->hash;
    g_dr.prog[g_dr.nprog].nslots = c->nslots;
    g_dr.prog[g_dr.nprog++].tok = g_memdup2(c + 1, 16 * c->nslots);
}

static const uint32_t *prog_find(uint64_t hash, uint32_t *nslots)
{
    for (uint32_t i = 0; i < g_dr.nprog; i++) {
        if (g_dr.prog[i].hash == hash) {
            *nslots = g_dr.prog[i].nslots;
            return g_dr.prog[i].tok;
        }
    }
    return NULL;
}

/* Without VK_EXT_vertex_input_dynamic_state (caps.vids off: Mali before
 * r49, PowerVR's BXM): the draw's vertex layout, built into its pipeline;
 * the strides stay dynamic (VERTEX_INPUT_BINDING_STRIDE) */
typedef struct ViKey {
    _Alignas(8) uint8_t nbind, nattr;
    uint8_t loc[HLE_DRAW_ATTRS], bind[HLE_DRAW_ATTRS];
    uint16_t off[HLE_DRAW_ATTRS];
    uint32_t fmt[HLE_DRAW_ATTRS];
} ViKey;

/* Without extended dynamic state (caps.eds1 / eds2 off: Vulkan 1.1 drivers):
 * the state the draws record elsewhere, built into their pipelines; a field
 * whose state is dynamic stays 0 */
typedef struct DsKey {
    /* eds1: */
    _Alignas(8) uint8_t topo, cull, ff, zt, zw, zop, stn;
    uint8_t sop[4];                     /* fail, pass, depth fail, compare */
    uint16_t stride[HLE_DRAW_MAX_BIND]; /* caps.vids off too */
    /* eds2: */
    uint8_t bias;
} DsKey;

typedef struct PipeKey {
    HleFfVsKey vs;
    HleFfPsKey ps;
    uint32_t blend_en, src, dst, op, wmask;
    uint32_t cfmt, dfmt, topo_class;
    uint64_t vsh;                       /* vertex program hash, 0 = FF vertex shader */
    HleVshKey vk;
    uint32_t psh;                       /* 1 = register combiners (pk), else FF stages */
    HlePshKey pk;
    /* Last, 8-aligned: on a device with dynamic vertex input the key ends
     * before it (g_key_bytes), its bytes, hash and record as they were */
    ViKey vi;
    /* After it, 8-aligned: there only without extended dynamic state */
    DsKey ds;
} PipeKey;

/* The key's bytes hashed, compared and recorded: up to vi, up to ds (caps.vids
 * off), or all of it (caps.eds1 or eds2 off) */
static size_t g_key_bytes = offsetof(PipeKey, vi);

/* Pipeline key hash, a word at a time (the key is ~450 bytes, hashed on every
 * state change). */
static uint64_t key_hash(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) {
        h = (h ^ b[i]) * 0x100000001b3ull;
    }
    return h;
}

/* Open-addressed index of g_dr.pipes by hash (entries: index + 1). */
#define PIPE_IDX 32768                  /* > HLE_MAX_PIPES, a power of two */
static uint16_t g_pipe_idx[PIPE_IDX];

static int pipe_lookup(uint64_t h)
{
    for (unsigned i = h & (PIPE_IDX - 1), n = 0; n < PIPE_IDX; i = (i + 1) & (PIPE_IDX - 1), n++) {
        unsigned e = g_pipe_idx[i];
        if (!e) {
            return -1;
        }
        if (g_dr.pipes[e - 1].hash == h) {
            return e - 1;
        }
    }
    return -1;
}

static void pipe_add(uint64_t h, VkPipeline p, uint8_t state)
{
    unsigned i = h & (PIPE_IDX - 1);
    while (g_pipe_idx[i]) {
        i = (i + 1) & (PIPE_IDX - 1);
    }
    g_dr.pipes[g_dr.npipes].hash = h;
    g_dr.pipes[g_dr.npipes].pipe = p;
    g_dr.pipes[g_dr.npipes].state = state;
    g_pipe_idx[i] = (uint16_t)++g_dr.npipes;
}

static HlePbResult pipeline_build(const void *key, const uint32_t *tok, uint32_t nslots,
                                  bool cache_only, VkPipeline *out);

/* The workers' finished builds into their pending entries */
static void pipe_collect(void)
{
    uint64_t h;
    VkPipeline p;
    HlePbResult r;
    while (hle_pipe_take(&h, &p, &r)) {
        int i = pipe_lookup(h);
        if (i < 0 || g_dr.pipes[i].state != PIPE_PENDING) {
            if (p) {
                T.vkDestroyPipeline(g_hvk.dev, p, NULL);
            }
            continue;
        }
        g_dr.pipes[i].pipe = r == HLE_PB_OK ? p : VK_NULL_HANDLE;
        g_dr.pipes[i].state = PIPE_READY;
        g_dr.compiles++;
        g_dr.async_built++;
        if (r != HLE_PB_OK) {
            g_dr.pipe_fail++;
        }
    }
}

/* Asynchronous builds skip a draw only on a target drawn in the last
 * SKIP_FRAMES frames: a per-frame pass (the frame buffers, which alternate;
 * shadow and reflection passes) draws it again soon. On one drawn first, or
 * once (a texture baked at a level's load, Conker's lighting maps), a skip
 * would stay: its pipeline is built in place. */
#define SKIP_FRAMES 3

static void target_note(HleVkSurf *s)
{
    uint64_t f = hle_back_swap_count() + 1;     /* 0: never drawn */
    if (s && s->drawn_cur != f) {
        s->drawn_prev = s->drawn_cur;
        s->drawn_cur = f;
    }
}

static bool target_redrawn(const HleVkSurf *s)
{
    uint64_t f = hle_back_swap_count() + 1;
    return s && s->drawn_prev && f - s->drawn_prev <= SKIP_FRAMES;
}

/* A new key's pipeline: built here; with asynchronous builds on, from the
 * caches at once, or else by the workers while the draw is skipped
 * (*pending) when its target is redrawn. */
static VkPipeline pipeline_new(const PipeKey *k, uint64_t h, const HleVkSurf *target,
                               bool *pending)
{
    if (g_dr.npipes >= HLE_MAX_PIPES) {
        static bool s_full;
        if (!s_full) {
            s_full = true;
            hle_log("hakuX-hle-pipe", "pipeline table full (%u): new pipelines' draws are "
                    "dropped", HLE_MAX_PIPES);
        }
        return VK_NULL_HANDLE;
    }
    uint32_t nslots = 0;
    const uint32_t *tok = k->vsh ? prog_find(k->vsh, &nslots) : NULL;
    VkPipeline p = VK_NULL_HANDLE;
    HlePbResult r = HLE_PB_COMPILE;
    if (hle_pipe_async() && !hle_back_capture_armed()) {
        r = pipeline_build(k, tok, nslots, true, &p);       /* the caches only */
        if (r == HLE_PB_COMPILE) {
            if (target_redrawn(target) && hle_pipe_queue(h, k, k->vsh, tok, nslots)) {
                pipe_add(h, VK_NULL_HANDLE, PIPE_PENDING);
                g_dr.async_queued++;
                *pending = true;
                return VK_NULL_HANDLE;
            }
        } else if (r == HLE_PB_OK) {
            g_dr.cache_hits++;
        }
    }
    if (r == HLE_PB_COMPILE) {
        g_dr.inplace++;
        uint32_t pa = hle_hitch_enter(HLE_ACT_COMPILE);
        r = pipeline_build(k, tok, nslots, false, &p);
        hle_hitch_leave(pa);
    }
    g_dr.compiles++;
    if (r == HLE_PB_OK) {
        hle_pipe_record(h, k, k->vsh, tok, nslots);
    } else {
        g_dr.pipe_fail++;
        p = VK_NULL_HANDLE;
    }
    pipe_add(h, p, PIPE_READY);             /* failures too: not tried every draw */
    return p;
}

/* The draw's pipeline: the previous draw's when the key is unchanged, else by
 * hash, else made (and remembered, failures included); VK_NULL_HANDLE with
 * *pending while the workers build it. */
static VkPipeline pipeline(const PipeKey *k, HleVkSurf *target, bool *pending)
{
    static PipeKey last;
    static VkPipeline last_p;
    static bool last_ok;
    *pending = false;
    if (hle_pipe_async()) {
        pipe_collect();
        target_note(target);
    }
    if (last_ok && memcmp(k, &last, g_key_bytes) == 0) {
        return last_p;
    }
    uint64_t h = key_hash(k, g_key_bytes);
    int i = pipe_lookup(h);
    VkPipeline p;
    if (i >= 0) {
        HlePipe *e = &g_dr.pipes[i];
        if (e->state == PIPE_PENDING) {
            *pending = true;
            last_ok = false;
            return VK_NULL_HANDLE;
        }
        if (e->state == PIPE_PREBUILT) {
            e->state = PIPE_READY;
            hle_pipe_touch(h);
            g_dr.prebuilt_used++;
        }
        p = e->pipe;
    } else {
        p = pipeline_new(k, h, target, pending);
        if (*pending) {
            last_ok = false;
            return VK_NULL_HANDLE;
        }
    }
    last = *k;
    last_p = p;
    last_ok = i >= 0 || g_dr.npipes >= HLE_MAX_PIPES || pipe_lookup(h) >= 0;
    return p;
}

/* d stays dynamic: extended dynamic state's only with it (else k->ds) */
static bool dyn_kept(VkDynamicState d)
{
    switch (d) {
    case VK_DYNAMIC_STATE_CULL_MODE:
    case VK_DYNAMIC_STATE_FRONT_FACE:
    case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY:
    case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE:
    case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE:
    case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP:
    case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE:
    case VK_DYNAMIC_STATE_STENCIL_OP:
    case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE:
        return g_hvk.caps.eds1;
    case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE:
    case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE:
        return g_hvk.caps.eds2;
    default:
        return true;
    }
}

/* key's pipeline: GLSL from the key (and the vertex program's tokens),
 * SPIR-V, the Vulkan pipeline. No render-thread state: the workers and the
 * prebuild run it too. cache_only: SPIR-V from the disk cache and the
 * pipeline from the driver's cache, HLE_PB_COMPILE when either would compile. */
static HlePbResult pipeline_build(const void *key, const uint32_t *tok, uint32_t nslots,
                                  bool cache_only, VkPipeline *out)
{
    const PipeKey *k = key;
    *out = VK_NULL_HANDLE;
    if (cache_only && !g_hvk.caps.cache_control) {
        return HLE_PB_COMPILE;
    }
    enum { SRC_MAX = 65536 };
    g_autofree char *vsrc = g_malloc(SRC_MAX), *fsrc = g_malloc(SRC_MAX);
    /* a program's tokens, or push-buffer mode's fixed-function key (ffhw.h) */
    if (k->vsh ? !tok || !(hle_ffhw_is(tok, nslots) ? hle_ffhw_glsl(tok, nslots, &k->vk, vsrc, SRC_MAX)
                                                    : hle_vsh_glsl(tok, nslots, &k->vk, vsrc, SRC_MAX))
               : !hle_ff_vs_glsl(&k->vs, vsrc, SRC_MAX)) {
        hle_log("hakuX-hle-pipe", "vertex shader %016llx: %s", (unsigned long long)k->vsh,
                tok || !k->vsh ? "translation failed" : "program unknown");
        return HLE_PB_FAILED;
    }
    unsigned unsup = 0;
    if (k->psh ? !hle_psh_glsl(&k->pk, fsrc, SRC_MAX, &unsup)
               : !hle_ff_ps_glsl(&k->ps, fsrc, SRC_MAX)) {
        hle_log("hakuX-hle-pipe", "pixel shader (%s, fvf=0x%x vsh=%016llx): translation failed",
                k->psh ? "combiners" : "stages", k->vs.fvf, (unsigned long long)k->vsh);
        return HLE_PB_FAILED;
    }
    if (g_hle_glsl_fixed && !hle_glsl_fixed_slots(fsrc, SRC_MAX)) {
        hle_log("hakuX-hle-pipe", "pixel shader: too long for the draw set's places");
        return HLE_PB_FAILED;
    }
    if (unsup && !cache_only) {
        hle_log("hakuX-hle-pipe", "pixel shader: texture modes %x sampled as PROJECT2D "
                "(modes 0x%05x)", unsup, k->pk.tex_modes);
    }
    if (pslog_on() && k->psh && !cache_only) {  /* HAKUX_HLE_PSLOG=1: combiner programs */
        const HlePshKey *q = &k->pk;
        hle_log("hakuX-hle-pipe", "psh count=%x tex=%05x in=%x dot=%x cmp=%x final=%08x/%08x "
                "fog=%u atest=%u/%u", q->count, q->tex_modes, q->input_tex, q->dot_map,
                q->compare, q->final_abcd, q->final_efg, q->fog, q->alpha_test, q->alpha_func);
        for (unsigned s = 0; s < (q->count & 0xFF) && s < 8; s++) {
            hle_log("hakuX-hle-pipe", "  stage %u rgb %08x -> %05x  alpha %08x -> %05x", s,
                    q->rgb_in[s], q->rgb_out[s], q->alpha_in[s], q->alpha_out[s]);
        }
    }
    int64_t t0 = g_get_monotonic_time();
    bool compile = false;
    VkShaderModule vm = module(HLE_SHADER_VERTEX, vsrc, cache_only, &compile);
    VkShaderModule fm = vm ? module(HLE_SHADER_FRAGMENT, fsrc, cache_only, &compile)
                           : VK_NULL_HANDLE;
    int64_t t1 = g_get_monotonic_time();
    VkPipeline p = VK_NULL_HANDLE;
    VkResult vr = VK_SUCCESS;
    if (vm && fm) {
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vm, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fm, .pName = "main" },
        };
        static const VkPrimitiveTopology k_class[3] = {
            VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkPipelineInputAssemblyStateCreateInfo ia = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = k_class[k->topo_class] };
        VkPipelineViewportStateCreateInfo vp = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            /* without depthClamp the vertex shader clamps (HLE_ZCLAMP_GLSL) */
            .depthClampEnable = (k->vsh ? k->vk.zclamp : k->vs.zclamp) && g_hvk.caps.depth_clamp,
            .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1.0f };
        VkPipelineMultisampleStateCreateInfo ms = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineDepthStencilStateCreateInfo ds = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            /* all dynamic, but Turnip keys on the static masks: with zero
             * write masks DOA3's colour-masked stencil pass (planar shadows:
             * INVERT, then EQUAL) left the stencil unchanged */
            .front = { VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                       VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, 0 },
            .back = { VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, 0 } };
        VkPipelineColorBlendAttachmentState ba = {
            .blendEnable = k->blend_en,
            .srcColorBlendFactor = blend_factor(k->src), .dstColorBlendFactor = blend_factor(k->dst),
            .colorBlendOp = blend_op(k->op),
            .srcAlphaBlendFactor = blend_factor(k->src), .dstAlphaBlendFactor = blend_factor(k->dst),
            .alphaBlendOp = blend_op(k->op), .colorWriteMask = k->wmask };
        VkPipelineColorBlendStateCreateInfo cb = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = k->cfmt ? 1 : 0, .pAttachments = &ba };
        VkDynamicState dyn[] = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
            VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
            VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
            VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT,
            VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
            VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,  /* strip batches (batch.c) */
        };
        VkPipelineDynamicStateCreateInfo dy = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = ARRAY_SIZE(dyn), .pDynamicStates = dyn };
        /* no dynamic vertex input: the key's layout, the strides dynamic
         * (k->vi is only there then: the key ends before it, g_key_bytes) */
        VkVertexInputBindingDescription vb[HLE_DRAW_MAX_BIND];
        VkVertexInputAttributeDescription va[HLE_DRAW_ATTRS];
        VkPipelineVertexInputStateCreateInfo vis = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pVertexBindingDescriptions = vb, .pVertexAttributeDescriptions = va };
        if (!g_hvk.caps.vids) {
            vis.vertexBindingDescriptionCount = MIN(k->vi.nbind, (uint32_t)HLE_DRAW_MAX_BIND);
            vis.vertexAttributeDescriptionCount = MIN(k->vi.nattr, (uint32_t)HLE_DRAW_ATTRS);
            for (uint32_t i = 0; i < vis.vertexBindingDescriptionCount; i++) {
                vb[i] = (VkVertexInputBindingDescription){ i, 0, VK_VERTEX_INPUT_RATE_VERTEX };
            }
            for (uint32_t i = 0; i < vis.vertexAttributeDescriptionCount; i++) {
                va[i] = (VkVertexInputAttributeDescription){
                    k->vi.loc[i], k->vi.bind[i], (VkFormat)k->vi.fmt[i], k->vi.off[i] };
            }
            for (size_t i = 0; i < ARRAY_SIZE(dyn); i++) {
                if (dyn[i] == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT) {
                    dyn[i] = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
                }
            }
        }
        /* no extended dynamic state: the key's (k->ds, there only then) */
        if (!g_hvk.caps.eds1 || !g_hvk.caps.eds2) {
            uint32_t n = 0;
            for (size_t i = 0; i < ARRAY_SIZE(dyn); i++) {
                if (dyn_kept(dyn[i])) {
                    dyn[n++] = dyn[i];
                }
            }
            dy.dynamicStateCount = n;
        }
        if (!g_hvk.caps.eds1) {
            const DsKey *d = &k->ds;
            ia.topology = (VkPrimitiveTopology)d->topo;
            rs.cullMode = d->cull;
            rs.frontFace = (VkFrontFace)d->ff;
            ds.depthTestEnable = d->zt;
            ds.depthWriteEnable = d->zw;
            ds.depthCompareOp = (VkCompareOp)d->zop;
            ds.stencilTestEnable = d->stn;
            ds.front.failOp = ds.back.failOp = (VkStencilOp)d->sop[0];
            ds.front.passOp = ds.back.passOp = (VkStencilOp)d->sop[1];
            ds.front.depthFailOp = ds.back.depthFailOp = (VkStencilOp)d->sop[2];
            ds.front.compareOp = ds.back.compareOp = (VkCompareOp)d->sop[3];
            for (uint32_t i = 0; !g_hvk.caps.vids && i < vis.vertexBindingDescriptionCount; i++) {
                vb[i].stride = d->stride[i];
            }
        }
        if (!g_hvk.caps.eds2) {
            rs.depthBiasEnable = k->ds.bias;    /* primitive restart off: no strip batches */
        }
        VkFormat cf = (VkFormat)k->cfmt, df = (VkFormat)k->dfmt;
        bool stencil = df == VK_FORMAT_D24_UNORM_S8_UINT || df == VK_FORMAT_D32_SFLOAT_S8_UINT;
        VkPipelineRenderingCreateInfo ri = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = cf ? 1 : 0, .pColorAttachmentFormats = &cf,
            .depthAttachmentFormat = df,
            .stencilAttachmentFormat = stencil ? df : VK_FORMAT_UNDEFINED };
        VkGraphicsPipelineCreateInfo gci = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
            .stageCount = 2, .pStages = st, .pInputAssemblyState = &ia,
            .pVertexInputState = g_hvk.caps.vids ? NULL : &vis,
            .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms,
            .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy,
            .layout = g_dr.layout };
        if (cache_only) {
            gci.flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        }
        vr = T.vkCreateGraphicsPipelines(g_hvk.dev, hle_pcache_get(), 1, &gci, NULL, &p);
        if (vr != VK_SUCCESS) {
            p = VK_NULL_HANDLE;
        } else if (!cache_only) {
            hle_pcache_compiled();      /* after a cache-only try failed (but async off) */
        }
    }
    if (vm) {
        T.vkDestroyShaderModule(g_hvk.dev, vm, NULL);
    }
    if (fm) {
        T.vkDestroyShaderModule(g_hvk.dev, fm, NULL);
    }
    if (cache_only && (compile || vr == VK_PIPELINE_COMPILE_REQUIRED)) {
        return HLE_PB_COMPILE;          /* not logged: built now or by the workers */
    }
    hle_log("hakuX-hle-pipe", "pipeline %016llx fvf=0x%x vsh=%016llx in=%x %s=%u %s in %.1f ms "
            "(spirv %.1f)%s",
            (unsigned long long)key_hash(k, g_key_bytes), k->vs.fvf, (unsigned long long)k->vsh,
            k->vk.in_mask, k->psh ? "combiners" : "stages",
            k->psh ? k->pk.count & 0xFF : k->ps.nstages, p ? "ok" : "FAILED",
            (g_get_monotonic_time() - t0) / 1000.0, (t1 - t0) / 1000.0,
            cache_only ? " from the cache" : "");
    if (!p) {
        if (tok && !vm) {
            /* the program's tokens, for hle_vshgen (a broken translation) */
            hle_log("hakuX-hle-pipe", "vtok %016llx slots=%u", (unsigned long long)k->vsh,
                    nslots);
            for (uint32_t i = 0; i < nslots; i++) {
                hle_log("hakuX-hle-pipe", "  vtok %u: %08x %08x %08x %08x", i, tok[4 * i],
                        tok[4 * i + 1], tok[4 * i + 2], tok[4 * i + 3]);
            }
        }
        return HLE_PB_FAILED;
    }
    *out = p;
    return HLE_PB_OK;
}

static void pipe_async_init(void)
{
    /* the record's key size too, and its own record (a device's keys are
     * one kind, unless debug.hakux.caps changes it) */
    HLE_BUILD_BUG_ON(_Alignof(ViKey) != 8);
    HLE_BUILD_BUG_ON(_Alignof(DsKey) != 8);
    /* up to vi: the key as it was before vi (8-aligned for its uint64_t) */
    HLE_BUILD_BUG_ON(offsetof(PipeKey, vi) !=
                     ((offsetof(PipeKey, pk) + sizeof(HlePshKey) + 7) & ~(size_t)7));
    /* up to ds: the key as it was before ds */
    HLE_BUILD_BUG_ON(offsetof(PipeKey, ds) != offsetof(PipeKey, vi) + sizeof(ViKey));
    const HleCaps *c = &g_hvk.caps;
    g_key_bytes = !c->eds1 || !c->eds2 ? sizeof(PipeKey)
                  : c->vids ? offsetof(PipeKey, vi) : offsetof(PipeKey, ds);
    static char variant[16];
    snprintf(variant, sizeof(variant), "%s%s%s", c->vids ? "" : "-vi", c->eds1 ? "" : "-e1",
             c->eds2 ? "" : "-e2");
    hle_pipe_init(g_key_bytes, pipeline_build, variant);
}

/* A prebuilt pipeline into the table (the boot thread, before the render thread) */
static void prebuild_add(uint64_t h, VkPipeline p)
{
    if (pipe_lookup(h) >= 0 || g_dr.npipes >= HLE_MAX_PIPES) {
        T.vkDestroyPipeline(g_hvk.dev, p, NULL);
        return;
    }
    pipe_add(h, p, PIPE_PREBUILT);
    g_dr.prebuilt++;
}

void hle_draw_prebuild(void)
{
    const HleXbeInfo *x = &g_hle.title.xbe;
    if (!x->title_id) {
        return;
    }
    hle_pipe_title(x->title_id, x->timedate);
    /* half the table at most: the session's new pipelines keep the rest */
    hle_pipe_prebuild(HLE_MAX_PIPES / 2, prebuild_add);
}

/* ---- draw ---------------------------------------------------------------------- */

void hle_draw_cmd_state(const HleCmdState *s)
{
    g_dr.st = *s;
    g_dr.st_valid = true;
}

void hle_draw_cmd_state_part(const HleCmdStatePart *p)
{
    if (g_dr.st_valid && p->off < sizeof(g_dr.st) && p->len <= sizeof(g_dr.st) - p->off) {
        memcpy((uint8_t *)&g_dr.st + p->off, p + 1, p->len);
    }
}

static VkStencilOp stencil_op(uint32_t o)
{
    switch (o) {
    case 0:      return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;
    }
}

static VkCompareOp cmp_op(uint32_t f)
{
    return (f >= 0x200 && f <= 0x207) ? (VkCompareOp)(f - 0x200) : VK_COMPARE_OP_ALWAYS;
}

/* The next draw runs without the driver's LRZ (quirks.h HLE_QUIRK_LRZ_CLEAR):
 * no depth test, a test LRZ does not keep (EQUAL, ALWAYS, NEVER, NOT_EQUAL),
 * or a stencil write when the depth test fails. */
bool hle_draw_lrz_off(void)
{
    const uint32_t *rs = g_dr.st.rs;
    if (!g_dr.st_valid || !rs[RS_ZENABLE] ||
        (rs[RS_STENCILENABLE] && stencil_op(rs[RS_STENCILZFAIL]) != VK_STENCIL_OP_KEEP)) {
        return true;
    }
    switch (cmp_op(rs[RS_ZFUNC])) {
    case VK_COMPARE_OP_LESS: case VK_COMPARE_OP_LESS_OR_EQUAL:
    case VK_COMPARE_OP_GREATER: case VK_COMPARE_OP_GREATER_OR_EQUAL:
        return false;
    default:
        return true;
    }
}

/* Vertex attributes for an FVF on binding 0. */
static uint32_t fvf_attrs(uint32_t fvf, VkVertexInputAttributeDescription2EXT *a)
{
    uint32_t n = 0, off = 0, pos = fvf & HLE_FVF_POSITION_MASK;
#define ATTR(loc, f, sz) a[n++] = (VkVertexInputAttributeDescription2EXT){ \
        .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,   \
        .location = (loc), .binding = 0, .format = (f), .offset = off };        \
        off += (sz)
    if (pos == HLE_FVF_XYZRHW) {
        ATTR(0, VK_FORMAT_R32G32B32A32_SFLOAT, 16);
    } else if (pos) {
        ATTR(0, VK_FORMAT_R32G32B32_SFLOAT, 12);
        unsigned nw = pos >= 0x006 ? (pos - 0x004) / 2 : 0;    /* XYZB1..5: weights */
        if (nw) {
            /* the blend weights (the FF vertex shader's iWeight); a fifth
             * one is past what the NV2A blends */
            static const VkFormat k_wf[4] = {
                VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                VK_FORMAT_R32G32B32A32_SFLOAT };
            uint32_t woff = off;
            ATTR(8, k_wf[MIN(nw, 4u) - 1], 0);
            off = woff + nw * 4;
        }
    }
    if (fvf & HLE_FVF_NORMAL) {
        ATTR(1, VK_FORMAT_R32G32B32_SFLOAT, 12);
    }
    if (fvf & HLE_FVF_PSIZE) {
        ATTR(9, VK_FORMAT_R32_SFLOAT, 4);
    }
    if (fvf & HLE_FVF_DIFFUSE) {
        ATTR(2, VK_FORMAT_B8G8R8A8_UNORM, 4);
    }
    if (fvf & HLE_FVF_SPECULAR) {
        ATTR(3, VK_FORMAT_B8G8R8A8_UNORM, 4);
    }
    unsigned ntc = (fvf & HLE_FVF_TEXCOUNT_MASK) >> HLE_FVF_TEXCOUNT_SHIFT;
    static const VkFormat k_tf[5] = { 0, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                      VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
    for (unsigned i = 0; i < ntc && i < 4; i++) {
        unsigned sz = hle_fvf_tex_size(fvf, i);
        ATTR(4 + i, k_tf[sz], sz * 4);
    }
#undef ATTR
    return n;
}

/* A render-target surface sampled into a target of a lower resolution scale:
 * its texels are denser than the guest's by the ratio, so the GPU would pick
 * a level that much coarser than the NV2A does. The sampler takes it back
 * (log2 of the ratio, in 1/16ths; 0 otherwise). Fable's lock-on halo blurs a
 * silhouette from a 128 x 128 mipmapped target (scaled, its levels made from
 * an earlier pass) into a single-level 128 x 128 one (guest resolution): the
 * blur read level 1, still the earlier pass's noise, and the halo showed it. */
static uint32_t stage_lod_down(const HleCmdDraw *c, unsigned st, uint32_t rt_id)
{
    HleVkSurf *ts = tex_surface(c->tex[st], rt_id);
    HleVkSurf *rt = ts && ts->nlevels > 1 ? hle_surf_get(rt_id) : NULL;
    if (!rt || ts->scale <= rt->scale) {
        return 0;
    }
    return (uint32_t)lrintf(log2f((float)ts->scale / (float)rt->scale) * 16.0f);
}

/* Surfaces sampled as textures, each told once (the first 64 ids below 8192) */
static uint8_t g_rt_seen[8192 / 8];
static unsigned g_rt_told;

/* Image slot of the texture bound to stage st (0 = the white texture);
 * *flags gets the stage's linear (texel coordinate) bit. A target's image
 * is addressed as the stage's format says, as the NV2A does, not as the
 * target is laid out: a linear and a swizzled target over the same memory
 * share one object (object_view). Fable draws a lock-on's silhouette into
 * a swizzled 256 x 256 target through its linear view and blurs it from
 * there with a linear texture, by texel; addressed normalized, as the
 * swizzled object, every tap clamped to the corner and the halo was gone. */
static uint32_t stage_slot(const HleCmdDraw *c, unsigned st, uint32_t rt_id, uint32_t *flags)
{
    HleVkTex *tx = tex_find(c->tex[st]);
    HleVkSurf *ts = tx ? NULL : tex_surface(c->tex[st], rt_id);
    uint32_t slot = 0;
    HleVkSurf *rt;
    if (tx) {
        slot = tx->slot;
        *flags |= tx->linear ? 1u << st : 0;
    } else if (c->tex[st] == rt_id && g_snap_cur >= 0 && g_snap_rt == rt_id &&
               (rt = hle_surf_get(rt_id)) != NULL) {
        slot = g_snap[g_snap_cur].slot;  /* the target, copied before the draw */
        *flags |= (c->tex_lin >> st & 1) ? (1u << st) | (256u << st) : 0;
        return slot;
    } else if (ts && (ts->general || ts->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) {
        slot = surf_slot(ts, c->tex_ord >> 2 * st & 3);  /* RT as texture (identity) */
        /* linear format: texel coords, of a scaled image */
        *flags |= (c->tex_lin >> st & 1) ? (1u << st) | (256u << st) : 0;
    }
    uint32_t id = c->tex[st];
    if (id && !tx && id < 8192 && g_rt_told < 64 && !(g_rt_seen[id >> 3] >> (id & 7) & 1)) {
        g_rt_seen[id >> 3] |= 1u << (id & 7);
        g_rt_told++;
        HleVkSurf *any = hle_surf_get(id);
        hle_log("hakuX-hle-drawlog", "rt-tex id=%u surf=%d img=%d layout=%d slot=%u "
                "fmt=%d %ux%u rt=%u", id, any != NULL,
                any && any->img, any ? (int)any->layout : -1, slot,
                any ? (int)any->fmt : -1, any ? any->d.w : 0, any ? any->d.h : 0,
                rt_id);
    }
    return slot;
}

/* The push block's fog.w for stage st when its linear texture (the draw's
 * tex_rect: per draw, as the state cache isn't rebuilt for a texture alone)
 * is a surface larger than the texture's own size on an axis (the top-left
 * part of a target): that size (an axis the surface matches at the surface's)
 * and the stage's address modes, its texel coordinates kept inside it (ff.h
 * HLE_NVRECT_GLSL); 0 when the rect covers the surface, isn't valid, or the
 * texture is no surface (a texture's image is its size). */
static uint32_t lin_rect(const HleCmdDraw *c, const HleCmdState *s, unsigned st)
{
    const HleVkSurf *sf = (c->tex_lin >> st & 1) && c->tex[st] && !tex_find(c->tex[st]) ?
                          hle_surf_get(c->tex[st]) : NULL;
    const uint32_t r = c->tex_rect[st];
    uint32_t w = (r >> 16) & 0x1FFF, h = r & 0x1FFF;
    if (!sf || !w || !h || !sf->d.w || !sf->d.h || (w >= sf->d.w && h >= sf->d.h)) {
        return 0;
    }
    w = MIN(w, sf->d.w);
    h = MIN(h, sf->d.h);
    if (w > 4096 || h > 4096) {
        return 0;
    }
    /* NV2A modes: 1 wrap, 2 mirror, 3 clamp to edge, 4 border, 5 clamp (as addr_mode) */
    static const uint8_t k_class[8] = { 2, 2, 3, 0, 1, 0, 2, 2 };
    return (w - 1) | (h - 1) << 12 | (uint32_t)k_class[s->tss[st][TSS_ADDRESSU] & 7] << 24 |
           (uint32_t)k_class[s->tss[st][TSS_ADDRESSV] & 7] << 26 | (st & 1u) << 28 | 1u << 29 |
           (st >> 1) << 31;
}

/* Vertex-buffer support of f, else of its four-component form (one more
 * element read past the attribute), else UNDEFINED. */
static VkFormat vfmt(VkFormat f, VkFormat wide)
{
    VkFormat c[2] = { f, wide };
    for (unsigned i = 0; i < 2; i++) {
        if (c[i] == VK_FORMAT_UNDEFINED || (uint32_t)c[i] >= 256) {
            continue;
        }
        int8_t *s = &g_dr.vfmt[c[i]];
        if (!*s) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(g_hvk.phys, c[i], &fp);
            *s = (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : -1;
        }
        if (*s > 0) {
            return c[i];
        }
    }
    return VK_FORMAT_UNDEFINED;
}

/* NV2A attribute format byte -> vertex input format. Programs fetch S32K as
 * integers (kind 1) and CMP as one packed dword (kind 2) and convert in the
 * shader; the FF generators take floats (SSCALED) and a CMP normal. */
static VkFormat vat_format(uint8_t fmt, bool prog, unsigned *kind)
{
#define F4(a, b, c, d) { VK_FORMAT_##a, VK_FORMAT_##b, VK_FORMAT_##c, VK_FORMAT_##d }
    static const VkFormat k_f[4] = F4(R32_SFLOAT, R32G32_SFLOAT, R32G32B32_SFLOAT,
                                      R32G32B32A32_SFLOAT);
    static const VkFormat k_ub[4] = F4(R8_UNORM, R8G8_UNORM, R8G8B8_UNORM, R8G8B8A8_UNORM);
    static const VkFormat k_s1[4] = F4(R16_SNORM, R16G16_SNORM, R16G16B16_SNORM,
                                       R16G16B16A16_SNORM);
    static const VkFormat k_si[4] = F4(R16_SINT, R16G16_SINT, R16G16B16_SINT, R16G16B16A16_SINT);
    static const VkFormat k_ss[4] = F4(R16_SSCALED, R16G16_SSCALED, R16G16B16_SSCALED,
                                       R16G16B16A16_SSCALED);
#undef F4
    unsigned n = hle_vat_count(fmt);
    *kind = 0;
    if (!n || n > 4) {
        return VK_FORMAT_UNDEFINED;
    }
    switch (hle_vat_type(fmt)) {
    case HLE_VAT_F:
        return vfmt(k_f[n - 1], VK_FORMAT_UNDEFINED);
    case HLE_VAT_UB_D3D:
        if (n == 4) {
            return vfmt(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_UNDEFINED);
        }
        /* fallthrough */
    case HLE_VAT_UB_OGL:
        return vfmt(k_ub[n - 1], k_ub[3]);
    case HLE_VAT_S1:
        return vfmt(k_s1[n - 1], k_s1[3]);
    case HLE_VAT_S32K:
        if (prog) {
            *kind = 1;
            return vfmt(k_si[n - 1], k_si[3]);
        }
        return vfmt(k_ss[n - 1], k_ss[3]);
    case HLE_VAT_CMP:                   /* 16-bit halves: often only 2-byte aligned */
        *kind = 2;
        return vfmt(VK_FORMAT_R16G16_UINT, VK_FORMAT_UNDEFINED);
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

/* Declaration layout of a draw (HleCmdDraw.nbind > 0): attributes by
 * location; in, ints, cmps = the program key masks. False when an
 * attribute the FF shader reads has no usable format. The last one is
 * kept: draws mostly declare what the one before did (Blinx changes its
 * vertex input about once in 850), and building it for each was a tenth
 * of the render thread's instructions. */
static struct {
    bool valid, prog, ok;
    uint32_t nbind, attr[HLE_DRAW_ATTRS];
    VkVertexInputAttributeDescription2EXT a[HLE_DRAW_ATTRS];
    uint32_t na, in, ints, cmps;
} g_decl;

static bool decl_build(const HleCmdDraw *c, bool prog, VkVertexInputAttributeDescription2EXT *a,
                       uint32_t *na, uint32_t *in, uint32_t *ints, uint32_t *cmps)
{
    *na = *in = *ints = *cmps = 0;
    for (uint32_t loc = 0; loc < HLE_DRAW_ATTRS; loc++) {
        uint32_t x = c->attr[loc];
        if (!hle_vat_count(x & 0xFF)) {
            continue;
        }
        unsigned kind;
        VkFormat f = vat_format(x & 0xFF, prog, &kind);
        if (!prog && kind == 2 && loc != 1) {
            f = VK_FORMAT_UNDEFINED;    /* FF decodes CMP for its normal only */
        }
        if (f == VK_FORMAT_UNDEFINED || ((x >> 8) & 0xFF) >= c->nbind) {
            if (!prog) {
                return false;
            }
            continue;                   /* the program reads the register default */
        }
        a[(*na)++] = (VkVertexInputAttributeDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
            .location = loc, .binding = (x >> 8) & 0xFF, .format = f, .offset = x >> 16 };
        *in |= 1u << loc;
        *ints |= (kind == 1 ? 1u : 0) << loc;
        *cmps |= (kind == 2 ? 1u : 0) << loc;
    }
    return true;
}

static bool decl_attrs(const HleCmdDraw *c, bool prog, VkVertexInputAttributeDescription2EXT *a,
                       uint32_t *na, uint32_t *in, uint32_t *ints, uint32_t *cmps)
{
    if (!g_decl.valid || g_decl.prog != prog || g_decl.nbind != c->nbind ||
        memcmp(g_decl.attr, c->attr, sizeof(g_decl.attr))) {
        g_decl.valid = true;
        g_decl.prog = prog;
        g_decl.nbind = c->nbind;
        memcpy(g_decl.attr, c->attr, sizeof(g_decl.attr));
        g_decl.ok = decl_build(c, prog, g_decl.a, &g_decl.na, &g_decl.in, &g_decl.ints,
                               &g_decl.cmps);
    }
    memcpy(a, g_decl.a, g_decl.na * sizeof(*a));
    *na = g_decl.na;
    *in = g_decl.in;
    *ints = g_decl.ints;
    *cmps = g_decl.cmps;
    return g_decl.ok;
}

static uint32_t imm_attrs(uint32_t fvf, VkVertexInputAttributeDescription2EXT *a);

/* caps.vids off: the vertex layout the draw records (hle_draw_cmd_draw: the
 * program's declaration, else the FVF's, with the current diffuse on binding
 * 1), into its pipeline key */
static void vi_key(ViKey *v, const HleCmdDraw *c, uint32_t key_fvf, bool cur_diffuse,
                   const VkVertexInputAttributeDescription2EXT *decl, uint32_t ndecl)
{
    VkVertexInputAttributeDescription2EXT a[HLE_DRAW_ATTRS + 1];
    uint32_t na = ndecl, nb = 1;
    if (c->nbind) {
        nb = MIN(c->nbind, (uint32_t)HLE_DRAW_MAX_BIND);
        memcpy(a, decl, ndecl * sizeof(a[0]));
    } else {
        na = c->imm ? imm_attrs(key_fvf, a) : fvf_attrs(c->fvf, a);
        if (cur_diffuse) {
            a[na++] = (VkVertexInputAttributeDescription2EXT){
                .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                .location = 2, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT };
            nb = 2;
        }
    }
    na = MIN(na, (uint32_t)HLE_DRAW_ATTRS);
    v->nbind = (uint8_t)nb;
    v->nattr = (uint8_t)na;
    for (uint32_t i = 0; i < na; i++) {
        v->loc[i] = (uint8_t)a[i].location;
        v->bind[i] = (uint8_t)a[i].binding;
        v->off[i] = (uint16_t)a[i].offset;
        v->fmt[i] = (uint32_t)a[i].format;
    }
}

/* Immediate-mode vertices: 8 float4 registers at locations 0..7; only the
 * ones the FVF uses are bound. */
static uint32_t imm_attrs(uint32_t fvf, VkVertexInputAttributeDescription2EXT *a)
{
    uint32_t n = 0;
    bool use[8] = { true, (fvf & HLE_FVF_NORMAL) != 0, (fvf & HLE_FVF_DIFFUSE) != 0,
                    (fvf & HLE_FVF_SPECULAR) != 0 };
    unsigned ntc = (fvf & HLE_FVF_TEXCOUNT_MASK) >> HLE_FVF_TEXCOUNT_SHIFT;
    for (unsigned i = 0; i < ntc && i < 4; i++) {
        use[4 + i] = true;
    }
    for (uint32_t l = 0; l < 8; l++) {
        if (use[l]) {
            a[n++] = (VkVertexInputAttributeDescription2EXT){
                .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                .location = l, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                .offset = 16 * l };
        }
    }
    return n;
}

/* The FF vertex key's uniform-block features: lighting (the front end sent
 * the material and lights), texgen and texture transforms of the stages
 * with coordinates; vertex blending later. */
static void ff_vs_key(HleFfVsKey *v, const HleCmdState *s, const HleCmdDraw *c)
{
    const uint32_t *rs = s->rs;
    bool tg = false;
    /* the key's stages start at texture stage 3 under point sprites (ff.h) */
    unsigned first = rs[RS_POINTSPRITEENABLE] && !s->psh ? HLE_FF_SPRITE_STAGE : 0;
    for (unsigned st = 0; st < v->ntex && st + first < HLE_FF_STAGES; st++) {
        uint32_t tci = s->tss[st + first][TSS_TEXCOORDINDEX] >> 16;
        uint32_t ttf = s->tss[st + first][TSS_TEXTURETRANSFORMFLAGS];
        v->texgen[st] = tci <= HLE_TCI_SPHEREMAP ? tci : 0;
        v->texxform[st] = (ttf & 7) | (ttf & 0x100 ? 0x10 : 0);
        tg |= v->texgen[st] != 0;
    }
    if (rs[RS_LIGHTING] && (c->ff_lights & HLE_DRAW_LIT_DATA)) {
        v->lighting = 1;
        v->nlights = MIN(c->ff_lights & 0xF, HLE_FF_LIGHTS);
        for (unsigned i = 0; i < v->nlights; i++) {
            v->light_type[i] = (c->ff_lights >> (4 + 2 * i)) & 3;
        }
        v->specular = rs[RS_SPECULARENABLE] != 0;
        v->local_viewer = rs[RS_LOCALVIEWER] != 0;
        v->colorvertex = rs[RS_COLORVERTEX] != 0;
        v->src[0] = rs[RS_DIFFUSEMATERIALSOURCE] & 3;
        v->src[1] = rs[RS_AMBIENTMATERIALSOURCE] & 3;
        v->src[2] = rs[RS_SPECULARMATERIALSOURCE] & 3;
        v->src[3] = rs[RS_EMISSIVEMATERIALSOURCE] & 3;
    }
    /* vertex blending: D3DRS_VERTEXBLEND is the NV2A skin mode. 1 / 3 / 5
     * blend 2 / 3 / 4 matrices with the last weight 1 - the vertex's; 2 / 4 /
     * 6 take all 2 / 3 / 4 weights from the vertex. Only for positions that
     * carry weights (XYZB1..XYZB4) */
    uint32_t vb = rs[RS_VERTEXBLEND], pos = v->fvf & HLE_FVF_POSITION_MASK;
    if (vb >= 1 && vb <= 6 && pos >= 0x006 && pos <= 0x00C) {
        v->blend = (uint8_t)((vb + 1) / 2 + 1);
        v->blend_mix = vb & 1;
    }
    if (v->lighting || tg || v->blend) {
        v->normalize = rs[RS_NORMALIZENORMALS] != 0;
    }
}

/* Material dump: every distinct material (combiner program, stage textures,
 * constants) of one frame, with textures and samplers. HAKUX_HLE_PSFRAME=
 * <swap> picks the frame; on demand, a new value of the Android property
 * debug.hakux.psframe dumps the next frame (screenshot first, then set it),
 * debug.hakux.psmaxn=<n> keeps draws of at most n indices/vertices. */
static int64_t g_psf_frame = -2;
static uint32_t g_psf_maxn;
static uint64_t g_psf_seen[1024];
static unsigned g_psf_nseen;
/* debug (debug.hakux.norec, live): 1 records every draw's whole state (no
 * cache of the recorded state), 2 keeps the vertex offset in the binding
 * (firstVertex 0), 3 both */
static unsigned g_norec;

/* debug (debug.hakux.probe=<program hash>[,<hash>...], hex, up to 4, live;
 * 0 = off): an occlusion query around each draw of those programs, so a
 * frame's samples per draw can be compared without changing how it renders
 * (no barrier or pass split). Logged (hakuX-hle-probe) 3 swaps later: per
 * draw its samples, a letter for its viewport registers c58/c59 (the
 * frame's distinct pairs: a, b, ...), its program's index in the list, '*'
 * when recorded into a later submission than the frame's first draw, then
 * ':' and 16-bit hashes of its indices, the vertices they fetch, its
 * constant file, its pipeline and its recorded state (lines stop near 1 KiB:
 * probe few programs). debug.hakux.probez=<VkCompareOp + 1> overrides the
 * probed draws' depth test; debug.hakux.probefocus=<n> logs draw n of each
 * program with its raw transform rows (c0..c3, c58, c59). */
#define PROBE_FRAMES 4
#define PROBE_N      128
#define PROBE_PROGS  4
static struct {
    uint64_t vsh[PROBE_PROGS];
    unsigned nvsh;
    uint32_t zop;           /* debug.hakux.probez: VkCompareOp + 1 for probed draws, 0 = theirs */
    int focus;              /* debug.hakux.probefocus: draw logged in full, -1 none */
    VkQueryPool pool;
    bool failed;
    struct {
        uint64_t swap;
        uint32_t n;
        struct {
            uint32_t ib_off, count, vfirst, consts, tex0, bind0, khash, ihash, vhash;
            uint32_t pipe, state;
            uint32_t xf[24];        /* c0..c3 and c58/c59 as recorded (focus log) */
            uint8_t prog;
            float vp[8];
            uint64_t submit;
        } d[PROBE_N];
    } fr[PROBE_FRAMES];
} g_probe;

static uint32_t probe_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static void probe_log(uint64_t swap)
{
    if (!g_probe.pool || swap < 3) {
        return;
    }
    typeof(g_probe.fr[0]) *f = &g_probe.fr[(swap - 3) % PROBE_FRAMES];
    if (f->swap != swap - 3 || !f->n) {
        return;
    }
    uint32_t base = (uint32_t)((swap - 3) % PROBE_FRAMES) * PROBE_N;
    uint64_t r[PROBE_N];
    VkResult vr = T.vkGetQueryPoolResults(g_hvk.dev, g_probe.pool, base, f->n,
                                          sizeof(r), r, sizeof(r[0]), VK_QUERY_RESULT_64_BIT);
    char line[4000];
    int m = snprintf(line, sizeof(line), "swap %llu n=%u%s sub0=%llu:",
                     (unsigned long long)f->swap, f->n, vr == VK_SUCCESS ? "" : " NOT READY",
                     (unsigned long long)f->d[0].submit);
    float vps[8][8];
    unsigned nvp = 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < f->n && m < (int)sizeof(line) - 300; i++) {
        unsigned j = 0;
        while (j < nvp && memcmp(vps[j], f->d[i].vp, sizeof(vps[0]))) {
            j++;
        }
        if (j == nvp && nvp < 8) {
            memcpy(vps[nvp++], f->d[i].vp, sizeof(vps[0]));
        }
        total += r[i];
        m += snprintf(line + m, sizeof(line) - m, " %llu%c%u%s:%04x%04x%04x%04x%04x",
                      (unsigned long long)(vr == VK_SUCCESS ? r[i] : 0),
                      j < 8 ? 'a' + (char)j : '?', f->d[i].prog,
                      f->d[i].submit != f->d[0].submit ? "*" : "",
                      f->d[i].ihash & 0xFFFF, f->d[i].vhash & 0xFFFF, f->d[i].khash & 0xFFFF,
                      f->d[i].pipe & 0xFFFF, f->d[i].state & 0xFFFF);
    }
    m += snprintf(line + m, sizeof(line) - m, " total=%llu vp:", (unsigned long long)total);
    for (unsigned j = 0; j < nvp && m < (int)sizeof(line) - 160; j++) {
        m += snprintf(line + m, sizeof(line) - m, " %c=%.9g,%.9g,%.9g,%.9g/%.9g,%.9g,%.9g,%.9g",
                      'a' + (char)j, vps[j][0], vps[j][1], vps[j][2], vps[j][3], vps[j][4],
                      vps[j][5], vps[j][6], vps[j][7]);
    }
    hle_log("hakuX-hle-probe", "%s", line);
    if (g_probe.focus >= 0) {
        /* debug.hakux.probefocus=<n>: draw n of each probed program, its
         * samples and transform rows c0..c3, c58, c59 (raw bits) */
        uint32_t seen[PROBE_PROGS] = { 0 };
        for (uint32_t i = 0; i < f->n; i++) {
            unsigned p = f->d[i].prog;
            if (seen[p]++ != (uint32_t)g_probe.focus) {
                continue;
            }
            m = snprintf(line, sizeof(line), "focus swap %llu prog %u draw %d samples %llu "
                         "sub %llu xf", (unsigned long long)f->swap, p, g_probe.focus,
                         (unsigned long long)(vr == VK_SUCCESS ? r[i] : 0),
                         (unsigned long long)f->d[i].submit);
            for (unsigned w = 0; w < 24; w++) {
                m += snprintf(line + m, sizeof(line) - m, "%s%08x", w % 4 ? "" : " ",
                              f->d[i].xf[w]);
            }
            hle_log("hakuX-hle-probe", "%s", line);
        }
    }
    /* the inputs, once in a while (they repeat while the scene does) */
    if (swap % 64 == 0) {
        m = snprintf(line, sizeof(line), "swap %llu inputs (ib/count/vfirst/consts/tex/bind):",
                     (unsigned long long)f->swap);
        for (uint32_t i = 0; i < f->n && m < (int)sizeof(line) - 64; i++) {
            m += snprintf(line + m, sizeof(line) - m, " %x/%u/%u/%x/%u/%x", f->d[i].ib_off,
                          f->d[i].count, f->d[i].vfirst, f->d[i].consts, f->d[i].tex0,
                          f->d[i].bind0);
        }
        hle_log("hakuX-hle-probe", "%s", line);
    }
    T.vkResetQueryPool(g_hvk.dev, g_probe.pool, base, PROBE_N);
    f->n = 0;
}

static uint32_t rec_state_hash(void);

/* Before draw c: its query index, or -1 when it is not probed. The recorded
 * state is hashed only for a probed program's draws: hashed for every draw
 * while any probe was set, it took half the RP5's render thread. */
static int probe_begin(VkCommandBuffer cb, const HleCmdDraw *c, uint32_t vfirst, uint64_t pipe)
{
    unsigned prog = 0;
    while (prog < g_probe.nvsh && g_probe.vsh[prog] != c->vsh) {
        prog++;
    }
    if (prog == g_probe.nvsh || !c->vsh || g_probe.failed ||
        hle_back_vis_query_open() || !g_hvk.caps.host_query_reset) {
        return -1;
    }
    if (!g_probe.pool) {
        VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                     .queryType = VK_QUERY_TYPE_OCCLUSION,
                                     .queryCount = PROBE_FRAMES * PROBE_N };
        if (T.vkCreateQueryPool(g_hvk.dev, &qi, NULL, &g_probe.pool) != VK_SUCCESS) {
            g_probe.failed = true;
            return -1;
        }
        T.vkResetQueryPool(g_hvk.dev, g_probe.pool, 0, PROBE_FRAMES * PROBE_N);
    }
    uint64_t swap = hle_back_swap_count();
    typeof(g_probe.fr[0]) *f = &g_probe.fr[swap % PROBE_FRAMES];
    if (f->swap != swap) {
        f->swap = swap;
        f->n = 0;
    }
    if (f->n >= PROBE_N) {
        return -1;
    }
    uint32_t i = f->n++;
    f->d[i].ib_off = c->ib_off;
    f->d[i].count = c->count;
    f->d[i].vfirst = vfirst;
    f->d[i].consts = c->consts_off;
    f->d[i].tex0 = c->tex[0];
    f->d[i].bind0 = c->nbind ? c->bind_off[0] : c->vb_off;
    f->d[i].submit = hle_back_submits();
    f->d[i].prog = (uint8_t)prog;
    f->d[i].pipe = (uint32_t)(pipe ^ pipe >> 32);
    f->d[i].state = rec_state_hash();
    {   /* the constant file as recorded: c58/c59 and a hash of all 192 */
        const uint8_t *k = g_harena.map + c->consts_off;
        memcpy(f->d[i].vp, k + 58 * 16, sizeof(f->d[i].vp));
        f->d[i].khash = probe_hash(k, 192 * 16);
        memcpy(f->d[i].xf, k, 64);
        memcpy(f->d[i].xf + 16, k + 58 * 16, 32);
    }
    {   /* its indices (arena) and the binding-0 vertices they fetch, as now */
        f->d[i].ihash = f->d[i].vhash = 0;
        uint32_t stride = c->nbind ? c->bind_stride[0] : c->stride;
        uint32_t off0 = c->nbind ? c->bind_off[0] : c->vb_off;
        bool guest = c->nbind ? (c->guest_vb & 1) : (c->guest_vb & HLE_DRAW_GUEST_FVF);
        if (c->ib_off != UINT32_MAX && stride) {
            const uint16_t *ix = (const uint16_t *)(g_harena.map + c->ib_off);
            f->d[i].ihash = probe_hash((const uint8_t *)ix, 2u * c->count);
            uint32_t h = 2166136261u;
            for (uint32_t j = 0; j < c->count; j++) {
                uint32_t a = off0 + ix[j] * stride;
                const uint8_t *p = guest ? hle_pa_ptr(a, stride) : g_harena.map + a;
                h = (h ^ (p ? probe_hash(p, stride) : 0xdeadu)) * 16777619u;
            }
            f->d[i].vhash = h;
        }
    }
    int q = (int)((swap % PROBE_FRAMES) * PROBE_N + i);
    T.vkCmdBeginQuery(cb, g_probe.pool, (uint32_t)q,
                      g_hvk.caps.occl_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    return q;
}

void hle_draw_psframe_poll(uint64_t swap)
{
    if (g_psf_frame == -2) {
        const char *e = getenv("HAKUX_HLE_PSFRAME");
        g_psf_frame = e ? atoll(e) : -1;
    }
    probe_log(swap);
    hle_quirks_poll();
#ifdef __ANDROID__
    static int64_t next_poll;
    if ((int64_t)swap < next_poll) {
        return;
    }
    next_poll = swap + 3;   /* every 3 swaps: odd, so re-arming reaches either frame parity */
    g_prop_gen++;           /* and the draws' own properties at their next draw */
    char v[PROP_VALUE_MAX] = "";
    hle_prop_get("debug.hakux.norec", v);
    if ((unsigned)atoi(v) != g_norec) {
        g_norec = (unsigned)atoi(v);
        hle_log("hakuX-hle-pipe", "norec=%u (1: no state cache, 2: no firstVertex)", g_norec);
    }
    v[0] = 0;
    hle_prop_get("debug.hakux.probe", v);
    {
        uint64_t pv[PROBE_PROGS] = { 0 };
        unsigned np = 0;
        for (const char *s = v; *s && np < PROBE_PROGS;) {
            char *end;
            uint64_t h = strtoull(s, &end, 16);
            if (end == s) {
                break;
            }
            if (h) {
                pv[np++] = h;
            }
            s = *end == ',' ? end + 1 : end;
        }
        if (np != g_probe.nvsh || memcmp(pv, g_probe.vsh, sizeof(pv))) {
            memcpy(g_probe.vsh, pv, sizeof(pv));
            g_probe.nvsh = np;
            hle_log("hakuX-hle-probe", "probe programs: %u (%s)", np, v);
        }
    }
    v[0] = 0;
    hle_prop_get("debug.hakux.eqtol", v);
    if (g_eqtol_off != !strcmp(v, "0") || g_zeq_off != !strcmp(v, "1")) {
        g_eqtol_off = !strcmp(v, "0");
        g_zeq_off = !strcmp(v, "1");
        hle_log("hakuX-hle-pipe", "EQUAL depth tests: %s",
                g_eqtol_off ? "exact" : g_zeq_off ? "a step behind, any in front" :
                "a step behind, a few in front (the depth read)");
    }
    v[0] = 0;
    hle_prop_get("debug.hakux.snap", v);
    if (g_snap_trunc != !strcmp(v, "trunc")) {
        g_snap_trunc = !strcmp(v, "trunc");
        hle_log("hakuX-hle-pipe", "vertex positions: %s",
                g_snap_trunc ? "each truncated to 1/16 pixel" : "all moved by the rounding bias (1/32 pixel)");
    }
    v[0] = 0;
    hle_prop_get("debug.hakux.probefocus", v);
    g_probe.focus = v[0] ? atoi(v) : -1;
    v[0] = 0;
    hle_prop_get("debug.hakux.probez", v);
    if ((uint32_t)atoi(v) != g_probe.zop) {
        g_probe.zop = (uint32_t)atoi(v);
        hle_log("hakuX-hle-probe", "probed draws' depth test: %s",
                g_probe.zop ? "overridden" : "theirs");
    }
    static HleProp psf_prop;
    if (hle_prop_new(&psf_prop, "debug.hakux.psframe", v)) {
        char n[PROP_VALUE_MAX] = "";
        hle_prop_get("debug.hakux.psmaxn", n);
        g_psf_maxn = (uint32_t)atoi(n);
        g_psf_frame = swap + 1;
        g_psf_nseen = 0;
        hle_log("hakuX-hle-pipe", "psframe armed for swap %llu (maxn %u)",
                (unsigned long long)g_psf_frame, g_psf_maxn);
    }
#endif
}

static void psframe_log(const HleCmdDraw *c, const HleCmdState *s, const HlePshKey *pk,
                        uint32_t rt_id, const uint32_t *psc)
{
    if (g_psf_frame < 0 || hle_back_swap_count() != (uint64_t)g_psf_frame ||
        (g_psf_maxn && c->count > g_psf_maxn)) {
        return;
    }
    uint64_t *seen = g_psf_seen;
    unsigned nseen = g_psf_nseen;
    uint64_t h = 1469598103934665603ull;
    const uint8_t *kb = (const uint8_t *)pk;
    for (size_t i = 0; i < sizeof(*pk); i++) {
        h = (h ^ kb[i]) * 1099511628211ull;
    }
    for (unsigned i = 0; i < 4; i++) {
        h = (h ^ c->tex[i]) * 1099511628211ull;
    }
    h = (h ^ (c->count <= 6 ? c->count : 0)) * 1099511628211ull;
    h = (h ^ rt_id) * 1099511628211ull;
    for (unsigned i = 0; i < 16; i++) {
        h = (h ^ psc[i]) * 1099511628211ull;
    }
    for (unsigned i = 0; i < nseen; i++) {
        if (seen[i] == h) {
            return;
        }
    }
    if (nseen >= ARRAY_SIZE(g_psf_seen)) {
        return;
    }
    seen[nseen++] = h;
    g_psf_nseen = nseen;
    HleVkSurf *rts = hle_surf_get(rt_id);
    hle_log("hakuX-hle-pipe", "psf #%u n=%u rt=%u/%d tex=%u,%u,%u,%u modes=%05x count=%x "
            "final=%08x/%08x shadow=%x/%x dot=%x in=%x cmp=%x blend=%u %x/%x bc=%08x op=%x atest=%u/%x/%x "
            "z=%u/%u/%x cw=%x vsh=%016llx",
            nseen, c->count, rt_id, rts ? (int)rts->fmt : -1, c->tex[0], c->tex[1], c->tex[2],
            c->tex[3], pk->tex_modes,
            pk->count, pk->final_abcd, pk->final_efg, pk->shadow, pk->shadow_fmt, pk->dot_map,
            pk->input_tex, pk->compare, s->rs[RS_ALPHABLENDENABLE], s->rs[RS_SRCBLEND],
            s->rs[RS_DESTBLEND], s->rs[RS_BLENDCOLOR], s->rs[RS_BLENDOP], s->rs[RS_ALPHATESTENABLE],
            s->rs[RS_ALPHAFUNC], s->rs[RS_ALPHAREF], s->rs[RS_ZENABLE], s->rs[RS_ZWRITEENABLE], s->rs[RS_ZFUNC],
            s->rs[RS_COLORWRITEENABLE], (unsigned long long)c->vsh);
    for (unsigned st = 1; st < 4; st++) {       /* each bump stage's matrix */
        unsigned m = (pk->tex_modes >> (5 * st)) & 0x1F;
        if (m == HLE_PST_BUMPENVMAP || m == HLE_PST_BUMPENVMAP_LUM) {
            float f[6];
            memcpy(f, &s->tss[st][TSS_BUMPENVMAT00], 16);
            memcpy(f + 4, &s->tss[st][TSS_BUMPENVLSCALE], 8);
            hle_log("hakuX-hle-pipe", "  bump st%u: %g %g %g %g l %g %g", st, f[0], f[1],
                    f[2], f[3], f[4], f[5]);
        }
    }
    if (s->rs[RS_FOGENABLE]) {
        float fs, fe, fd;
        memcpy(&fs, &s->rs[RS_FOGSTART], 4);
        memcpy(&fe, &s->rs[RS_FOGEND], 4);
        memcpy(&fd, &s->rs[RS_FOGDENSITY], 4);
        hle_log("hakuX-hle-pipe", "  fog: table mode %u start %g end %g density %g colour %08x "
                "(key %u)", s->rs[RS_FOGTABLEMODE], fs, fe, fd, s->rs[RS_FOGCOLOR], pk->fog);
    }
    if (c->vsh) {                               /* the program's input layout */
        hle_log("hakuX-hle-pipe", "  a: %x %x %x %x %x %x stride=%u", c->attr[0], c->attr[1],
                c->attr[2], c->attr[3], c->attr[4], c->attr[5], c->bind_stride[0]);
        /* each binding's first two vertices (bytes) */
        for (unsigned b = 0; b < c->nbind && b < 6; b++) {
            uint32_t st = c->bind_stride[b], nb = MIN(st, 24u);
            const uint8_t *vb = ((c->guest_vb >> b) & 1) ? hle_pa_ptr(c->bind_off[b], 2 * st)
                                                          : g_harena.map + c->bind_off[b];
            if (!vb || !st || st > 4096) {
                continue;
            }
            char hex[2 * (2 * 24 + 2) + 1];
            int m = 0;
            for (unsigned v = 0; v < 2; v++) {
                for (unsigned k = 0; k < nb; k++) {
                    m += snprintf(hex + m, sizeof(hex) - m, "%02x", vb[v * st + k]);
                }
                m += snprintf(hex + m, sizeof(hex) - m, v ? "" : " | ");
            }
            hle_log("hakuX-hle-pipe", "  bind%u %s %08x stride %u: %s", b,
                    ((c->guest_vb >> b) & 1) ? "pa" : "arena", c->bind_off[b], st, hex);
        }
    }
    for (unsigned i = 0; ((pk->shadow & 15) || c->count <= 6) && i < (pk->count & 0xFF) &&
                         i < 8; i++) {
        hle_log("hakuX-hle-pipe", "  comb%u rgb in=%08x out=%08x a in=%08x out=%08x fog=%u", i,
                pk->rgb_in[i], pk->rgb_out[i], pk->alpha_in[i], pk->alpha_out[i], pk->fog);
    }
    if (c->count <= 6 && c->nbind) {           /* small draws: layout and raw vertices */
        hle_log("hakuX-hle-pipe", "  attr: %x %x %x %x %x %x | bind0 off=%u stride=%u",
                c->attr[0], c->attr[1], c->attr[2], c->attr[3], c->attr[4], c->attr[5],
                c->bind_off[0], c->bind_stride[0]);
        const uint8_t *vb = (c->guest_vb & 1) ? hle_pa_ptr(c->bind_off[0], 4 * 256)
                                              : g_harena.map + c->bind_off[0];
        for (unsigned v = 0; vb && v < 4 && c->bind_stride[0] && c->bind_stride[0] <= 256; v++) {
            char hex[400];
            int m = 0;
            for (unsigned b = 0; b < MIN(c->bind_stride[0], 176u) && m < (int)sizeof(hex) - 3; b++) {
                m += snprintf(hex + m, sizeof(hex) - m, "%02x", vb[v * c->bind_stride[0] + b]);
            }
            hle_log("hakuX-hle-pipe", "  v%u: %s", v, hex);
        }
    }
    static uint64_t vdumped[32];
    bool vseen = !c->vsh;
    for (unsigned i = 0; i < 32 && !vseen; i++) {
        vseen = vdumped[i] == c->vsh;
    }
    uint32_t ns = 0;
    const uint32_t *tok = vseen ? NULL : prog_find(c->vsh, &ns);
    if (tok) {
        for (unsigned i = 0; i < 32; i++) {
            if (!vdumped[i]) {
                vdumped[i] = c->vsh;
                break;
            }
        }
        hle_log("hakuX-hle-pipe", "  vsh %016llx slots=%u attr: %x %x %x %x %x %x %x %x %x %x "
                "%x %x %x %x %x %x", (unsigned long long)c->vsh, ns, c->attr[0], c->attr[1],
                c->attr[2], c->attr[3], c->attr[4], c->attr[5], c->attr[6], c->attr[7],
                c->attr[8], c->attr[9], c->attr[10], c->attr[11], c->attr[12], c->attr[13],
                c->attr[14], c->attr[15]);
        for (uint32_t i = 0; i < ns; i++) {
            hle_log("hakuX-hle-pipe", "  vtok %u: %08x %08x %08x %08x", i, tok[4 * i],
                    tok[4 * i + 1], tok[4 * i + 2], tok[4 * i + 3]);
        }
        const float (*cf)[4] = (const float (*)[4])(g_harena.map + c->consts_off);
        for (unsigned i = 0; i < 192; i += 4) {
            hle_log("hakuX-hle-pipe", "  c%u: %g %g %g %g | %g %g %g %g | %g %g %g %g | "
                    "%g %g %g %g", i, cf[i][0], cf[i][1], cf[i][2], cf[i][3], cf[i + 1][0],
                    cf[i + 1][1], cf[i + 1][2], cf[i + 1][3], cf[i + 2][0], cf[i + 2][1],
                    cf[i + 2][2], cf[i + 2][3], cf[i + 3][0], cf[i + 3][1], cf[i + 3][2],
                    cf[i + 3][3]);
        }
    }
    for (unsigned i = 0; i < (pk->count & 0xFF) && i < 8; i++) {
        hle_log("hakuX-hle-pipe", "  st%u rgb %08x>%05x a %08x>%05x c0=%08x c1=%08x", i,
                pk->rgb_in[i], pk->rgb_out[i], pk->alpha_in[i], pk->alpha_out[i], psc[i],
                psc[8 + i]);
    }
    for (unsigned st = 0; st < 4; st++) {
        const uint32_t *t = s->tss[st];
        HleVkTex *tx = tex_find(c->tex[st]);
        HleVkSurf *sf = tx || !c->tex[st] ? NULL : hle_surf_get(c->tex[st]);
        if (!tx && !sf) {
            continue;
        }
        hle_log("hakuX-hle-pipe", "  t%u %s xfmt=0x%x %ux%ux%u%s lv=%u f=%u%s addr=%u,%u,%u "
                "filt=%u,%u,%u maxmip=%u bias=%08x tci=%x ttf=%x", st, tx ? "tex" : "surf",
                tx ? tx->xfmt : sf->d.xfmt, tx ? tx->w : sf->d.w, tx ? tx->h : sf->d.h,
                tx ? tx->d : 1, tx && tx->vol ? " vol" : "",
                tx ? tx->levels : sf->nlevels, tx ? tx->faces : sf->nfaces,
                sf && sf->general ? " general" : "", t[TSS_ADDRESSU], t[TSS_ADDRESSV],
                t[TSS_ADDRESSW], t[TSS_MAGFILTER], t[TSS_MINFILTER], t[TSS_MIPFILTER],
                t[TSS_MAXMIPLEVEL], t[TSS_MIPMAPLODBIAS], t[TSS_TEXCOORDINDEX],
                t[TSS_TEXTURETRANSFORMFLAGS]);
    }
}

enum { DROP_STATE, DROP_TARGET, DROP_SKIP, DROP_DECL, DROP_PIPE, DROP_PENDING };

/* A draw the back end drops: counted by reason (hle_draw_ff_diag), the
 * first of each reason logged with the draw. */
static void drop(int why, const HleCmdDraw *c, uint32_t rt_id, uint32_t ds_id)
{
    static const char *const k_why[] = { "no state", "no target", "skipped (debug)",
                                         "vertex layout", "no pipeline",
                                         "pipeline being built" };
    if (g_dr.drops[why]++ == 0) {
        hle_log("hakuX-hle-back", "draw dropped (%s): n=%u prim=%u fvf=0x%x vsh=%016llx "
                "nbind=%u tex=%u,%u,%u,%u rt=%u ds=%u", k_why[why], c->count, c->prim, c->fvf,
                (unsigned long long)c->vsh, c->nbind, c->tex[0], c->tex[1], c->tex[2],
                c->tex[3], rt_id, ds_id);
    }
}

/* ---- state recorded into the command buffer ----------------------------------
 * Consecutive draws repeat most of their state (Blinx: 14,000 draws a frame
 * that differ in their vertices and now and then texture or cull mode), and
 * each command costs both the driver and the GPU. The state the draws last
 * recorded into the current command buffer is kept and only what changed is
 * recorded again. A new command buffer, or anything else that binds a
 * pipeline or sets dynamic state in it (masked clears, gamma), forgets it
 * (hle_draw_state_lost). Dynamic state and bindings outlive render pass
 * instances within a command buffer; all draw pipelines share g_dr.layout. */
static struct {
    bool valid;
    VkPipeline pipe;
    VkDescriptorSet set0;               /* caps.bindless off: the draw's own (fixed_bind) */
    VkDescriptorSet set1;
    uint32_t set1_off;
    VkViewport vp;
    VkRect2D sc;
    HleFfPush pc;
    uint32_t cull, ff, topo, zt, zw, zop, bias_en, stn, sop[4], smask, swmask, sref;
    uint32_t sraw[4];               /* the registers sop was translated from */
    float bias[2], bc[4];
    bool bc_lost;                   /* a pipeline without colour zeroed the driver's
                                       blend constants (HLE_QUIRK_BLEND_CONST_RESET) */
    uint32_t nbd, nad;
    VkVertexInputBindingDescription2EXT bd[HLE_DRAW_MAX_BIND];
    VkVertexInputAttributeDescription2EXT ad[HLE_DRAW_ATTRS];
    VkBuffer vbuf[HLE_DRAW_MAX_BIND];
    VkDeviceSize voff[HLE_DRAW_MAX_BIND];
    VkBuffer ibuf;
    VkDeviceSize ioff;
} g_rec;

/* debug probe: the recorded state a draw runs with, without its per-draw
 * offsets (constants, vertex and index buffers) */
static uint32_t rec_state_hash(void)
{
    uint32_t h = probe_hash((const uint8_t *)&g_rec.vp, sizeof(g_rec.vp));
    h ^= probe_hash((const uint8_t *)&g_rec.sc, sizeof(g_rec.sc)) * 3u;
    h ^= probe_hash((const uint8_t *)&g_rec.pc, sizeof(g_rec.pc)) * 5u;
    h ^= probe_hash((const uint8_t *)&g_rec.cull,
                    offsetof(typeof(g_rec), nbd) - offsetof(typeof(g_rec), cull)) * 7u;
    h ^= probe_hash((const uint8_t *)g_rec.bd, g_rec.nbd * sizeof(g_rec.bd[0])) * 11u;
    h ^= probe_hash((const uint8_t *)g_rec.ad, g_rec.nad * sizeof(g_rec.ad[0])) * 13u;
    return h;
}

void hle_draw_rec_begin(VkCommandBuffer cb)
{
    if (g_dr.ready) {
        white_init(cb);
    }
}

void hle_draw_state_lost(void)
{
    memset(&g_rec, 0, sizeof(g_rec));   /* also what a draw does not record */
    g_rec.bc_lost = hle_quirk(HLE_QUIRK_BLEND_CONST_RESET);    /* whatever was bound */
}

/* A strip batch bound its own index buffer (batch.c). */
/* caps.bindless off: the draw's textures (pc's slots and samplers) bound as
 * set 0, a set of the draw's own: stage s's images at s, its sampler at 1 + s
 * (0: the first sampler, for the shaders' size queries). pc.tex then names
 * those places, the same for every draw (hle_glsl_fixed_slots). */
static void fixed_bind(VkCommandBuffer cb, HleFfPush *pc)
{
    FixedKey k;
    memset(&k, 0, sizeof(k));
    for (unsigned s = 0; s < HLE_FF_STAGES; s++) {
        uint32_t slot = pc->tex[s] & 0xFFFFu, smp = pc->tex[s] >> 16;
        k.img[s] = g_dr.mirror[MIN(slot, HLE_MAX_IMAGES - 1u)];
        k.smp[s] = g_dr.smp[smp < g_dr.nsmp ? smp : 0].s;
        pc->tex[s] = s | (1u + s) << 16;
    }
    VkDescriptorSet set = fixed_set(&k);
    if (set && (!g_rec.valid || g_rec.set0 != set)) {
        g_rec.set0 = set;
        T.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dr.layout, 0, 1, &set,
                                  0, NULL);
    }
}

void hle_draw_ibuf_lost(void)
{
    g_rec.ibuf = VK_NULL_HANDLE;
}

/* new != the recorded value (always when nothing is recorded); records it */
#define REC_CHANGED(field, val) \
    ((!g_rec.valid || g_rec.field != (val)) ? (g_rec.field = (val), true) : false)

static bool rec_vertex_input(uint32_t nbd, const VkVertexInputBindingDescription2EXT *bd,
                             uint32_t nad, const VkVertexInputAttributeDescription2EXT *ad)
{
    bool same = g_rec.valid && g_rec.nbd == nbd && g_rec.nad == nad;
    for (uint32_t i = 0; same && i < nbd; i++) {
        same = g_rec.bd[i].binding == bd[i].binding && g_rec.bd[i].stride == bd[i].stride &&
               g_rec.bd[i].inputRate == bd[i].inputRate &&
               g_rec.bd[i].divisor == bd[i].divisor;
    }
    for (uint32_t i = 0; same && i < nad; i++) {
        same = g_rec.ad[i].location == ad[i].location && g_rec.ad[i].binding == ad[i].binding &&
               g_rec.ad[i].format == ad[i].format && g_rec.ad[i].offset == ad[i].offset;
    }
    if (same) {
        return false;
    }
    g_rec.nbd = nbd;
    g_rec.nad = nad;
    memcpy(g_rec.bd, bd, nbd * sizeof(bd[0]));
    memcpy(g_rec.ad, ad, nad * sizeof(ad[0]));
    return true;
}

static bool rec_vertex_buffers(uint32_t n, const VkBuffer *bufs, const VkDeviceSize *offs)
{
    bool same = g_rec.valid;
    for (uint32_t i = 0; same && i < n; i++) {
        same = g_rec.vbuf[i] == bufs[i] && g_rec.voff[i] == offs[i];
    }
    if (same) {
        return false;
    }
    memcpy(g_rec.vbuf, bufs, n * sizeof(bufs[0]));
    memcpy(g_rec.voff, offs, n * sizeof(offs[0]));
    return true;
}

/* The vertex index the draw starts at, with the bindings' offsets reduced by
 * it: draws walking one buffer then keep the same binding and differ only
 * in firstVertex. Bindings read per vertex must agree on it (stride-0
 * bindings ignore it); 0 when they do not. */
static uint32_t first_vertex(uint32_t n, const uint32_t *stride, VkDeviceSize *offs)
{
    uint64_t q = UINT64_MAX;
    for (uint32_t i = 0; i < n; i++) {
        if (stride[i]) {
            uint64_t qi = offs[i] / stride[i];
            if (q != UINT64_MAX && qi != q) {
                return 0;
            }
            q = qi;
        }
    }
    if (q == UINT64_MAX || q > INT32_MAX) {
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (stride[i]) {
            offs[i] -= q * stride[i];
        }
    }
    return (uint32_t)q;
}

void hle_draw_cmd_draw(const HleCmdDraw *c, VkCommandBuffer cb, uint32_t rt_id, uint32_t ds_id,
                       const HleCmdViewport *vpc)
{
    if (!g_dr.ready || !g_dr.st_valid) {
        drop(DROP_STATE, c, rt_id, ds_id);
        return;
    }
    const HleCmdState *s = &g_dr.st;
    HleVkSurf *rt = hle_surf_get(rt_id), *ds = hle_surf_get(ds_id);
    if (!rt && !ds) {
        drop(DROP_TARGET, c, rt_id, ds_id);
        return;
    }
    {   /* debug: HAKUX_HLE_SKIPVSH=<hash>[/<count>[/<alphatest>]][,...], those
         * programs' draws (with that index count, alpha test on/off) are dropped;
         * the property debug.hakux.skipvsh (same syntax, "0" = none) replaces
         * it live */
        static uint64_t skip[8];
        static int64_t skip_n[8], skip_at[8];
        static int nskip = -1;
        static unsigned polled;
        static char last[PROP_VALUE_MAX];
        const char *e = NULL;
        char v[PROP_VALUE_MAX];
        if (nskip < 0) {
            e = getenv("HAKUX_HLE_SKIPVSH");
        }
        if (polled != g_prop_gen) {
            polled = g_prop_gen;
            v[0] = 0;
            if (hle_prop_get("debug.hakux.skipvsh", v) > 0 && strcmp(v, last)) {
                snprintf(last, sizeof(last), "%s", v);
                e = v;
                hle_log("hakuX-hle-pipe", "skipvsh: %s", v);
            }
        }
        if (e || nskip < 0) {
            nskip = 0;
            while (e && *e && nskip < 8) {
                char *end;
                skip_n[nskip] = skip_at[nskip] = -1;
                skip[nskip] = strtoull(e, &end, 16);
                if (*end == '/') {
                    skip_n[nskip] = strtoll(end + 1, &end, 10);
                }
                if (*end == '/') {
                    skip_at[nskip] = strtoll(end + 1, &end, 10);
                }
                /* "0" is none: hash 0 would match every FF draw (vsh 0) */
                nskip += skip[nskip] != 0;
                e = *end == ',' ? end + 1 : NULL;
            }
        }
        for (int i = 0; i < nskip; i++) {
            if (c->vsh == skip[i] && (skip_n[i] < 0 || c->count == (uint64_t)skip_n[i]) &&
                (skip_at[i] < 0 || (g_dr.st.rs[RS_ALPHATESTENABLE] != 0) == (skip_at[i] != 0))) {
                drop(DROP_SKIP, c, rt_id, ds_id);
                return;
            }
        }
    }
    {   /* debug: the property debug.hakux.vtok=<hash> logs that program's
         * tokens at its next draw */
        static uint64_t want;
        static unsigned polled;
        if (polled != g_prop_gen) {
            static char last[PROP_VALUE_MAX];
            polled = g_prop_gen;
            char v[PROP_VALUE_MAX] = "";
            if (hle_prop_get("debug.hakux.vtok", v) > 0 && strcmp(v, last)) {
                snprintf(last, sizeof(last), "%s", v);
                want = strtoull(v, NULL, 16);
            }
        }
        uint32_t ns = 0;
        const uint32_t *tok = want && c->vsh == want ? prog_find(c->vsh, &ns) : NULL;
        if (tok) {
            want = 0;
            hle_log("hakuX-hle-pipe", "vtok %016llx slots=%u", (unsigned long long)c->vsh, ns);
            for (uint32_t i = 0; i < ns; i++) {
                hle_log("hakuX-hle-pipe", "  vtok %u: %08x %08x %08x %08x", i, tok[4 * i],
                        tok[4 * i + 1], tok[4 * i + 2], tok[4 * i + 3]);
            }
        }
    }
    PipeKey k;
    memset(&k, 0, sizeof(k));
    k.vs.fvf = c->fvf;
    bool prog = c->vsh != 0;
    VkVertexInputAttributeDescription2EXT ad[HLE_DRAW_ATTRS];
    uint32_t na = 0, in = 0, ints = 0, cmps = 0;
    if (c->nbind && !decl_attrs(c, prog, ad, &na, &in, &ints, &cmps)) {
        g_dr.layout_skips++;
        drop(DROP_DECL, c, rt_id, ds_id);
        return;
    }
    k.vs.cmp_normal = !prog && (cmps & 2);      /* Morrowind's meshes */
    if (c->imm) {
        /* latched registers: diffuse/specular and 4 four-component coordinate
         * sets are always present */
        k.vs.fvf = (c->fvf & (HLE_FVF_POSITION_MASK | HLE_FVF_NORMAL)) | HLE_FVF_DIFFUSE |
                   HLE_FVF_SPECULAR | (4u << HLE_FVF_TEXCOUNT_SHIFT) | 0xAA0000u;
    }
    k.vs.flat = k.ps.flat = s->rs[RS_SHADEMODE] == 0x1D00;
    HleFfPush pc;
    memset(&pc, 0, sizeof(pc));
    memcpy(pc.mvp, s->mvp, sizeof(pc.mvp));
    pc.fog_color = s->rs[RS_FOGCOLOR];
    memcpy(&pc.fog[0], &s->rs[RS_FOGSTART], 4);
    memcpy(&pc.fog[1], &s->rs[RS_FOGEND], 4);
    memcpy(&pc.fog[2], &s->rs[RS_FOGDENSITY], 4);
    uint32_t rect = 0;                  /* lin_rect's (register combiners: a stage's that samples by it) */
    /* POINTSPRITEENABLE: the cascade is texture stage 3 alone (ff.h) */
    unsigned first = s->rs[RS_POINTSPRITEENABLE] ? HLE_FF_SPRITE_STAGE : 0;
    for (unsigned st = first; st < HLE_FF_STAGES; st++) {
        const uint32_t *t = s->tss[st];
        if (t[TSS_COLOROP] <= 1) {        /* DISABLE ends the cascade */
            break;
        }
        unsigned ki = st - first;         /* the key's stage */
        HleFfStage *fs = &k.ps.st[ki];
        fs->colorop = t[TSS_COLOROP];
        fs->colorarg0 = t[TSS_COLORARG0];
        fs->colorarg1 = t[TSS_COLORARG1];
        fs->colorarg2 = t[TSS_COLORARG2];
        fs->alphaop = t[TSS_ALPHAOP];
        fs->alphaarg0 = t[TSS_ALPHAARG0];
        fs->alphaarg1 = t[TSS_ALPHAARG1];
        fs->alphaarg2 = t[TSS_ALPHAARG2];
        fs->resultarg = t[TSS_RESULTARG];
        uint32_t lin = 0;
        /* fixed-function shadow buffers: not yet (white, as before) */
        uint32_t slot = shadow_fmt(c->tex[st]) ? 0 : stage_slot(c, st, rt_id, &lin);
        pc.flags |= (lin >> st & 1) << ki | (lin >> (st + 8) & 1) << (ki + 8);
        fs->texture = c->tex[st] != 0;
        fs->unset = c->tex_unset >> st & 1;
        fs->alpha_kill = fs->texture && t[TSS_ALPHAKILL] != 0;
        pc.tex[ki] = slot | sampler_index(t, stage_lod_down(c, st, rt_id)) << 16;
        k.vs.texcoord[ki] = t[TSS_TEXCOORDINDEX] & 7;
        k.ps.nstages = ki + 1;
        k.vs.ntex = ki + 1;
    }
    k.ps.fbfetch = (uint8_t)ff_fb_stages(c, rt_id);
    if (s->psh) {
        /* register combiners: every stage samples its texture; FF vertex
         * shaders feed all four coordinate sets */
        memset(&k.ps, 0, sizeof(k.ps));
        pc.flags = 0;
        for (unsigned st = 0; st < HLE_FF_STAGES; st++) {
            pc.tex[st] = stage_slot(c, st, rt_id, &pc.flags) |
                         sampler_index(s->tss[st], stage_lod_down(c, st, rt_id)) << 16;
            k.vs.texcoord[st] = s->tss[st][TSS_TEXCOORDINDEX] & 7;
        }
        k.vs.ntex = HLE_FF_STAGES;
        k.psh = 1;
        hle_psh_key(s->rs, &k.pk);
        k.pk.fbfetch = hle_draw_fb_stages(c, rt_id);
        k.pk.alpha_test = s->rs[RS_ALPHATESTENABLE] != 0;
        k.pk.alpha_func = s->rs[RS_ALPHAFUNC] >= 0x200 ? (s->rs[RS_ALPHAFUNC] - 0x200) & 7 : 7;
        k.pk.flat = s->rs[RS_SHADEMODE] == 0x1D00;
        k.pk.fog = s->rs[RS_FOGENABLE] ? (s->rs[RS_FOGTABLEMODE] & 3) + 1 : 0;
        k.pk.fog_abs = (s->rs[RS_FOGTABLEMODE] >> 8) & 1;
        k.pk.specular = s->rs[RS_SPECULARENABLE] != 0;
        k.pk.point_sprite = c->prim == 0 && s->rs[RS_POINTSPRITEENABLE] != 0;
        k.pk.out_bgra = rt && hle_surf_out_bgra(rt->fmt);
        for (unsigned st = 0; st < 4; st++) {
            k.pk.alpha_kill |= (c->tex[st] && s->tss[st][TSS_ALPHAKILL] != 0) << st;
        }
        for (unsigned st = 0; st < 4; st++) {
            unsigned sf = shadow_fmt(c->tex[st]);
            if (sf) {
                k.pk.shadow |= 1u << st;
                k.pk.shadow_fmt |= sf << (2 * st);
            }
        }
        for (unsigned st = 0; st < 4; st++) {
            /* the bound texture's dimensionality picks the fetch, as on the
             * NV2A (and LLE): a volume under a 2D-coordinate mode is read in
             * 3D (Conker's menu: animated electricity, a 256 x 128 x 4 volume,
             * bump-env mapped), PROJECT3D on a 2D texture s/q, t/q */
            unsigned m = (k.pk.tex_modes >> (5 * st)) & 0x1F;
            HleVkTex *t = c->tex[st] ? tex_find(c->tex[st]) : NULL;
            bool vol = t && t->vol, flat = c->tex[st] && !vol && !(t && t->faces == 6);
            if (vol) {
                k.pk.vol |= 1u << st;
            } else if (m == HLE_PST_PROJECT3D && flat && !((k.pk.shadow >> st) & 1)) {
                k.pk.tex_modes -= (HLE_PST_PROJECT3D - HLE_PST_PROJECT2D) << (5 * st);
            }
        }
        /* the first stage that samples a 2D image through samp() (as psh.c
         * generates it) whose linear texture is part of a larger surface:
         * its texel coordinates kept to the texture's own size */
        for (unsigned st = 0; st < 4 && !rect; st++) {
            const unsigned m = (k.pk.tex_modes >> (5 * st)) & 0x1F;
            const bool v = (k.pk.vol >> st) & 1;
            const bool samp2d = m == HLE_PST_DOT_ST || m == HLE_PST_BRDF || m == HLE_PST_DOT_ZW ||
                                m > HLE_PST_DOT_RFLCT_SPEC_CONST ||
                                ((m == HLE_PST_PROJECT2D || m == HLE_PST_BUMPENVMAP ||
                                  m == HLE_PST_BUMPENVMAP_LUM) && !v) ||
                                ((m == HLE_PST_DPNDNT_AR || m == HLE_PST_DPNDNT_GB) && !st);
            if (samp2d && !((k.pk.shadow >> st) & 1) && !((k.pk.fbfetch >> st) & 1) &&
                (pc.tex[st] & 0xFFFFu)) {
                rect = lin_rect(c, s, st);
            }
        }
        if (rect) {
            k.pk.vol |= HLE_PSH_RECT;           /* the stage fog.w names, kept to its size */
        }
        if (k.pk.shadow) {
            uint32_t f = s->rs[RS_SHADOWFUNC];
            static int lit = -1;                /* debug: HAKUX_HLE_SHADOWLIT, always lit */
            if (lit < 0) {
                lit = getenv("HAKUX_HLE_SHADOWLIT") != NULL;
            }
            /* D3DCMP_* (0x200 + n), or SET_SHADOW_COMPARE_FUNC's word as
             * pushed: the NV2A keeps its low 3 bits */
            k.pk.shadow |= (lit ? 7 : f & 7) << 4;
            static unsigned s_logged;
            if (s_logged < 4) {
                s_logged++;
                hle_log("hakuX-hle-pipe", "shadow draw: stages=%x fmt=%x func=%x modes=%05x "
                        "tex=%u,%u,%u,%u", k.pk.shadow & 15, k.pk.shadow_fmt, f,
                        k.pk.tex_modes, c->tex[0], c->tex[1], c->tex[2], c->tex[3]);
            }
        }
        static int psdbg = -1;
        if (psdbg < 0) {
            const char *e = getenv("HAKUX_HLE_PSDBG");     /* debug: HlePshKey.debug */
            psdbg = e ? atoi(e) : 0;
        }
#ifdef __ANDROID__
        {   /* on demand: debug.hakux.psdbg=<view> (0 = off) switches the combiner
             * debug view live; with pixhist.sh, a stage's value at a pixel. Polled
             * by program draws seen, not drawn: a view whose pipelines cannot be
             * made drops every draw, and must still be switched off. */
            static unsigned polled;
            if (polled != g_prop_gen) {
                char v[PROP_VALUE_MAX] = "";
                polled = g_prop_gen;
                if (hle_prop_get("debug.hakux.psdbg", v) > 0) {
                    psdbg = atoi(v);
                }
            }
        }
#endif
        k.pk.debug = psdbg;
        static int pbtint = -1;         /* debug: push-buffer runs' draws in magenta */
        if (pbtint < 0) {
            pbtint = getenv("HAKUX_HLE_PBTINT") != NULL;
        }
        if (pbtint && c->dbg) {
            k.pk.debug = 18;
        }
        for (unsigned i = 0; i < 8; i++) {
            pc.psc[i] = s->rs[HLE_PS_C0 + i];
            pc.psc[8 + i] = s->rs[HLE_PS_C1 + i];
        }
        pc.psc[16] = s->rs[HLE_PS_FINAL_C0];
        pc.psc[17] = s->rs[HLE_PS_FINAL_C1];
        for (unsigned st = 1; st < 4; st++) {   /* each bump stage's matrix */
            unsigned m = (k.pk.tex_modes >> (5 * st)) & 0x1F;
            if (m == HLE_PST_BUMPENVMAP || m == HLE_PST_BUMPENVMAP_LUM) {
                const uint32_t *t = s->tss[st];
                uint32_t *mt = st == 3 ? pc.bump3 : &pc.bump[2 * (st - 1)];
                mt[0] = half2(t[TSS_BUMPENVMAT00], t[TSS_BUMPENVMAT00 + 1]);
                mt[1] = half2(t[TSS_BUMPENVMAT00 + 2], t[TSS_BUMPENVMAT00 + 3]);
                pc.bumpl[st - 1] = half2(t[TSS_BUMPENVLSCALE], t[TSS_BUMPENVLSCALE + 1]);
            }
        }
        psframe_log(c, s, &k.pk, rt_id, pc.psc);
        static unsigned s_logged, s_cube;
        HleVkTex *t3 = tex_find(c->tex[3]);
        bool cube_draw = t3 && t3->faces == 6 && s_cube < 12;
        s_cube += cube_draw;
        if (pslog_on() && (cube_draw || (s_logged < 40 && g_dr.draws % 97 == 0))) {
            if (cube_draw) {
                const HlePshKey *q = &k.pk;
                hle_log("hakuX-hle-pipe", "cube draw: count=%x final=%08x/%08x c0=%08x %08x "
                        "%08x %08x c1=%08x %08x %08x %08x", q->count, q->final_abcd,
                        q->final_efg, pc.psc[0], pc.psc[1], pc.psc[2], pc.psc[3], pc.psc[8],
                        pc.psc[9], pc.psc[10], pc.psc[11]);
                for (unsigned i = 0; i < (q->count & 0xFF) && i < 8; i++) {
                    hle_log("hakuX-hle-pipe", "  stage %u rgb %08x -> %05x  alpha %08x -> %05x",
                            i, q->rgb_in[i], q->rgb_out[i], q->alpha_in[i], q->alpha_out[i]);
                }
                /* the vertex program (once per program), for host-side study */
                static uint64_t s_dumped[8];
                uint32_t ns = 0;
                const uint32_t *tok = c->vsh ? prog_find(c->vsh, &ns) : NULL;
                bool seen = false;
                for (unsigned i = 0; i < 8; i++) {
                    seen |= s_dumped[i] == c->vsh;
                }
                if (tok && !seen) {
                    for (unsigned i = 0; i < 8; i++) {
                        if (!s_dumped[i]) {
                            s_dumped[i] = c->vsh;
                            break;
                        }
                    }
                    hle_log("hakuX-hle-pipe", "  vsh %016llx slots=%u in=%x attrs=%x %x %x %x %x",
                            (unsigned long long)c->vsh, ns, in, c->attr[0], c->attr[2],
                            c->attr[3], c->attr[4], c->attr[9]);
                    const float (*cf)[4] = (const float (*)[4])(g_harena.map + c->consts_off);
                    static const unsigned k_c[] = { 58, 59, 97, 126, 128, 147, 148, 149, 150,
                                                    152, 163 };
                    for (unsigned i = 0; i < ARRAY_SIZE(k_c); i++) {
                        hle_log("hakuX-hle-pipe", "  c[%u] = %g %g %g %g", k_c[i],
                                cf[k_c[i]][0], cf[k_c[i]][1], cf[k_c[i]][2], cf[k_c[i]][3]);
                    }
                    for (uint32_t i = 0; i < ns; i += 2) {
                        const uint32_t *t = tok + 4 * i;
                        hle_log("hakuX-hle-pipe", "  vtok %u: %08x %08x %08x %08x %08x %08x %08x "
                                "%08x", i, t[0], t[1], t[2], t[3], i + 1 < ns ? t[4] : 0,
                                i + 1 < ns ? t[5] : 0, i + 1 < ns ? t[6] : 0,
                                i + 1 < ns ? t[7] : 0);
                    }
                }
            }
            s_logged++;
            hle_log("hakuX-hle-pipe", "psdraw tex=%05x c0=%08x %08x %08x c1=%08x %08x %08x "
                    "fc=%08x %08x fog=%08x %.3g %.3g %.3g en=%u mode=%u tex=%u,%u,%u,%u",
                    k.pk.tex_modes, pc.psc[0], pc.psc[1], pc.psc[2], pc.psc[8], pc.psc[9],
                    pc.psc[10], pc.psc[16], pc.psc[17], pc.fog_color, pc.fog[0], pc.fog[1],
                    pc.fog[2], s->rs[RS_FOGENABLE], s->rs[RS_FOGTABLEMODE], c->tex[0],
                    c->tex[1], c->tex[2], c->tex[3]);
            for (unsigned st = 0; st < 4; st++) {
                HleVkTex *tx = tex_find(c->tex[st]);
                if (tx) {
                    hle_log("hakuX-hle-pipe", "  t%u id=%u xfmt=0x%x %ux%u levels=%u faces=%u "
                            "vkfmt=%d", st, tx->id, tx->xfmt, tx->w, tx->h, tx->levels,
                            tx->faces, (int)tx->fmt);
                }
            }
        }
    }
    bool rhw = (k.vs.fvf & HLE_FVF_POSITION_MASK) == HLE_FVF_XYZRHW;
    bool ff_ubo = false;
    if (!prog) {
        /* the fog distance as D3D programs the NV2A fog generator */
        if (s->rs[RS_FOGENABLE]) {
            k.vs.fog_src = !(s->rs[RS_FOGTABLEMODE] & 3) ? HLE_FOG_SRC_SPEC_ALPHA :
                           s->rs[RS_RANGEFOGENABLE] ? HLE_FOG_SRC_RADIAL : HLE_FOG_SRC_PLANAR;
        }
        if (c->ff_ubo != HLE_DRAW_NO_UBO) {
            /* point lists: the size from the block (scaled by distance for
             * transformed vertices with POINTSCALEENABLE) */
            if (c->prim == 0) {
                k.vs.points = !rhw && s->rs[RS_POINTSCALEENABLE] ? 2 : 1;
            }
            if (!rhw) {
                ff_vs_key(&k.vs, s, c);
            }
            ff_ubo = hle_ff_vs_needs_ubo(&k.vs);
        } else if (!rhw && k.vs.fog_src != HLE_FOG_SRC_SPEC_ALPHA) {
            k.vs.fog_src = HLE_FOG_SRC_OFF;     /* no block: the clip w */
        }
    }
    /* an unlit FVF without diffuse: the diffuse register's current value
     * (D3D's SetVertexData4f(D3DVSDE_DIFFUSE) outside Begin / End), which
     * the NV2A reads for a register no array feeds; on binding 1 below */
    bool cur_diffuse = !prog && !c->nbind && !c->imm && c->diffuse_cur &&
                       !(k.vs.fvf & HLE_FVF_DIFFUSE) && !k.vs.lighting;
    if (cur_diffuse) {
        k.vs.fvf |= HLE_FVF_DIFFUSE;
    }
    if (prog) {                         /* the FF vertex key does not apply */
        memset(&k.vs, 0, sizeof(k.vs));
        k.vsh = c->vsh;
        k.vk.in_mask = in;
        k.vk.int_mask = ints;
        k.vk.cmp_mask = cmps;
        k.vk.flat = s->rs[RS_SHADEMODE] == 0x1D00;
        k.vk.spec_off = !s->spec;
        k.vk.fog_table = s->rs[RS_FOGENABLE] && (s->rs[RS_FOGTABLEMODE] & 3) != 0;
    }
    if (!s->psh) {
        k.ps.alpha_test = s->rs[RS_ALPHATESTENABLE] != 0;
        k.ps.alpha_func = s->rs[RS_ALPHAFUNC] >= 0x200 ? (s->rs[RS_ALPHAFUNC] - 0x200) & 7 : 7;
        k.ps.specular = s->rs[RS_SPECULARENABLE] != 0;
        k.ps.fog = s->rs[RS_FOGENABLE] ? (s->rs[RS_FOGTABLEMODE] & 3) + 1 : 0;
        k.ps.point_sprite = c->prim == 0 && s->rs[RS_POINTSPRITEENABLE] != 0;
        k.ps.out_bgra = rt && hle_surf_out_bgra(rt->fmt);
    }
    k.blend_en = s->rs[RS_ALPHABLENDENABLE] != 0 &&
                 !(s->psh && k.pk.debug == 17 && (k.pk.shadow & 15));  /* debug view 17 */
    k.src = s->rs[RS_SRCBLEND];
    k.dst = s->rs[RS_DESTBLEND];
    k.op = s->rs[RS_BLENDOP];
    /* The NV2A's signed equations, ADDSIGNED (0xF006) and REVSUBTRACTSIGNED
     * (0xF005), add and subtract the colours as two's-complement bytes,
     * saturating: Midnight Club 3 counts its headlight volumes' front and back
     * faces into the alpha of a cleared light mask, so the faces in front of
     * the road cancel to 0 (an unsigned subtract clamps at 0 and the add then
     * lights everything in front of the volume). With ONE / ONE the shader
     * does the equation on the target's pixel (fbfetch), the blend is off. */
    if (k.blend_en && (k.op == 0xF005 || k.op == 0xF006)) {
        unsigned sb = signed_blend(s);
        if (sb && rt && (rt->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) {
            k.blend_en = 0;
            k.op = 0x8006;
            k.ps.sblend = k.pk.sblend = (uint8_t)sb;
            g_dr.signed_blends++;
        } else {
            static unsigned s_logged;
            if (s_logged++ < 4) {
                hle_log("hakuX-hle-back", "signed blend equation %x with factors %x / %x: "
                        "drawn unsigned", k.op, k.src, k.dst);
            }
            k.op = k.op == 0xF005 ? 0x800B : 0x8006;
        }
    }
    uint32_t cw = s->rs[RS_COLORWRITEENABLE];
    bool bgra = rt && hle_surf_out_bgra(rt->fmt);       /* red and blue swap places */
    k.wmask = ((cw >> 16) & 1 ? (bgra ? VK_COLOR_COMPONENT_B_BIT : VK_COLOR_COMPONENT_R_BIT) : 0) |
              ((cw >> 8) & 1 ? VK_COLOR_COMPONENT_G_BIT : 0) |
              (cw & 1 ? (bgra ? VK_COLOR_COMPONENT_R_BIT : VK_COLOR_COMPONENT_B_BIT) : 0) |
              ((cw >> 24) & 1 ? VK_COLOR_COMPONENT_A_BIT : 0);
    k.cfmt = rt ? rt->fmt : 0;
    k.dfmt = ds ? ds->fmt : 0;
    k.topo_class = c->prim == 0 ? 0 : c->prim <= 2 ? 1 : 2;
    /* HAKUX_HLE_DBGFLAGS (debug): 1 stencil test off, 2 depth test off on
     * stencil-tested draws, 4 polygon offset off, 8 W-buffering off, 16 fog
     * off, 32 culling off */
    static int dbgf = -1;
    if (dbgf < 0) {
        const char *e = getenv("HAKUX_HLE_DBGFLAGS");
        dbgf = e ? (int)strtol(e, NULL, 0) : 0;
    }
    if (dbgf & 16) {
        k.pk.fog = 0;
        k.ps.fog = 0;
    }
    /* W-buffering (ZENABLE = D3DZB_USEW, NV2A z_perspective): the pixel stage
     * stores the perspective-correct w, for vertex programs and transformed
     * vertices (w = 1 / rhw; PGR2's menu quads over a car's leftover depth).
     * Untransformed FF vertices store w / wfar: native's projection x
     * viewport matrix scales w by the depth maximum / wfar (rt.c), so their
     * depth meets the programs' (Splinter Cell: FF world, program
     * characters); their pixel stage scale is 1 / wfar (c->wscale). */
    bool wff = !prog && !rhw && c->wscale > 0.0f;
    bool wbuf = (prog || rhw || wff) && ds && s->rs[RS_ZENABLE] == 2 && !(dbgf & 8) &&
                !hle_quirk(HLE_QUIRK_NO_WBUF);
    /* NV2A ZCLAMP_EN CLAMP: depth past the near and far planes is clamped to
     * them instead of clipping the primitive (MechAssault 2's sky, drawn past
     * the far plane, was black): the rasterizer's depth clamp */
    bool zclamp = ds && s->rs[RS_ZENABLE] != 0 && s->zclamp;
    if (prog) {
        k.vk.wbuf = wbuf;
        k.vk.zfree = !ds || s->rs[RS_ZENABLE] == 0;
        k.vk.zclamp = zclamp;
    } else {
        k.vs.wbuf = wbuf;
        k.vs.zclamp = zclamp;
    }
    if (s->psh) {
        k.pk.wbuf = wbuf;
    } else {
        k.ps.wbuf = wbuf;
    }
    /* Polygon offset (decals over coplanar surfaces): NV2A adds the offset in
     * depth-buffer units and the slope factor times the polygon's depth slope,
     * which are Vulkan's constant (one depth step) and slope factors; under
     * W-buffering the pixel stage adds them to w. Filled polygons only. */
    bool stn_req = s->rs[RS_STENCILENABLE] && s->rs[RS_STENCILFUNC] != 0x207;
    bool zt = ds && s->rs[RS_ZENABLE] != 0 && !((dbgf & 2) && stn_req);
    float zslope, zofs;
    memcpy(&zslope, &s->rs[RS_ZSLOPESCALE], 4);
    memcpy(&zofs, &s->rs[RS_ZOFFSET], 4);
    bool tri = c->prim >= VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool bias = zt && tri && !(dbgf & 4) && s->rs[RS_SOLIDOFFSETENABLE] && isfinite(zslope) &&
                isfinite(zofs) && (zslope != 0.0f || zofs != 0.0f);
    /* Passes that test without writing (LESSEQUAL / GREATEREQUAL) redraw
     * surfaces already in the depth buffer: the NV2A gives the same program
     * math the same depth on every pass, ours can come out a step off (the
     * strips' vertex order, the clip-space round trip), and the pass then
     * fails in hatched rows (Fable's terrain layers over its depth prepass).
     * One step toward the eye absorbs it. */
    if (zt && tri && !(dbgf & 4) && !s->rs[RS_ZWRITEENABLE] &&
        (s->rs[RS_ZFUNC] == 0x203 || s->rs[RS_ZFUNC] == 0x206)) {
        zslope = bias ? zslope : 0.0f;
        zofs = (bias ? zofs : 0.0f) + (s->rs[RS_ZFUNC] == 0x203 ? -1.0f : 1.0f);
        bias = true;
    }
    /* EQUAL passes that do not write need that depth bit for bit (Halo's
     * lighting passes over its base pass). Turnip gives two programs with
     * the same invariant position math a step apart when they run in
     * different command buffers (a mid-frame kick between them; the stock
     * Adreno driver does not), and the pass then misses whole triangles,
     * which flash unlit. LESSEQUAL one step toward the eye passes a step
     * either way and still fails hidden surfaces; the shader discards what
     * is further in front (hle_draw_zeq, bound by back.c). Only on the drivers
     * with HLE_QUIRK_EQ_DEPTH_STEP; debug.hakux.eqtol=0 turns it off (live). */
    bool eq_tol = zt && tri && !(dbgf & 4) && !s->rs[RS_ZWRITEENABLE] &&
                  s->rs[RS_ZFUNC] == 0x202 && hle_quirk(HLE_QUIRK_EQ_DEPTH_STEP) && !g_eqtol_off;
    if (eq_tol) {
        zslope = bias ? zslope : 0.0f;
        zofs = (bias ? zofs : 0.0f) - 1.0f;
        bias = true;
        if (s->psh) {
            k.pk.fbfetch |= (uint8_t)g_hle_zeq;
        } else {
            k.ps.fbfetch |= (uint8_t)g_hle_zeq;
        }
    }
    /* the slope is per guest pixel; the target has scale pixels to one */
    zslope *= (float)(rt ? rt->scale : ds->scale);
    if (wbuf && wff) {
        /* the offsets are in depth-buffer units; vW is w here */
        float f = c->zscale / c->wscale;
        zofs *= f;
        zslope *= f;
    }
    if (wbuf) {
        memcpy(&pc.psc[18], bias ? &zofs : &(float){ 0.0f }, 4);
        memcpy(&pc.psc[19], bias ? &zslope : &(float){ 0.0f }, 4);
        bias = false;
    }
    if (!g_hvk.caps.vids) {
        vi_key(&k.vi, c, k.vs.fvf, cur_diffuse, ad, na);
    }
    /* raster, depth and stencil state: recorded (extended dynamic state),
     * else the pipeline's (k.ds) */
    uint32_t cull = s->rs[RS_CULLMODE], ff = s->rs[RS_FRONTFACE];
    if (dbgf & 32) {
        cull = 0;                       /* debug: culling off */
    }
    const uint32_t cullv = cull == 0 ? VK_CULL_MODE_NONE :
                           cull == ff ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    const uint32_t ffv = ff == 0x901 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    const uint32_t zwv = zt && s->rs[RS_ZWRITEENABLE];
    uint32_t zopv = eq_tol ? VK_COMPARE_OP_LESS_OR_EQUAL : cmp_op(s->rs[RS_ZFUNC]);
    if (g_probe.zop && g_probe.nvsh) {  /* debug: the probed programs' depth test */
        for (unsigned i = 0; i < g_probe.nvsh; i++) {
            if (c->vsh == g_probe.vsh[i]) {
                zopv = g_probe.zop - 1;
            }
        }
    }
    const bool stn = ds && s->rs[RS_STENCILENABLE] && (ds->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) &&
                     !(dbgf & 1);
    const uint32_t sraw[4] = { s->rs[RS_STENCILFAIL], s->rs[RS_STENCILPASS],
                               s->rs[RS_STENCILZFAIL], s->rs[RS_STENCILFUNC] };
    if (!g_hvk.caps.eds1) {
        DsKey *d = &k.ds;
        d->topo = (uint8_t)c->prim;
        d->cull = (uint8_t)cullv;
        d->ff = (uint8_t)ffv;
        d->zt = zt;
        d->zw = (uint8_t)zwv;
        d->zop = (uint8_t)zopv;
        d->stn = stn;
        for (unsigned i = 0; i < 3; i++) {
            d->sop[i] = (uint8_t)stencil_op(sraw[i]);
        }
        d->sop[3] = (uint8_t)cmp_op(sraw[3]);
        if (!g_hvk.caps.vids) {         /* the strides recorded below (binding 1: 0) */
            uint32_t nb = c->nbind ? MIN(c->nbind, (uint32_t)HLE_DRAW_MAX_BIND) : 1;
            for (uint32_t i = 0; i < nb; i++) {
                d->stride[i] = (uint16_t)MIN(c->nbind ? c->bind_stride[i] : c->stride, 0xFFFFu);
            }
        }
    }
    if (!g_hvk.caps.eds2) {
        k.ds.bias = bias;
    }
    bool pending;
    VkPipeline p = pipeline(&k, rt ? rt : ds, &pending);
    if (!p) {
        drop(pending ? DROP_PENDING : DROP_PIPE, c, rt_id, ds_id);
        return;
    }
    if (g_norec & 1) {
        hle_draw_state_lost();
    }
    if (REC_CHANGED(pipe, p)) {
        T.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        g_dr.emits[EM_PIPE]++;
        if (hle_quirk(HLE_QUIRK_STATE_AFTER_PIPELINE)) {
            hle_draw_state_lost();      /* the driver keeps only the pipeline (quirks.h) */
            g_rec.pipe = p;
        } else if (hle_quirk(HLE_QUIRK_VI_AFTER_PIPELINE)) {
            g_rec.nbd = UINT32_MAX;     /* the vertex input is recorded again (quirks.h) */
        }
        g_rec.bc_lost |= !k.cfmt && hle_quirk(HLE_QUIRK_BLEND_CONST_RESET);
    }
    if (!g_rec.valid) {                 /* the bindless set never changes */
        if (g_hvk.caps.bindless) {
            T.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dr.layout, 0, 1,
                                      &g_dr.set, 0, NULL);
        }
        if (g_hvk.caps.eds2) {
            T.vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);  /* on only inside strip batches */
        }
    }
    if (prog || ff_ubo) {
        VkDescriptorSet s1 = prog ? g_dr.cset : g_dr.fset;
        uint32_t off = prog ? c->consts_off : c->ff_ubo;
        bool ch = REC_CHANGED(set1, s1);
        if (REC_CHANGED(set1_off, off) || ch) {
            T.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_dr.layout, 1, 1,
                                      &s1, 1, &off);
            g_dr.emits[EM_SET1]++;
        }
    }
    /* The shaders leave positions in D3D screen pixels (programs: c[58]/c[59];
     * fixed function: pc.scr, the D3D viewport transform and the
     * screen-space offset) and map them to the whole target, scaled by the
     * NV2A's antialiasing factor when it antialiases (D3D's are then logical
     * pixels); y flipped (D3D/NV2A NDC y up), the D3D viewport as scissor and
     * depth range. */
    uint32_t tw = rt ? rt->d.w : ds->d.w, th = rt ? rt->d.h : ds->d.h;
    uint32_t sw = rt ? rt->w : ds->w, sh = rt ? rt->h : ds->h;
    uint32_t rs = rt ? rt->scale : ds->scale;
    bool full = !vpc->w || !vpc->h;
    VkRect2D sc = full ? (VkRect2D){ { 0, 0 }, { sw, sh } } :
                         (VkRect2D){ { (int32_t)(vpc->x * rs), (int32_t)(vpc->y * rs) },
                                     { vpc->w * rs, vpc->h * rs } };
    if (vpc->clip) {
        /* SetScissors' rectangle replaces the viewport as the clip */
        int32_t x1 = MAX(vpc->cx1, 0), y1 = MAX(vpc->cy1, 0);
        int32_t x2 = MIN(vpc->cx2, (int32_t)tw), y2 = MIN(vpc->cy2, (int32_t)th);
        sc = (VkRect2D){ { x1 * (int32_t)rs, y1 * (int32_t)rs },
                         { x2 > x1 ? (uint32_t)(x2 - x1) * rs : 0,
                           y2 > y1 ? (uint32_t)(y2 - y1) * rs : 0 } };
    }
    if (g_hle_gpucut == 1 || g_hle_gpucut == 4) {
        sc = (VkRect2D){ { 0, 0 }, { 1, 1 } };  /* debug: next to no fragments */
    }
    VkViewport v = { 0, (float)sh, (float)sw, -(float)sh, 0.0f, 1.0f };
    if (!prog && !rhw && !full) {
        v.minDepth = vpc->minz;
        v.maxDepth = vpc->maxz;
    }
    if (!g_rec.valid || memcmp(&g_rec.vp, &v, sizeof(v))) {
        g_rec.vp = v;
        T.vkCmdSetViewport(cb, 0, 1, &v);
        g_dr.emits[EM_VP]++;
    }
    if (!g_rec.valid || memcmp(&g_rec.sc, &sc, sizeof(sc))) {
        g_rec.sc = sc;
        T.vkCmdSetScissor(cb, 0, 1, &sc);
        g_dr.emits[EM_SC]++;
    }
    float msx = MAX(s->xs, 1u), msy = MAX(s->ys, 1u);
    float aax = s->aa ? msx : 1.0f, aay = s->aa ? msy : 1.0f;
    pc.vp[0] = 2.0f * aax / (float)tw;
    pc.vp[1] = -2.0f * aay / (float)th;
    if (rhw && !prog) {
        /* XYZRHW positions are in logical (unscaled) target pixels */
        pc.scr[0] = msx / aax;
        pc.scr[1] = msy / aay;
        pc.scr[2] = vpc->offx;
        pc.scr[3] = vpc->offy;
    } else if (!prog) {
        /* the viewport rectangle is in target pixels */
        float vx = full ? 0.0f : vpc->x, vy = full ? 0.0f : vpc->y;
        float vw = full ? tw : vpc->w, vh = full ? th : vpc->h;
        pc.scr[0] = vw * 0.5f / aax;
        pc.scr[1] = -vh * 0.5f / aay;
        pc.scr[2] = (vx + vw * 0.5f) / aax + vpc->offx;
        pc.scr[3] = (vy + vh * 0.5f) / aay + vpc->offy;
    }
    pc.zscale = wbuf && wff ? c->wscale : c->zscale;
    pc.tfactor = s->rs[RS_TEXTUREFACTOR];
    pc.alpha_ref = (s->rs[RS_ALPHAREF] & 0xFF) / 255.0f;
    memcpy(&pc.fog[3], &rect, 4);       /* (bits: HLE_NVRECT_GLSL) */
    /* the target's scale (point sizes, texel edges) and linear surfaces'
     * (always scaled); antialiased: texel edges as they are (ff.h) */
    pc.flags |= MIN(rs, 255u) << 16 | MIN(g_hle_rscale, 255u) << 24 |
                (aax > 1.0f || aay > 1.0f ? HLE_TEXEL_EDGE_FLAG : 0u) | (g_snap_trunc ? HLE_SNAP_TRUNC_FLAG : 0u);
    if (!g_hvk.caps.bindless) {
        fixed_bind(cb, &pc);            /* the textures a set of the draw's own */
    }
    if (!g_rec.valid || memcmp(&g_rec.pc, &pc, sizeof(pc))) {
        g_rec.pc = pc;
        if (g_hle_push_ssbo) {
            push_block(cb, &pc);
        } else {
            T.vkCmdPushConstants(cb, g_dr.layout,
                                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(pc), &pc);
        }
        g_dr.emits[EM_PC]++;
    }
    /* raster / depth / stencil (computed above; tracked without extended
     * dynamic state too: what the draw writes, below) */
    const bool eds1 = g_hvk.caps.eds1, eds2 = g_hvk.caps.eds2;
    if (REC_CHANGED(cull, cullv) && eds1) {
        T.vkCmdSetCullMode(cb, g_rec.cull);
        g_dr.emits[EM_DYN]++;
    }
    if (REC_CHANGED(ff, ffv) && eds1) {
        T.vkCmdSetFrontFace(cb, (VkFrontFace)g_rec.ff);
        g_dr.emits[EM_DYN]++;
    }
    if (REC_CHANGED(topo, c->prim) && eds1) {
        T.vkCmdSetPrimitiveTopology(cb, (VkPrimitiveTopology)c->prim);
        g_dr.emits[EM_DYN]++;
    }
    if (REC_CHANGED(zt, zt) && eds1) {
        T.vkCmdSetDepthTestEnable(cb, zt);
        g_dr.emits[EM_DYN]++;
    }
    if (REC_CHANGED(zw, zwv) && eds1) {
        T.vkCmdSetDepthWriteEnable(cb, g_rec.zw);
        g_dr.emits[EM_DYN]++;
    }
    if (REC_CHANGED(zop, zopv) && eds1) {
        T.vkCmdSetDepthCompareOp(cb, (VkCompareOp)g_rec.zop);
        g_dr.emits[EM_DYN]++;
    }
    if (REC_CHANGED(bias_en, bias) && eds2) {
        T.vkCmdSetDepthBiasEnable(cb, bias);
        g_dr.emits[EM_DYN]++;
    }
    {
        float bv[2] = { bias ? zofs : 0.0f, bias ? zslope : 0.0f };
        if (!g_rec.valid || memcmp(g_rec.bias, bv, sizeof(bv))) {
            memcpy(g_rec.bias, bv, sizeof(bv));
            T.vkCmdSetDepthBias(cb, bv[0], 0.0f, bv[1]);
            g_dr.emits[EM_DYN]++;
        }
    }
    if (REC_CHANGED(stn, stn) && eds1) {
        T.vkCmdSetStencilTestEnable(cb, stn);
        g_dr.emits[EM_DYN]++;
    }
    if (!g_rec.valid || memcmp(g_rec.sraw, sraw, sizeof(sraw))) {
        memcpy(g_rec.sraw, sraw, sizeof(sraw));
        uint32_t so[4] = { stencil_op(sraw[0]), stencil_op(sraw[1]), stencil_op(sraw[2]),
                           cmp_op(sraw[3]) };
        if (!g_rec.valid || memcmp(g_rec.sop, so, sizeof(so))) {
            memcpy(g_rec.sop, so, sizeof(so));
            if (eds1) {
                T.vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (VkStencilOp)so[0],
                                    (VkStencilOp)so[1], (VkStencilOp)so[2], (VkCompareOp)so[3]);
                g_dr.emits[EM_DYN]++;
            }
        }
    }
    if (REC_CHANGED(smask, s->rs[RS_STENCILMASK] & 0xFF)) {
        T.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g_rec.smask);
        g_dr.emits[EM_DYN]++;
    }
    if (REC_CHANGED(swmask, s->rs[RS_STENCILWRITEMASK] & 0xFF)) {
        T.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g_rec.swmask);
        g_dr.emits[EM_DYN]++;
    }
    if (REC_CHANGED(sref, s->rs[RS_STENCILREF] & 0xFF)) {
        T.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, g_rec.sref);
        g_dr.emits[EM_DYN]++;
    }
    if (ds && (g_rec.zw || (stn && g_rec.swmask && (g_rec.sop[0] | g_rec.sop[1] | g_rec.sop[2])))) {
        hle_surf_written(ds);           /* HLE_OP_ZREAD / ZWRITE: which wrote the memory last */
    }
    if (prog && !s->psh && g_psf_frame >= 0 && hle_back_swap_count() == (uint64_t)g_psf_frame) {
        /* debug (debug.hakux.psframe): a program draw's fixed-function stages */
        char sb[256];
        int o = 0;
        for (unsigned i = 0; i < k.ps.nstages && o < (int)sizeof(sb) - 48; i++) {
            const HleFfStage *f = &k.ps.st[i];
            o += snprintf(sb + o, sizeof(sb) - o, " s%u %u/%u,%u a%u/%u,%u r%u t%u u%u k%u", i,
                          f->colorop, f->colorarg1, f->colorarg2, f->alphaop, f->alphaarg1,
                          f->alphaarg2, f->resultarg, f->texture, f->unset, f->alpha_kill);
        }
        hle_log("hakuX-hle-pipe", "prog ff draw n=%u vsh=%016llx tex=%u,%u,%u,%u unset=%x "
                "stages=%u psprite=%u%s", c->count, (unsigned long long)c->vsh, c->tex[0],
                c->tex[1], c->tex[2], c->tex[3], c->tex_unset, k.ps.nstages,
                s->rs[RS_POINTSPRITEENABLE], sb);
    }
    if (!prog && g_psf_frame >= 0 && hle_back_swap_count() == (uint64_t)g_psf_frame) {
        /* debug (debug.hakux.psframe): a fixed-function draw's raster state */
        hle_log("hakuX-hle-pipe", "ff draw n=%u prim=%u fvf=%x rt=%u %ux%u ds=%u sc=%d,%d %ux%u "
                "vp=%g,%g scr=%g,%g,%g,%g cull=%x ff=%x zt=%d zw=%u zf=%x stn=%d "
                "wmask=%x blend=%u atest=%u st0 tex=%u op=%u akill=%u",
                c->count, c->prim, c->fvf, rt_id, tw, th, ds_id, sc.offset.x, sc.offset.y,
                sc.extent.width, sc.extent.height, pc.vp[0], pc.vp[1],
                pc.scr[0], pc.scr[1], pc.scr[2], pc.scr[3], cull, ff, zt,
                s->rs[RS_ZWRITEENABLE], s->rs[RS_ZFUNC], stn, k.wmask, k.blend_en,
                k.ps.alpha_test, c->tex[0], s->tss[0][TSS_COLOROP], s->tss[0][TSS_ALPHAKILL]);
    }
    uint32_t bc = s->rs[RS_BLENDCOLOR];
    if (rt && hle_surf_out_bgra(rt->fmt)) {             /* red and blue swap places */
        bc = (bc & 0xFF00FF00u) | ((bc >> 16) & 0xFF) | ((bc & 0xFF) << 16);
    }
    float bcf[4] = { ((bc >> 16) & 0xFF) / 255.0f, ((bc >> 8) & 0xFF) / 255.0f,
                     (bc & 0xFF) / 255.0f, (bc >> 24) / 255.0f };
    /* HLE_QUIRK_BLEND_CONST_RESET: the first colour draw after a pipeline
     * without colour sets other constants first, so its own are recorded */
    bool bc_reset = g_rec.bc_lost && k.cfmt;
    if (!g_rec.valid || memcmp(g_rec.bc, bcf, sizeof(bcf)) || bc_reset) {
        memcpy(g_rec.bc, bcf, sizeof(bcf));
        if (bc_reset) {
            const float other[4] = { bcf[0], bcf[1], bcf[2], bcf[3] == 0.0f ? 1.0f : 0.0f };
            T.vkCmdSetBlendConstants(cb, other);
            g_rec.bc_lost = false;
        }
        T.vkCmdSetBlendConstants(cb, bcf);
        g_dr.emits[EM_DYN]++;
    }
    /* vertex input: the vertex offset goes into firstVertex (vertexOffset),
     * so draws walking one buffer keep their binding */
    VkVertexInputBindingDescription2EXT bd[HLE_DRAW_MAX_BIND];
    VkBuffer bufs[HLE_DRAW_MAX_BIND];
    VkDeviceSize offs[HLE_DRAW_MAX_BIND];
    uint32_t strides[HLE_DRAW_MAX_BIND];
    uint32_t nb;
    if (c->nbind) {
        nb = MIN(c->nbind, (uint32_t)HLE_DRAW_MAX_BIND);
        for (uint32_t i = 0; i < nb; i++) {
            strides[i] = c->bind_stride[i];
            bufs[i] = (c->guest_vb >> i) & 1 ? g_hram.buf : g_harena.buf;  /* in place */
            offs[i] = c->bind_off[i];
        }
    } else {
        nb = 1;
        strides[0] = c->stride;
        na = c->imm ? imm_attrs(k.vs.fvf, ad) : fvf_attrs(c->fvf, ad);
        bufs[0] = (c->guest_vb & HLE_DRAW_GUEST_FVF) ? g_hram.buf : g_harena.buf;
        offs[0] = c->vb_off;
        if (cur_diffuse) {
            ad[na++] = (VkVertexInputAttributeDescription2EXT){
                .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                .location = 2, .binding = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT };
            strides[1] = 0;
            bufs[1] = g_harena.buf;
            offs[1] = c->diffuse_cur - 1;
            nb = 2;
        }
    }
    for (uint32_t i = 0; i < nb; i++) {
        bd[i] = (VkVertexInputBindingDescription2EXT){
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
            .binding = i, .stride = strides[i],
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX, .divisor = 1 };
    }
    uint32_t vfirst = g_norec & 2 ? 0 : first_vertex(nb, strides, offs);
    if (!g_hvk.caps.vids) {
        /* the layout is the pipeline's (k.vi): only the buffers and strides */
        bool vi = rec_vertex_input(nb, bd, na, ad);
        if (rec_vertex_buffers(nb, bufs, offs) || vi) {
            VkDeviceSize st[HLE_DRAW_MAX_BIND];
            for (uint32_t i = 0; i < nb; i++) {
                st[i] = strides[i];
            }
            if (eds1) {
                T.vkCmdBindVertexBuffers2(cb, 0, nb, bufs, offs, NULL, st);
            } else {                    /* the strides are the pipeline's too (k.ds) */
                T.vkCmdBindVertexBuffers(cb, 0, nb, bufs, offs);
            }
            g_dr.emits[EM_VB]++;
        }
    } else {
        if (rec_vertex_input(nb, bd, na, ad)) {
            T.vkCmdSetVertexInputEXT(cb, nb, bd, na, ad);
            g_dr.emits[EM_VI]++;
        }
        if (rec_vertex_buffers(nb, bufs, offs)) {
            T.vkCmdBindVertexBuffers(cb, 0, nb, bufs, offs);
            g_dr.emits[EM_VB]++;
        }
    }
    /* Merge potential (counted, not done yet): a non-indexed list draw that
     * recorded no state and nothing else since the previous one, and whose
     * vertices continue the previous one's with the same topology. */
    {
        extern uint64_t hle_back_breaks;
        static struct { bool valid; uint64_t emits, breaks; uint32_t prim, end; } mp;
        uint64_t em = 0;
        for (unsigned i = 0; i < EM_N; i++) {
            em += g_dr.emits[i];
        }
        bool list = c->prim == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ||
                    c->prim == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
                    c->prim == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        bool flat = c->ib_off == UINT32_MAX && list;
        if (!list) {
            g_dr.nomerge[0]++;
        } else if (c->ib_off != UINT32_MAX) {
            g_dr.nomerge[1]++;
        } else if (!mp.valid || em != mp.emits || mp.prim != c->prim) {
            g_dr.nomerge[2]++;
        } else if (hle_back_breaks != mp.breaks) {
            g_dr.nomerge[3]++;
        } else if (mp.end != vfirst) {
            g_dr.nomerge[4]++;
        } else {
            g_dr.mergeable++;
        }
        /* indexed: no state since the previous indexed draw (same topology),
         * and of those, the ones whose indices also follow its own at the
         * same vertex base (one draw as they are) */
        static struct { bool valid; uint64_t emits, breaks; uint32_t prim, iend, vbase; } mi;
        if (c->ib_off != UINT32_MAX) {
            if (mi.valid && em == mi.emits && hle_back_breaks == mi.breaks && mi.prim == c->prim) {
                g_dr.idx_same++;
                g_dr.idx_contig += mi.iend == c->ib_off / 2 && mi.vbase == vfirst;
            }
            mi.iend = c->ib_off / 2 + c->count;
            mi.vbase = vfirst;
        }
        mi.valid = c->ib_off != UINT32_MAX && list;
        mi.emits = em;
        mi.breaks = hle_back_breaks;
        mi.prim = c->prim;
        mp.valid = flat;
        mp.emits = em;
        mp.breaks = hle_back_breaks;
        mp.prim = c->prim;
        mp.end = vfirst + c->count;
    }
    if (c->ib_off != UINT32_MAX) {
        /* A strip run still held binds its index buffer as it is recorded,
         * which this draw's own recording would set off after the check
         * below: the draw then read the run's indices. Nightcaster's terrain
         * lost tiles so, its strips read in place sharing one binding (no
         * state recorded between them and the indexed draw after). */
        hle_batch_flush(cb);
        /* 16-bit indices from the arena, bound once: the offset is firstIndex */
        if (REC_CHANGED(ibuf, g_harena.buf)) {
            T.vkCmdBindIndexBuffer(cb, g_harena.buf, 0, VK_INDEX_TYPE_UINT16);
        }
        int q = g_probe.nvsh ? probe_begin(cb, c, vfirst, (uint64_t)(uintptr_t)g_rec.pipe)
                                : -1;
        if (g_hle_gpucut != 2) {
            T.vkCmdDrawIndexed(cb, g_hle_gpucut == 4 ? MIN(c->count, 3u) : c->count, 1,
                               c->ib_off / 2, (int32_t)vfirst, 0);
        }
        if (q >= 0) {
            T.vkCmdEndQuery(cb, g_probe.pool, (uint32_t)q);
        }
    } else {
        int q = g_probe.nvsh ? probe_begin(cb, c, vfirst, (uint64_t)(uintptr_t)g_rec.pipe)
                                : -1;
        if (g_hle_gpucut == 2) {
            /* debug: the state alone */
        } else if (g_hle_gpucut == 4) {
            T.vkCmdDraw(cb, MIN(c->count, 3u), 1, vfirst, 0);  /* debug: a draw's own cost */
        } else if (q < 0 && (c->prim == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                      c->prim == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
                      c->prim == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) &&
            hle_batch_draw(cb, (VkPrimitiveTopology)c->prim, vfirst, c->count)) {
            /* held: recorded with the strips that follow it (a probed draw never is) */
        } else {
            T.vkCmdDraw(cb, c->count, 1, vfirst, 0);
        }
        if (q >= 0) {
            T.vkCmdEndQuery(cb, g_probe.pool, (uint32_t)q);
        }
    }
    g_rec.valid = true;
    g_dr.draws++;
    g_dr.verts += c->count;
    g_dr.prog_draws += prog;
    if (prog) {
        g_dr.prog_fog += s->rs[RS_FOGENABLE] != 0;
    } else if ((c->fvf & HLE_FVF_POSITION_MASK) != HLE_FVF_XYZRHW) {
        bool tg = false, tx = false;
        for (unsigned st = 0; st < k.vs.ntex; st++) {
            tg |= (s->tss[st][TSS_TEXCOORDINDEX] >> 16) != 0;
            tx |= (s->tss[st][TSS_TEXTURETRANSFORMFLAGS] & 7) != 0;
        }
        g_dr.ff3d++;
        g_dr.ff_lit += s->rs[RS_LIGHTING] && (c->fvf & HLE_FVF_NORMAL);
        g_dr.ff_fog += s->rs[RS_FOGENABLE] != 0;
        g_dr.ff_texgen += tg;
        g_dr.ff_texxform += tx;
        g_dr.ff_blend += s->rs[RS_VERTEXBLEND] != 0;
        g_dr.ff_psprite += s->rs[RS_POINTSPRITEENABLE] && c->prim == 0;
    }
    /* debug: the first point-list draws' state (point sprites, P3 Task 11) */
    static unsigned s_pts;
    if (c->prim == 0 && s_pts < 24 && pslog_on()) {
        s_pts++;
        const uint32_t *r = s->rs;
        float ps[8];
        memcpy(ps, &r[116], sizeof(ps));
        hle_log("hakuX-hle-pipe", "points: n=%u fvf=0x%x prog=%d psh=%u sprite=%u size=%g "
                "min=%g scale=%u a=%g b=%g c=%g max=%g tex=%u,%u,%u,%u "
                "op=%u,%u,%u,%u aop=%u,%u,%u,%u tci=%x,%x,%x,%x blend=%u %x/%x z=%u/%u",
                c->count, c->fvf, prog, s->psh, r[RS_POINTSPRITEENABLE], ps[0], ps[1],
                r[119], ps[4], ps[5], ps[6], ps[7], c->tex[0], c->tex[1], c->tex[2],
                c->tex[3], s->tss[0][TSS_COLOROP], s->tss[1][TSS_COLOROP],
                s->tss[2][TSS_COLOROP], s->tss[3][TSS_COLOROP], s->tss[0][TSS_ALPHAOP],
                s->tss[1][TSS_ALPHAOP], s->tss[2][TSS_ALPHAOP], s->tss[3][TSS_ALPHAOP],
                s->tss[0][TSS_TEXCOORDINDEX], s->tss[1][TSS_TEXCOORDINDEX],
                s->tss[2][TSS_TEXCOORDINDEX], s->tss[3][TSS_TEXCOORDINDEX],
                r[RS_ALPHABLENDENABLE], r[RS_SRCBLEND], r[RS_DESTBLEND], r[RS_ZENABLE],
                r[RS_ZWRITEENABLE]);
    }
}

void hle_draw_ff_diag(void)
{
    static uint64_t prev[EM_N], prev_draws, prev_merge, prev_no[5], prev_verts;
    char line[512];
    int n = snprintf(line, sizeof(line), "per s: draws=%llu verts=%llu mergeable=%llu (not: strip "
                     "%llu idx %llu state %llu work %llu gap %llu)",
                     (unsigned long long)(g_dr.draws - prev_draws),
                     (unsigned long long)(g_dr.verts - prev_verts),
                     (unsigned long long)(g_dr.mergeable - prev_merge),
                     (unsigned long long)(g_dr.nomerge[0] - prev_no[0]),
                     (unsigned long long)(g_dr.nomerge[1] - prev_no[1]),
                     (unsigned long long)(g_dr.nomerge[2] - prev_no[2]),
                     (unsigned long long)(g_dr.nomerge[3] - prev_no[3]),
                     (unsigned long long)(g_dr.nomerge[4] - prev_no[4]));
    prev_merge = g_dr.mergeable;
    memcpy(prev_no, g_dr.nomerge, sizeof(prev_no));
    static uint64_t prev_batches, prev_merged;
    uint64_t batches, merged;
    hle_batch_stats(&batches, &merged);
    n += snprintf(line + n, sizeof(line) - (size_t)n, " batches=%llu of %llu draws",
                  (unsigned long long)(batches - prev_batches), (unsigned long long)(merged - prev_merged));
    prev_batches = batches;
    prev_merged = merged;
    static uint64_t prev_isame, prev_icontig;
    n += snprintf(line + n, sizeof(line) - (size_t)n, " indexed: no state between %llu, "
                  "contiguous %llu", (unsigned long long)(g_dr.idx_same - prev_isame),
                  (unsigned long long)(g_dr.idx_contig - prev_icontig));
    prev_isame = g_dr.idx_same;
    prev_icontig = g_dr.idx_contig;
    for (unsigned i = 0; i < EM_N && n > 0 && (size_t)n < sizeof(line); i++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %s=%llu", k_em_names[i],
                      (unsigned long long)(g_dr.emits[i] - prev[i]));
        prev[i] = g_dr.emits[i];
    }
    prev_draws = g_dr.draws;
    prev_verts = g_dr.verts;
    hle_log("hakuX-hle-emit", "%s", line);
    hle_log("hakuX-hle-back", "ff3d=%llu lit=%llu fog=%llu texgen=%llu texxform=%llu "
            "blend=%llu psprite=%llu prog=%llu prog_fog=%llu dropped: state=%llu target=%llu "
            "skip=%llu layout=%llu pipeline=%llu",
            (unsigned long long)g_dr.ff3d, (unsigned long long)g_dr.ff_lit,
            (unsigned long long)g_dr.ff_fog, (unsigned long long)g_dr.ff_texgen,
            (unsigned long long)g_dr.ff_texxform, (unsigned long long)g_dr.ff_blend,
            (unsigned long long)g_dr.ff_psprite, (unsigned long long)g_dr.prog_draws,
            (unsigned long long)g_dr.prog_fog, (unsigned long long)g_dr.drops[DROP_STATE],
            (unsigned long long)g_dr.drops[DROP_TARGET], (unsigned long long)g_dr.drops[DROP_SKIP],
            (unsigned long long)g_dr.drops[DROP_DECL], (unsigned long long)g_dr.drops[DROP_PIPE]);
    {   /* new pipelines this second: from the caches at once, queued to the
         * workers and built by them, built in place; prebuilt ones first used;
         * draws skipped while theirs were being built */
        static uint64_t p[6];
        uint64_t v[6] = { g_dr.cache_hits, g_dr.async_queued, g_dr.async_built, g_dr.inplace,
                          g_dr.prebuilt_used, g_dr.drops[DROP_PENDING] };
        uint32_t inflight = hle_pipe_inflight();
        if (memcmp(v, p, sizeof(v)) || inflight) {
            hle_log("hakuX-hle-pipe", "per s: cache=%llu queued=%llu built=%llu inplace=%llu "
                    "inflight=%u prebuilt_used=%llu skipped_draws=%llu | pipelines %u (prebuilt %llu)",
                    (unsigned long long)(v[0] - p[0]), (unsigned long long)(v[1] - p[1]),
                    (unsigned long long)(v[2] - p[2]), (unsigned long long)(v[3] - p[3]),
                    inflight, (unsigned long long)(v[4] - p[4]), (unsigned long long)(v[5] - p[5]),
                    g_dr.npipes, (unsigned long long)g_dr.prebuilt);
            memcpy(p, v, sizeof(p));
        }
    }
}

