# hakuX native core (QEMU-free): FEXCore fork + reimplemented Xbox kernel.
#
# hakux_native is the core library; xboxrun runs a title from the command
# line on the device (adb shell) for the frontier loop. Cross-compiled with
# the NDK into build/native/core; the steps are in android/README.md.
cmake_minimum_required(VERSION 3.20)
project(hakux_native C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
add_library(nc_platform INTERFACE)
target_link_libraries(nc_platform INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
if(ANDROID)
  target_link_libraries(nc_platform INTERFACE android log)
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  message(FATAL_ERROR "The native core requires Android arm64 or AArch64 Linux")
endif()
if(NOT ANDROID)
  target_compile_definitions(nc_platform INTERFACE _GNU_SOURCE)
endif()
option(HAKUX_BUILD_APP "Android: build the app's native library (hakuxcore)" ${ANDROID})
option(HAKUX_BUILD_TESTS "Build native Linux smoke tests" ON)
option(HAKUX_BUILD_LIBRETRO "Linux: build the libretro core (hakux_libretro.so), everything as PIC" OFF)
option(HAKUX_LR_END_PROCESS "libretro, PortareOS's build: a stop that hangs ends RetroArch's process, and HAKUX_LR_STOP=0 brings back ending it when the game closes (a title's relaunch then starts it again)" OFF)
set(HAKUX_VERSION "dev" CACHE STRING "The version the libretro core reports")
if(HAKUX_BUILD_LIBRETRO)
  if(ANDROID)
    message(FATAL_ERROR "The libretro core is for Linux (PortareOS)")
  endif()
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# ThinLTO for Release builds of everything but FEX (its own ENABLE_LTO stays
# off, fex.cmake): the decoder and the renderer ~2% lighter (the RP5, DOA2).
# The cache keeps relinking after a change to the files changed.
option(HAKUX_LTO "ThinLTO in Release builds (FEX excepted)" ON)
if(HAKUX_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT nc_ipo LANGUAGES C CXX)
  if(nc_ipo)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    add_link_options("-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/lto-cache")
  endif()
endif()

get_filename_component(NC_REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/fex.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/hle.cmake")

add_library(hakux_native STATIC
  core/log.c
  core/jmp.c
  core/vm.c
  core/xdvdfs.c
  core/chd.c
  core/xbe_load.c
  core/mods.c
  core/options.c
  core/cheats.c
  core/memscan.c
  core/core.c
  core/mmio.c
  core/irq.c
  dev/nv2a.c
  dev/ports.c
  dev/usb.c
  dev/usb_hub.c
  dev/xid.c
  dev/ohci.c
  dev/aci.c
  dev/nvnet.c
  net/net.c
  net/lan.c
  net/kai.c
  net/nat.c
  kernel/kernel.c
  kernel/pool.c
  kernel/ob.c
  kernel/ps.c
  kernel/ke.c
  kernel/mm.c
  kernel/paging.c
  kernel/ex.c
  kernel/rtl.c
  kernel/seh.c
  kernel/fixups.c
  kernel/x86mmio.c
  kernel/timer.c
  kernel/intr.c
  kernel/apc.c
  kernel/io.c
  kernel/xc.c
  kernel/xc_crypto.c
  kernel/kstate.c
  state/state.c
  state/vm_state.c
  state/save.c
  state/cache.c
  cpu/cpu.cpp
  "${NC_REPO}/hw/xbox/hle/core/xbe.c"
)
target_include_directories(hakux_native PUBLIC "${NC_REPO}")
# xxhash (single header, FEX's copy): the savestates' blob store (state/cache.c)
target_include_directories(hakux_native PRIVATE "${HAKUX_FEX_DIR}/External/xxhash")
# The HLE layer's pure modules build without QEMU in this mode.
target_compile_definitions(hakux_native PRIVATE HLE_HOST_TEST)
target_link_libraries(hakux_native PUBLIC nc_fex hakux_hle hakux_apu nc_platform z)
target_compile_options(hakux_native PRIVATE -Wall -Wno-unused-parameter)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/chd.cmake")

# xboxrun shares the app's custom driver loader on Android.
add_executable(xboxrun run/xboxrun.c run/capture.c app/vkdriver.c)
target_link_libraries(xboxrun PRIVATE hakux_native hakux_hle z)
if(ANDROID)
  target_sources(xboxrun PRIVATE app/vklayer.c)
else()
  target_sources(xboxrun PRIVATE "${NC_APU_OUT_NULL}")
endif()
target_link_libraries(hakux_hle PUBLIC hakux_native)
if(NOT ANDROID)
  add_executable(vulkan-probe run/vulkan_probe.c "${NC_APU_OUT_NULL}")
  target_link_libraries(vulkan-probe PRIVATE hakux_native)
  add_executable(kms-grab run/kms_grab.c)
  target_link_libraries(kms-grab PRIVATE z)
endif()

# The hakuX app's native core library (native/app, cmake/app.cmake).
if(HAKUX_BUILD_APP AND ANDROID)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/app.cmake")
endif()

# RetroArch's core (cmake/libretro.cmake).
if(HAKUX_BUILD_LIBRETRO)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/libretro.cmake")
endif()

if(HAKUX_BUILD_TESTS AND NOT ANDROID)
  enable_testing()
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux-tests.cmake")
endif()
