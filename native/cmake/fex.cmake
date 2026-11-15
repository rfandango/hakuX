# FEXCore from the FEX fork (branch hakuX), for the native core and its tests.
#
# Provides the target nc_fex: FEXCore plus FEX's host feature detection
# (taken from its front-end Common library without the rest of it), with the
# directory-scoped definitions FEX sources expect.

set(HAKUX_FEX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../external/fex" CACHE PATH "Checkout of the FEX fork (branch hakuX)")
if(NOT EXISTS "${HAKUX_FEX_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "No FEX fork at ${HAKUX_FEX_DIR}: git submodule update --init external/fex "
    "(and its submodules, android/README.md), or set HAKUX_FEX_DIR")
endif()

# FEXCore only. No thunks, tests, FEXConfig, telemetry, glibc jemalloc,
# vixl or LTO; FEX's own allocator (rpmalloc) stays on.
set(ENABLE_FEX_ALLOCATOR        ON      CACHE BOOL   "" FORCE)
set(ENABLE_JEMALLOC_GLIBC_ALLOC OFF     CACHE BOOL   "" FORCE)
set(ENABLE_OFFLINE_TELEMETRY    OFF     CACHE BOOL   "" FORCE)
set(BUILD_FEXCONFIG             OFF     CACHE BOOL   "" FORCE)
set(ENABLE_GDB_SYMBOLS          OFF     CACHE BOOL   "" FORCE)
set(ENABLE_LTO                  OFF     CACHE BOOL   "" FORCE)
set(ENABLE_CCACHE               OFF     CACHE BOOL   "" FORCE)
set(BUILD_THUNKS                OFF     CACHE BOOL   "" FORCE)
set(BUILD_TESTING               OFF     CACHE BOOL   "" FORCE)
set(ENABLE_VIXL_DISASSEMBLER    OFF     CACHE BOOL   "" FORCE)
set(ENABLE_VIXL_SIMULATOR       OFF     CACHE BOOL   "" FORCE)
set(BUILD_FEX_LINUX_TESTS       OFF     CACHE BOOL   "" FORCE)
set(ENABLE_ASSERTIONS           OFF     CACHE BOOL   "" FORCE)
set(TUNE_CPU                    none    CACHE STRING "" FORCE)

# HAKUX_ARCH: the CPUs the build may assume, for FEX (its TUNE_ARCH) and the
# core alike, e.g. armv8.2-a+fp16+dotprod+rcpc (the Snapdragon 865 up: the
# RP5, the Odin 3). Empty: FEX's generic armv8-a+crc, the core's NDK default,
# which runs anywhere but calls out for every atomic read-modify-write.
set(HAKUX_ARCH "" CACHE STRING "-march for FEX and the core (empty: generic)")
if(HAKUX_ARCH)
  set(TUNE_ARCH "${HAKUX_ARCH}" CACHE STRING "" FORCE)
  add_compile_options("-march=${HAKUX_ARCH}")
else()
  set(TUNE_ARCH generic CACHE STRING "" FORCE)
endif()

if(NOT TARGET FEXCore)
  add_subdirectory("${HAKUX_FEX_DIR}" "${CMAKE_BINARY_DIR}/fex-build" EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET nc_fex)
  add_library(nc_fex STATIC
    "${HAKUX_FEX_DIR}/Source/Common/HostFeatures.cpp"
    "${HAKUX_FEX_DIR}/Source/Common/CPUInfo.cpp"
    "${HAKUX_FEX_DIR}/Source/Common/Linux/LinuxVersion.cpp"
  )
  target_compile_features(nc_fex PUBLIC cxx_std_20)
  target_include_directories(nc_fex PUBLIC
    "${HAKUX_FEX_DIR}/Source"
    "${CMAKE_BINARY_DIR}/fex-build/Source"
  )
  # FEX sets these per directory (add_compile_definitions), so sources built
  # outside its tree need them repeated. preserve_all as FEX found it
  # (HAS_CLANG_PRESERVE_ALL): HostFeatures.cpp tells the JIT, which then
  # keeps guest registers in x9-x15 across calls to FEXCore's helpers; the
  # NDK's clang has it, Debian 12's clang 16 doesn't on aarch64, and a JIT
  # told otherwise lost them to every x87 fsin (Halo 2's stick code).
  target_compile_definitions(nc_fex PUBLIC ARCHITECTURE_arm64=1 _LARGEFILE64_SOURCE)
  if(HAS_CLANG_PRESERVE_ALL)
    target_compile_definitions(nc_fex PUBLIC
      "FEX_PRESERVE_ALL_ATTR=__attribute__((preserve_all))" FEX_HAS_PRESERVE_ALL_ATTR=1)
  else()
    target_compile_definitions(nc_fex PUBLIC "FEX_PRESERVE_ALL_ATTR=" FEX_HAS_PRESERVE_ALL_ATTR=0)
  endif()
  target_link_libraries(nc_fex PUBLIC FEXCore FEXCore_Base JemallocLibs FEXHeaderUtils range-v3::range-v3 nc_host)
endif()
