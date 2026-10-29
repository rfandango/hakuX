# hakuX native core: the HLE GPU layer (hw/xbox/hle) on its host interface
# (hw/xbox/hle/host.h, native/hle) and the GLib subset (native/compat/glib);
# the MCPX APU (native/dev/apu).
#
# Vulkan dependencies come from NC_DEPS (same revisions as the app build):
#   volk, glslang, spirv_reflect, vma, root/subprojects/nv2a_vsh_cpu.
set(NC_DEPS "D:/tools/native-deps" CACHE PATH "native core third-party sources")

add_library(nc_volk STATIC "${NC_DEPS}/volk/volk.c")
target_include_directories(nc_volk PUBLIC "${NC_DEPS}/volk")
if(EXISTS "${NC_DEPS}/Vulkan-Headers/include/vulkan/vulkan.h")
  target_include_directories(nc_volk PUBLIC "${NC_DEPS}/Vulkan-Headers/include")
endif()
target_compile_definitions(nc_volk PUBLIC VK_NO_PROTOTYPES)
if(ANDROID)
  target_compile_definitions(nc_volk PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
endif()

add_library(nc_spirv_reflect STATIC "${NC_DEPS}/spirv_reflect/spirv_reflect.c")
target_include_directories(nc_spirv_reflect PUBLIC "${NC_DEPS}/spirv_reflect")

set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${NC_DEPS}/glslang" glslang-build EXCLUDE_FROM_ALL)

set(NC_HLE "${NC_REPO}/hw/xbox/hle")

# The GLib subset the HLE and the APU use.
add_library(nc_glib STATIC "${CMAKE_CURRENT_SOURCE_DIR}/compat/glib/glib.c")
target_include_directories(nc_glib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/compat/glib")
set(NC_VSH "${NC_DEPS}/root/subprojects/nv2a_vsh_cpu/src")

# Every HLE GPU source.
add_library(hakux_hle STATIC
  ${NC_HLE}/hle.c
  ${NC_HLE}/hitch.c
  ${NC_HLE}/core/title.c
  ${NC_HLE}/core/probe.c
  ${NC_HLE}/core/xbe_guest.c
  ${NC_HLE}/front/arena.c
  ${NC_HLE}/front/fe.c
  ${NC_HLE}/front/ring.c
  ${NC_HLE}/mem/barrier.c
  ${NC_HLE}/mem/guestmem.c
  ${NC_HLE}/mem/pageuse.c
  ${NC_HLE}/pb/kelvin.c
  ${NC_HLE}/pb/pbdraw.c
  ${NC_HLE}/pb/pbmode.c
  ${NC_HLE}/pb/pbres.c
  ${NC_HLE}/pb/pbstate.c
  ${NC_HLE}/pb/pusher.c
  ${NC_HLE}/shader/ff_gen.c
  ${NC_HLE}/shader/ffhw.c
  ${NC_HLE}/shader/glsl_compile.c
  ${NC_HLE}/shader/psh.c
  ${NC_HLE}/shader/vsh.c
  ${NC_HLE}/tex/xdk_res.c
  ${NC_HLE}/tex/xg_swizzle.c
  ${NC_HLE}/uma/uma.c
  ${NC_HLE}/util/thread.c
  ${NC_HLE}/vk/back.c
  ${NC_HLE}/vk/display.c
  ${NC_HLE}/vk/batch.c
  ${NC_HLE}/vk/compat.c
  ${NC_HLE}/vk/draw.c
  ${NC_HLE}/vk/quirks.c
  ${NC_HLE}/vk/gamma.c
  ${NC_HLE}/vk/guest_ram.c
  ${NC_HLE}/vk/pcache.c
  ${NC_HLE}/vk/pipeasync.c
  ${NC_HLE}/vk/surf.c
  ${NC_HLE}/vk/timeline.c
  ${NC_HLE}/vk/vkdev.c
  ${NC_HLE}/vk/zread.c
  ${NC_VSH}/nv2a_vsh_disassembler.c
  ${NC_VSH}/nv2a_vsh_cpu.c
  ${NC_VSH}/nv2a_vsh_emulator.c
  ${NC_VSH}/nv2a_vsh_emulator_execution_state.c
  ${CMAKE_CURRENT_SOURCE_DIR}/hle/glue.c
  ${CMAKE_CURRENT_SOURCE_DIR}/hle/host_native.c
  ${CMAKE_CURRENT_SOURCE_DIR}/hle/dirty.c
)
if(ANDROID)
  target_sources(hakux_hle PRIVATE ${NC_HLE}/vk/swapchain.c)
endif()
target_include_directories(hakux_hle PUBLIC "${NC_REPO}" "${NC_DEPS}/root" "${NC_DEPS}/vma/include"
                           "${NC_DEPS}/glslang")
# xxhash (single header, FEX's copy): pb/pbdraw.c's span hashes
target_include_directories(hakux_hle PRIVATE "${HAKUX_FEX_DIR}/External/xxhash")
target_compile_definitions(hakux_hle PUBLIC CONFIG_VULKAN=1 HAKUX_HLE_GPU=1 HAKUX_NATIVE_CORE=1)
target_compile_options(hakux_hle PRIVATE -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare)
target_link_libraries(hakux_hle PUBLIC nc_glib nc_volk nc_spirv_reflect glslang SPIRV
                      glslang-default-resource-limits nc_platform)

# The MCPX APU: xemu's device (hw/xbox/mcpx/apu) ported onto the core's own
# services (native/dev/apu/apu_base.h).
set(NC_APU "${CMAKE_CURRENT_SOURCE_DIR}/dev/apu")
add_library(hakux_apu STATIC
  ${NC_APU}/apu.c
  ${NC_APU}/state.c
  ${NC_APU}/dsp/debug.c
  ${NC_APU}/dsp/dsp.c
  ${NC_APU}/dsp/dsp_cpu.c
  ${NC_APU}/dsp/dsp_dma.c
  ${NC_APU}/dsp/gp_ep.c
  ${NC_APU}/vp/resample.c
  ${NC_APU}/vp/vp.c
)
if(ANDROID)
  target_sources(hakux_apu PRIVATE ${NC_APU}/out.c)
  target_link_libraries(hakux_apu PUBLIC aaudio)
else()
  # Linux: each executable brings its output (apu_out_*), the diagnostics
  # the paced silent sink; the libretro core has its own (libretro/audio.c).
  set(NC_APU_OUT_NULL "${NC_APU}/out_null.c")
endif()
target_include_directories(hakux_apu PRIVATE "${NC_REPO}")
target_compile_options(hakux_apu PRIVATE -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare
                       -Wno-unused-variable)
target_link_libraries(hakux_apu PUBLIC hakux_hle nc_platform)
