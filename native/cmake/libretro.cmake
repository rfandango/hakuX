# The libretro core (native/libretro): hakux_libretro.so,
# for RetroArch. Everything linked in stays hidden: it exports the libretro
# ABI (retro_*) only, and needs no SDL. Every static library in it is PIC
# (HAKUX_BUILD_LIBRETRO sets CMAKE_POSITION_INDEPENDENT_CODE; FEX does its own).
add_library(hakux_libretro SHARED
  "${CMAKE_CURRENT_SOURCE_DIR}/libretro/core.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/libretro/audio.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/libretro/input.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/libretro/video.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/libretro/vulkan.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/libretro/options.c"
)
target_link_libraries(hakux_libretro PRIVATE hakux_native hakux_hle)
# libretro_vulkan.h, as RetroArch ships it, includes <libretro.h>
target_include_directories(hakux_libretro PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/libretro")
target_compile_definitions(hakux_libretro PRIVATE "HAKUX_VERSION=\"${HAKUX_VERSION}\""
  "HAKUX_LR_END_PROCESS=$<BOOL:${HAKUX_LR_END_PROCESS}>")
target_compile_options(hakux_libretro PRIVATE -Wall -Wno-unused-parameter)
set_target_properties(hakux_libretro PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
set(NC_LR_EXPORTS "${CMAKE_CURRENT_SOURCE_DIR}/libretro/exports.map")
target_link_options(hakux_libretro PRIVATE -Wl,--exclude-libs,ALL -Wl,--gc-sections -Wl,-z,defs
  "-Wl,--version-script=${NC_LR_EXPORTS}")
set_property(TARGET hakux_libretro APPEND PROPERTY LINK_DEPENDS "${NC_LR_EXPORTS}")
