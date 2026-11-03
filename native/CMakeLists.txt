# kantaiX native core: FEXCore fork + reimplemented Xbox kernel + SDL3 desktop UI.
#
# Linux arm64 (NC_LINUX_ARM64): builds the kantaiX executable — the standalone
# SDL3 dashboard UI. Android: hakux_native library + xboxrun, as in hakuX.
cmake_minimum_required(VERSION 3.20)
project(hakux_native C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform detection: arm64 Linux host outside the Android NDK → desktop build.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID AND
   CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(NC_LINUX_ARM64 ON)
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

if(NC_LINUX_ARM64)
  # Desktop arm64 Linux: FEX as the CPU backend, SDL3 front end, no NDK.
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux_arm64.cmake")
else()
  # Android (default hakuX path).
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/fex.cmake")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/hle.cmake")

# libslirp (NC_DEPS/libslirp, 4.7.0, unmodified): the NAT backend's TCP/IP
# stack (net/nat.c), on the GLib subset plus net/slirp/glib.h. Its public
# header is copied beside the generated version header, so the core's
# include path gets only those two.
set(NC_SLIRP "${NC_DEPS}/libslirp/src")
set(SLIRP_MAJOR_VERSION 4)
set(SLIRP_MINOR_VERSION 7)
set(SLIRP_MICRO_VERSION 0)
set(SLIRP_VERSION_STRING "\"4.7.0\"")
configure_file("${NC_SLIRP}/libslirp-version.h.in" "${CMAKE_BINARY_DIR}/slirp/libslirp-version.h" @ONLY)
configure_file("${NC_SLIRP}/libslirp.h" "${CMAKE_BINARY_DIR}/slirp/libslirp.h" COPYONLY)
add_library(nc_slirp STATIC
  net/slirp/glib.c
  "${NC_SLIRP}/arp_table.c" "${NC_SLIRP}/bootp.c" "${NC_SLIRP}/cksum.c" "${NC_SLIRP}/dhcpv6.c"
  "${NC_SLIRP}/dnssearch.c" "${NC_SLIRP}/if.c" "${NC_SLIRP}/ip6_icmp.c" "${NC_SLIRP}/ip6_input.c"
  "${NC_SLIRP}/ip6_output.c" "${NC_SLIRP}/ip_icmp.c" "${NC_SLIRP}/ip_input.c" "${NC_SLIRP}/ip_output.c"
  "${NC_SLIRP}/mbuf.c" "${NC_SLIRP}/misc.c" "${NC_SLIRP}/ncsi.c" "${NC_SLIRP}/ndp_table.c"
  "${NC_SLIRP}/sbuf.c" "${NC_SLIRP}/slirp.c" "${NC_SLIRP}/socket.c" "${NC_SLIRP}/state.c"
  "${NC_SLIRP}/stream.c" "${NC_SLIRP}/tcp_input.c" "${NC_SLIRP}/tcp_output.c" "${NC_SLIRP}/tcp_subr.c"
  "${NC_SLIRP}/tcp_timer.c" "${NC_SLIRP}/tftp.c" "${NC_SLIRP}/udp.c" "${NC_SLIRP}/udp6.c"
  "${NC_SLIRP}/util.c" "${NC_SLIRP}/version.c" "${NC_SLIRP}/vmstate.c")
target_include_directories(nc_slirp BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/net/slirp" "${NC_REPO}")
target_include_directories(nc_slirp PUBLIC "${CMAKE_BINARY_DIR}/slirp")
target_link_libraries(nc_slirp PRIVATE nc_glib)
target_compile_options(nc_slirp PRIVATE -w)
if(NC_LINUX_ARM64)
  # Our glib shim doesn't pull signal.h transitively like the real GLib does.
  target_compile_options(nc_slirp PRIVATE -include signal.h)
endif()

set(NC_NATIVE_SOURCES
  core/log.c
  core/jmp.c
  core/vm.c
  core/xdvdfs.c
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
if(NC_LINUX_ARM64)
  # SIGSEGV/SIGBUS fault handler + the system-properties / thread compat shim
  list(APPEND NC_NATIVE_SOURCES core/fault_linux_arm64.c compat/linux/linux.c)
endif()
add_library(hakux_native STATIC ${NC_NATIVE_SOURCES})
target_include_directories(hakux_native PUBLIC "${NC_REPO}")
# xxhash (single header, FEX's copy): the savestates' blob store (state/cache.c)
target_include_directories(hakux_native PRIVATE "${HAKUX_FEX_DIR}/External/xxhash")
# The HLE layer's pure modules build without QEMU in this mode.
target_compile_definitions(hakux_native PRIVATE HLE_HOST_TEST)
if(NC_LINUX_ARM64)
  target_link_libraries(hakux_native PUBLIC nc_fex hakux_hle hakux_apu nc_slirp pthread dl m ${NC_ZLIB_LIB})
  if(NC_ZLIB_INCLUDE_DIRS)
    target_include_directories(hakux_native PRIVATE ${NC_ZLIB_INCLUDE_DIRS})
  endif()
  nc_linux_compat(hakux_native hakux_hle)
else()
  target_link_libraries(hakux_native PUBLIC nc_fex hakux_hle hakux_apu nc_slirp log z)
endif()
target_compile_options(hakux_native PRIVATE -Wall -Wno-unused-parameter)

target_link_libraries(hakux_hle PUBLIC hakux_native)
target_link_libraries(nc_slirp PRIVATE hakux_native)   # nc_log

if(NC_LINUX_ARM64)
  # Desktop arm64: SDL3 dashboard executable (app/desk_main.c, cmake/app_desk.cmake).
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/app_desk.cmake")
  nc_linux_compat(hakux)
else()
  # Android: xboxrun command-line runner + Android app native library.
  # xboxrun's custom Vulkan driver ("vkdriver="): the app's code for it
  # (native/app/vkdriver.c, vklayer.c), through adrenotools when NC_DEPS has it
  # (cmake/app.cmake).
  add_executable(xboxrun run/xboxrun.c run/capture.c app/vkdriver.c app/vklayer.c)
  target_link_libraries(xboxrun PRIVATE hakux_native hakux_hle z log)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/app.cmake")
endif()
