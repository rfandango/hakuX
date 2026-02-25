cmake_minimum_required(VERSION 3.30)
project(xemu_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJECT_PATH_MAX 180)

include(FetchContent)
include(ExternalProject)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
find_program(PYTHON_EXECUTABLE python)
if(NOT PYTHON_EXECUTABLE)
  message(FATAL_ERROR "python not found in PATH; required for QAPI/trace generation")
endif()

option(XEMU_ENABLE_VULKAN "Enable Vulkan renderer" ON)
option(XEMU_ENABLE_XISO_CONVERTER "Enable ISO->XISO converter library" ON)
option(XEMU_OPT_NATIVE_FLOAT "Enable native ARM64 float operations for SSE/FPU" OFF)
option(XEMU_OPT_TCG_PEEPHOLE "Enable TCG optimizer setcond+brcond peephole fusion" OFF)
option(XEMU_OPT_TB_CACHE_HINTS "Enable persistent TB cache hints (pre-warm on load)" OFF)

find_program(CARGO_EXECUTABLE NAMES cargo cargo.exe)
if(CARGO_EXECUTABLE MATCHES "-NOTFOUND$" OR NOT CARGO_EXECUTABLE)
  if(DEFINED ENV{USERPROFILE})
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}/.cargo/bin/cargo.exe" CARGO_USERPROFILE_BIN)
    if(EXISTS "${CARGO_USERPROFILE_BIN}")
      set(CARGO_EXECUTABLE "${CARGO_USERPROFILE_BIN}" CACHE FILEPATH "Path to cargo executable" FORCE)
    endif()
  endif()
endif()
set(XEMU_XISO_CONVERTER_BUILD_ENABLED ${XEMU_ENABLE_XISO_CONVERTER})
if(XEMU_XISO_CONVERTER_BUILD_ENABLED AND NOT CARGO_EXECUTABLE)
  message(WARNING "cargo not found in PATH; disabling XISO converter")
  set(XEMU_XISO_CONVERTER_BUILD_ENABLED OFF)
endif()

# --- SDL2 ---
option(XEMU_FETCH_SDL2 "Download and build SDL2 for Android" ON)
set(SDL2_VERSION "2.32.10")
set(SDL2_LOCAL_DEFAULT "${REPO_ROOT}/thirdparty/SDL2")
if(NOT DEFINED SDL2_LOCAL_DIR AND EXISTS "${SDL2_LOCAL_DEFAULT}/CMakeLists.txt")
  set(SDL2_LOCAL_DIR "${SDL2_LOCAL_DEFAULT}")
endif()

if(DEFINED SDL2_LOCAL_DIR)
  message(STATUS "Using local SDL2 at ${SDL2_LOCAL_DIR}")
  add_subdirectory("${SDL2_LOCAL_DIR}" "${CMAKE_BINARY_DIR}/sdl2-local")
elseif(XEMU_FETCH_SDL2)
  set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_RENDER_D3D OFF CACHE BOOL "" FORCE)
  set(SDL_RENDER_D3D11 OFF CACHE BOOL "" FORCE)
  set(SDL_RENDER_D3D12 OFF CACHE BOOL "" FORCE)
  set(SDL_RENDER_METAL OFF CACHE BOOL "" FORCE)
  set(SDL_RENDER_PS2 OFF CACHE BOOL "" FORCE)
  set(SDL_VIDEO_OPENGL OFF CACHE BOOL "" FORCE)
  set(SDL_VIDEO_VULKAN ON CACHE BOOL "" FORCE)
  set(SDL_VIDEO_OPENGL_ES ON CACHE BOOL "" FORCE)
  set(SDL_VIDEO_OPENGL_ES2 ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    SDL2
    URL "https://github.com/libsdl-org/SDL/archive/refs/tags/release-${SDL2_VERSION}.zip"
  )
  FetchContent_MakeAvailable(SDL2)
endif()

# --- toml++ ---
set(TOMLPLUSPLUS_GIT_REV "30172438cee64926dc41fdd9c11fb3ba5b2ba9de")
FetchContent_Declare(
  tomlplusplus
  GIT_REPOSITORY "https://github.com/marzer/tomlplusplus.git"
  GIT_TAG "${TOMLPLUSPLUS_GIT_REV}"
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

if(XEMU_ENABLE_VULKAN)
  # --- adrenotools (custom GPU driver loading on Adreno) ---
  FetchContent_Declare(
    adrenotools
    GIT_REPOSITORY "https://github.com/bylaws/libadrenotools"
    GIT_TAG "master"
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(adrenotools)

  # --- Vulkan deps (volk, glslang, SPIRV-Reflect, VMA) ---
  set(VOLK_GIT_REV "0b17a763ba5643e32da1b2152f8140461b3b7345")
  FetchContent_Declare(
    volk
    GIT_REPOSITORY "https://github.com/zeux/volk"
    GIT_TAG "${VOLK_GIT_REV}"
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(volk)

  add_library(volk_static STATIC "${volk_SOURCE_DIR}/volk.c")
  target_include_directories(volk_static PUBLIC "${volk_SOURCE_DIR}")
  target_compile_definitions(volk_static PUBLIC VK_NO_PROTOTYPES)

  set(GLSLANG_GIT_REV "b5782e52ee2f7b3e40bb9c80d15b47016e008bc9")
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
  set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
  set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
  set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
  set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    glslang
    GIT_REPOSITORY "https://github.com/KhronosGroup/glslang"
    GIT_TAG "${GLSLANG_GIT_REV}"
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(glslang)

  set(SPIRV_REFLECT_GIT_REV "ef913b3ab3da1becca3cf46b15a10667c67bebe5")
  FetchContent_Declare(
    spirv_reflect
    GIT_REPOSITORY "https://github.com/KhronosGroup/SPIRV-Reflect"
    GIT_TAG "${SPIRV_REFLECT_GIT_REV}"
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(spirv_reflect)

  add_library(spirv_reflect_static STATIC
    "${spirv_reflect_SOURCE_DIR}/spirv_reflect.c"
  )
  target_include_directories(spirv_reflect_static PUBLIC
    "${spirv_reflect_SOURCE_DIR}"
  )

  set(VMA_GIT_REV "1d8f600fd424278486eade7ed3e877c99f0846b1")
  FetchContent_Declare(
    vma
    GIT_REPOSITORY "https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator"
    GIT_TAG "${VMA_GIT_REV}"
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(vma)
endif()

# --- GLib (for QEMU core) ---
find_program(MESON_EXECUTABLE meson)
find_program(NINJA_EXECUTABLE ninja)

if(NOT MESON_EXECUTABLE)
  message(FATAL_ERROR "meson not found in PATH; required to build glib for Android")
endif()

if(NOT NINJA_EXECUTABLE)
  message(FATAL_ERROR "ninja not found in PATH; required to build glib for Android")
endif()

if(CMAKE_ANDROID_NDK)
  set(NDK_PATH "${CMAKE_ANDROID_NDK}")
elseif(ANDROID_NDK)
  set(NDK_PATH "${ANDROID_NDK}")
else()
  set(NDK_PATH "")
endif()

if(NDK_PATH STREQUAL "")
  message(FATAL_ERROR "Android NDK path not found in CMAKE_ANDROID_NDK/ANDROID_NDK")
endif()

if(DEFINED ANDROID_PLATFORM)
  string(REPLACE "android-" "" ANDROID_API "${ANDROID_PLATFORM}")
elseif(DEFINED CMAKE_SYSTEM_VERSION)
  set(ANDROID_API "${CMAKE_SYSTEM_VERSION}")
else()
  set(ANDROID_API "26")
endif()

if(NOT ANDROID_API)
  set(ANDROID_API "26")
endif()

set(ANDROID_TRIPLE "aarch64-none-linux-android${ANDROID_API}")
set(NDK_TOOLCHAIN "${NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64/bin")

if(XEMU_XISO_CONVERTER_BUILD_ENABLED)
  if(CMAKE_HOST_WIN32)
    if(NOT DEFINED XISO_CONVERTER_RUSTUP_TOOLCHAIN OR
       XISO_CONVERTER_RUSTUP_TOOLCHAIN STREQUAL "")
      set(XISO_CONVERTER_RUSTUP_TOOLCHAIN
        "stable-x86_64-unknown-linux-gnu"
        CACHE STRING "Rustup host toolchain for ISO->XISO converter"
        FORCE
      )
    else()
      set(XISO_CONVERTER_RUSTUP_TOOLCHAIN
        "${XISO_CONVERTER_RUSTUP_TOOLCHAIN}"
        CACHE STRING "Rustup host toolchain for ISO->XISO converter"
      )
    endif()
  else()
    set(XISO_CONVERTER_RUSTUP_TOOLCHAIN
      ""
      CACHE STRING "Rustup host toolchain for ISO->XISO converter"
    )
  endif()

  set(XISO_CONVERTER_CARGO_ENV_VARS "")
  if(DEFINED XISO_CONVERTER_RUSTUP_TOOLCHAIN AND
     NOT XISO_CONVERTER_RUSTUP_TOOLCHAIN STREQUAL "")
    list(APPEND XISO_CONVERTER_CARGO_ENV_VARS
      "RUSTUP_TOOLCHAIN=${XISO_CONVERTER_RUSTUP_TOOLCHAIN}"
    )
  endif()

  set(XISO_CONVERTER_RS_DIR "${REPO_ROOT}/android/rust/xiso_converter")
  set(XISO_CONVERTER_RS_TARGET "aarch64-linux-android")
  set(XISO_CONVERTER_RS_TARGET_DIR "${CMAKE_BINARY_DIR}/rust-target")
  set(XISO_CONVERTER_RS_LIB
    "${XISO_CONVERTER_RS_TARGET_DIR}/${XISO_CONVERTER_RS_TARGET}/release/libxiso_converter_rs.a"
  )
  set(XISO_CONVERTER_RS_LINKER
    "${NDK_TOOLCHAIN}/aarch64-linux-android${ANDROID_API}-clang"
  )
  set(XISO_CONVERTER_RS_AR "${NDK_TOOLCHAIN}/llvm-ar")

  file(GLOB_RECURSE XISO_CONVERTER_RS_SOURCES CONFIGURE_DEPENDS
    "${XISO_CONVERTER_RS_DIR}/src/*.rs"
    "${XISO_CONVERTER_RS_DIR}/Cargo.toml"
    "${REPO_ROOT}/android/third_party/xdvdfs-core/src/*.rs"
    "${REPO_ROOT}/android/third_party/xdvdfs-core/Cargo.toml"
  )

  add_custom_command(
    OUTPUT "${XISO_CONVERTER_RS_LIB}"
    COMMAND "${CMAKE_COMMAND}" -E env
      "CARGO_TARGET_DIR=${XISO_CONVERTER_RS_TARGET_DIR}"
      "CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=${XISO_CONVERTER_RS_LINKER}"
      "CARGO_TARGET_AARCH64_LINUX_ANDROID_AR=${XISO_CONVERTER_RS_AR}"
      ${XISO_CONVERTER_CARGO_ENV_VARS}
      "${CARGO_EXECUTABLE}" build
      --manifest-path "${XISO_CONVERTER_RS_DIR}/Cargo.toml"
      --release
      --target "${XISO_CONVERTER_RS_TARGET}"
    DEPENDS ${XISO_CONVERTER_RS_SOURCES}
    WORKING_DIRECTORY "${XISO_CONVERTER_RS_DIR}"
    COMMENT "Building Rust ISO->XISO converter"
    VERBATIM
  )

  add_custom_target(xiso_converter_rs_build ALL
    DEPENDS "${XISO_CONVERTER_RS_LIB}"
  )

  add_library(xiso_converter_rs STATIC IMPORTED GLOBAL)
  set_target_properties(xiso_converter_rs PROPERTIES
    IMPORTED_LOCATION "${XISO_CONVERTER_RS_LIB}"
  )
  add_dependencies(xiso_converter_rs xiso_converter_rs_build)
endif()

set(GLIB_VERSION "2.66.8")
set(GLIB_SOURCE_DIR "${CMAKE_BINARY_DIR}/glib-src")
set(GLIB_BUILD_DIR "${CMAKE_BINARY_DIR}/glib-build-android")
set(GLIB_INSTALL_DIR "${CMAKE_BINARY_DIR}/glib-install-android")
set(GLIB_CROSS_FILE "${CMAKE_BINARY_DIR}/glib-android-cross.txt")
set(ICONV_STUB_DIR "${CMAKE_BINARY_DIR}/iconv-stub")

add_library(iconv_stub STATIC
  "${CMAKE_CURRENT_LIST_DIR}/iconv_stub/iconv_stub.c"
)
set_target_properties(iconv_stub PROPERTIES
  OUTPUT_NAME "iconv"
  ARCHIVE_OUTPUT_DIRECTORY "${ICONV_STUB_DIR}"
)
target_include_directories(iconv_stub PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/iconv_stub"
)

file(WRITE "${GLIB_CROSS_FILE}"
"[host_machine]\n"
"system = 'android'\n"
"cpu_family = 'aarch64'\n"
"cpu = 'aarch64'\n"
"endian = 'little'\n"
"\n"
"[binaries]\n"
"c = '${NDK_TOOLCHAIN}/clang'\n"
"cpp = '${NDK_TOOLCHAIN}/clang++'\n"
"ar = '${NDK_TOOLCHAIN}/llvm-ar'\n"
"strip = '${NDK_TOOLCHAIN}/llvm-strip'\n"
"\n"
"[properties]\n"
"needs_exe_wrapper = true\n"
"sys_root = '${NDK_PATH}/toolchains/llvm/prebuilt/linux-x86_64/sysroot'\n"
"c_args = ['--target=${ANDROID_TRIPLE}', '-I${CMAKE_CURRENT_LIST_DIR}/iconv_stub']\n"
"cpp_args = ['--target=${ANDROID_TRIPLE}', '-I${CMAKE_CURRENT_LIST_DIR}/iconv_stub']\n"
"c_link_args = ['--target=${ANDROID_TRIPLE}', '-L${ICONV_STUB_DIR}', '-liconv']\n"
"cpp_link_args = ['--target=${ANDROID_TRIPLE}', '-L${ICONV_STUB_DIR}', '-liconv']\n"
)

ExternalProject_Add(glib_ep
  URL "https://download.gnome.org/sources/glib/2.66/glib-${GLIB_VERSION}.tar.xz"
  URL_HASH "SHA256=97BC87DD91365589AF5CBBFEA2574833AEA7A1B71840FD365ECD2852C76B9C8B"
  DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads"
  SOURCE_DIR "${GLIB_SOURCE_DIR}"
  BINARY_DIR "${GLIB_BUILD_DIR}"
  PATCH_COMMAND
    "${CMAKE_COMMAND}" -DGLIB_SOURCE_DIR="${GLIB_SOURCE_DIR}" -P "${CMAKE_CURRENT_LIST_DIR}/patch_glib.cmake"
  CONFIGURE_COMMAND
    "${MESON_EXECUTABLE}" setup "${GLIB_BUILD_DIR}" "${GLIB_SOURCE_DIR}"
    --cross-file "${GLIB_CROSS_FILE}"
    --prefix "${GLIB_INSTALL_DIR}"
    --buildtype "release"
    --default-library "static"
    -Dgtk_doc=false
    -Dman=false
    -Diconv=external
    -Dlibmount=disabled
    -Dselinux=disabled
    -Dxattr=false
    -Dnls=disabled
    -Dinternal_pcre=true
    -Dinstalled_tests=false
  BUILD_COMMAND "${NINJA_EXECUTABLE}" -C "${GLIB_BUILD_DIR}" -j 1
  INSTALL_COMMAND "${MESON_EXECUTABLE}" install -C "${GLIB_BUILD_DIR}"
  BUILD_BYPRODUCTS
    "${GLIB_INSTALL_DIR}/lib/libglib-2.0.a"
    "${GLIB_INSTALL_DIR}/lib/libgobject-2.0.a"
    "${GLIB_INSTALL_DIR}/lib/libgio-2.0.a"
    "${GLIB_INSTALL_DIR}/lib/libgmodule-2.0.a"
    "${GLIB_INSTALL_DIR}/lib/libgthread-2.0.a"
)

add_dependencies(glib_ep iconv_stub)

set(XEMU_CORE_DIRS
  accel
  audio
  backends
  block
  chardev
  common-user
  crypto
  disas
  fpu
  fsdev
  gdbstub
  host
  hw
  io
  libdecnumber
  migration
  monitor
  net
  qapi
  qobject
  qom
  replay
  scsi
  semihosting
  stats
  stubs
  system
  target
  tcg
  ui
  util
)

set(XEMU_CORE_SOURCES "")
foreach(dir ${XEMU_CORE_DIRS})
  file(GLOB_RECURSE DIR_SOURCES CONFIGURE_DEPENDS
    "${REPO_ROOT}/${dir}/*.c"
    "${REPO_ROOT}/${dir}/*.cc"
    "${REPO_ROOT}/${dir}/*.cpp"
  )
  list(APPEND XEMU_CORE_SOURCES ${DIR_SOURCES})
endforeach()

file(GLOB ROOT_SOURCES CONFIGURE_DEPENDS
  "${REPO_ROOT}/*.c"
  "${REPO_ROOT}/*.cc"
  "${REPO_ROOT}/*.cpp"
)
list(APPEND XEMU_CORE_SOURCES ${ROOT_SOURCES})

list(REMOVE_DUPLICATES XEMU_CORE_SOURCES)

# Remove Windows-only sources for Android builds.
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "chardev[\\\\/]char-win.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "chardev[\\\\/]char-console\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "chardev[\\\\/]baum\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "chardev[\\\\/]char-pty\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "chardev[\\\\/]spice\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "os-win32\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "os-wasm\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "accel[\\\\/](hvf|mshv|xen|qtest|kvm)[\\\\/]")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "accel[\\\\/]tcg[\\\\/]plugin-gen\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "accel[\\\\/]tcg[\\\\/]user-exec\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "accel[\\\\/]tcg[\\\\/]user-exec-stub\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "audio[\\\\/](alsaaudio|dbusaudio|dsoundaudio|jackaudio|ossaudio|paaudio|pwaudio|sdlaudio|sndioaudio|spiceaudio|audio_win_int|wavaudio)\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "backends[\\\\/]cryptodev-.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "backends[\\\\/]igvm.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "backends[\\\\/]tpm[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "crypto[\\\\/].*afalg.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "crypto[\\\\/].*gcrypt.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "crypto[\\\\/].*gnutls.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "crypto[\\\\/].*nettle.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "crypto[\\\\/]random-.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "crypto[\\\\/]tls-cipher-suites\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "crypto[\\\\/]tls.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "crypto[\\\\/]x509.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]blkio\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]curl\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]dmg-bz2\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]dmg-lzfse\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]nbd\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]export[\\\\/]fuse\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]export[\\\\/]vduse-blk\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]export[\\\\/]vhost-user-blk-server\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]file-win32\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]gluster\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]io_uring\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]iscsi-opts\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]iscsi\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]linux-aio\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]nfs\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]rbd\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]replication\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]ssh\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "block[\\\\/]win32-aio\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "blockdev-nbd\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "qemu-keymap\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "replication\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "disas[\\\\/]capstone\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "fsdev[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "gdbstub[\\\\/]user-target\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "gdbstub[\\\\/]user\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]xemu-settings\\.cc$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]xui[\\\\/].*")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]9pfs[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]acpi[\\\\/]tpm\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]acpi[\\\\/]acpi-stub\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]acpi[\\\\/]aml-build-stub\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]core[\\\\/]eif\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]core[\\\\/]loader-fit\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]core[\\\\/]sysbus-fdt\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]cpu[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]display[\\\\/]qxl.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]display[\\\\/]virtio-gpu.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]display[\\\\/]virtio-dmabuf\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]display[\\\\/]vhost-user-gpu.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]display[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]hyperv[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]input[\\\\/]virtio-.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]i386[\\\\/]kvm[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]i386[\\\\/]xen[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]intc[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]ipack[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]ipmi[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]misc[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]pci-host[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]cxl[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]net[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]nubus[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]nvme[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]nvram[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]remote[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]sensor[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]smbios[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]tpm[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]rtc[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]ssi[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]scsi[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]sd[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]timer[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]ufs[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]usb[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]uefi[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]vfio[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]vfio-user[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]virtio[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]watchdog[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]xbox[\\\\/]chihiro.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]xbox[\\\\/]nv2a[\\\\/]pgraph[\\\\/]gl[\\\\/]gpuprops\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]xbox[\\\\/]nv2a[\\\\/]pgraph[\\\\/]vk[\\\\/]gpuprops\\.c$")
if(NOT XEMU_ENABLE_VULKAN)
  list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]xbox[\\\\/]nv2a[\\\\/]pgraph[\\\\/]vk[\\\\/].*\\.c$")
endif()
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "migration[\\\\/]colo.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "migration[\\\\/]multifd-(qatzip|qpl|uadk|zstd)\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "migration[\\\\/]rdma\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]cocoa\\.m$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]curses\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]dbus.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]egl-helpers\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]egl-headless\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]egl-context\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]gtk.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]sdl2.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]console-gl\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]console-vc\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]shader\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]vnc.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]xemu-snapshots\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]xemu-thumbnail\\.cc$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]win32-kbd-hook\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]input-keymap\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]thirdparty[\\\\/]noc_file_dialog[\\\\/].*")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]x_keymap\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]vdagent\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]xemu-os-utils-windows\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]xemu-net\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]aio-win32\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]atomic64\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]coroutine-ucontext\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]coroutine-wasm\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]coroutine-win32\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]coroutine-windows\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]cpuinfo-loongarch\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]cpuinfo-ppc\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]cpuinfo-riscv\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]fast-hash\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]event_notifier-win32\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]fdmon-epoll\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]fdmon-io_uring\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]http\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]oslib-win32\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]qemu-thread-win32\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]s390x_pci_mmio\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "util[\\\\/]sys_membarrier\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "monitor[\\\\/]hmp-target\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "system[\\\\/]async-teardown\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "system[\\\\/]device_tree\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "system[\\\\/]qemu-seccomp\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "system[\\\\/]tpm\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "stubs[\\\\/].*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]acpi[\\\\/]ghes-stub\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]mem[\\\\/]cxl.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]mem[\\\\/]memory-device-stubs\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "net[\\\\/]af-xdp\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "net[\\\\/]netmap\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "net[\\\\/]pcap\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "net[\\\\/]passt\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "net[\\\\/]slirp\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "net[\\\\/]tap-bsd\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "net[\\\\/]tap-solaris\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "net[\\\\/]tap-win32\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "net[\\\\/]tap\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "net[\\\\/]vde\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "net[\\\\/]vmnet.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]virtio[\\\\/]cbor-helpers\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]virtio[\\\\/]virtio-nsm.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]net[\\\\/]spapr_.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]intc[\\\\/]pnv_.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]pci-host[\\\\/]pnv_.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]ssi[\\\\/]pnv_.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]nvram[\\\\/]spapr_.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]scsi[\\\\/]spapr_.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/](alpha|arm|avr|hppa|loongarch|m68k|microblaze|mips|openrisc|ppc|riscv|rx|s390x|sh4|sparc|sparc64|tricore|xtensa|vmapple|xen|xenpv)[\\\\/]")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "target[\\\\/](alpha|arm|avr|hexagon|hppa|loongarch|m68k|microblaze|mips|openrisc|ppc|riscv|rx|s390x|sh4|sparc|tricore|xtensa)[\\\\/]")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "target[\\\\/]i386[\\\\/]emulate[\\\\/].*")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "target[\\\\/]i386[\\\\/]hvf[\\\\/].*")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "target[\\\\/]i386[\\\\/]kvm[\\\\/].*")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "target[\\\\/]i386[\\\\/]tcg[\\\\/]user[\\\\/].*")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "target[\\\\/]i386[\\\\/]mshv[\\\\/].*")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "target[\\\\/]i386[\\\\/]nvmm[\\\\/].*")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "target[\\\\/]i386[\\\\/]sev\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "target[\\\\/]i386[\\\\/]whpx[\\\\/].*")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "tcg[\\\\/]debuginfo\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "tcg[\\\\/]tci\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "ui[\\\\/]spice.*\\.c$")
list(FILTER XEMU_CORE_SOURCES EXCLUDE REGEX "hw[\\\\/]char[\\\\/].*\\.c$")
list(APPEND XEMU_CORE_SOURCES
  "${REPO_ROOT}/accel/stubs/kvm-stub.c"
  "${REPO_ROOT}/crypto/tlscreds.c"
  "${REPO_ROOT}/crypto/tlssession.c"
  "${REPO_ROOT}/migration/colo-stubs.c"
  "${REPO_ROOT}/stubs/gdbstub.c"
  "${REPO_ROOT}/stubs/target-get-monitor-def.c"
  "${REPO_ROOT}/stubs/target-monitor-defs.c"
  "${REPO_ROOT}/stubs/xen-hw-stub.c"
  "${REPO_ROOT}/trace/control.c"
  "${REPO_ROOT}/trace/control-target.c"
  "${REPO_ROOT}/hw/i386/kvm/xen-stubs.c"
  "${REPO_ROOT}/hw/virtio/virtio-md-stubs.c"
  "${REPO_ROOT}/hw/virtio/vhost-stub.c"
  "${REPO_ROOT}/hw/cxl/cxl-host-stubs.c"
  "${REPO_ROOT}/hw/intc/kvm_irqcount.c"
  "${REPO_ROOT}/hw/smbios/smbios-stub.c"
  "${REPO_ROOT}/hw/char/debugcon.c"
  "${REPO_ROOT}/hw/char/parallel.c"
  "${REPO_ROOT}/hw/char/parallel-isa.c"
  "${REPO_ROOT}/hw/char/serial.c"
  "${REPO_ROOT}/hw/char/serial-isa.c"
  "${REPO_ROOT}/hw/char/serial-mm.c"
  "${REPO_ROOT}/hw/char/serial-pci.c"
  "${REPO_ROOT}/hw/char/serial-pci-multi.c"
  "${REPO_ROOT}/hw/display/bochs-display.c"
  "${REPO_ROOT}/hw/display/cirrus_vga.c"
  "${REPO_ROOT}/hw/display/cirrus_vga_isa.c"
  "${REPO_ROOT}/hw/display/vga.c"
  "${REPO_ROOT}/hw/display/vga-isa.c"
  "${REPO_ROOT}/hw/display/vga-mmio.c"
  "${REPO_ROOT}/hw/display/vga-pci.c"
  "${REPO_ROOT}/hw/intc/apic.c"
  "${REPO_ROOT}/hw/intc/apic_common.c"
  "${REPO_ROOT}/hw/intc/i8259.c"
  "${REPO_ROOT}/hw/intc/i8259_common.c"
  "${REPO_ROOT}/hw/intc/intc.c"
  "${REPO_ROOT}/hw/intc/ioapic.c"
  "${REPO_ROOT}/hw/intc/ioapic_common.c"
  "${REPO_ROOT}/hw/intc/ioapic-stub.c"
  "${REPO_ROOT}/hw/pci-host/pam.c"
  "${REPO_ROOT}/hw/rtc/mc146818rtc.c"
  "${REPO_ROOT}/hw/timer/i8254.c"
  "${REPO_ROOT}/hw/timer/i8254_common.c"
  "${REPO_ROOT}/hw/usb/bus.c"
  "${REPO_ROOT}/hw/usb/combined-packet.c"
  "${REPO_ROOT}/hw/usb/core.c"
  "${REPO_ROOT}/hw/usb/desc.c"
  "${REPO_ROOT}/hw/usb/desc-msos.c"
  "${REPO_ROOT}/hw/usb/dev-hub.c"
  "${REPO_ROOT}/hw/usb/hcd-ohci.c"
  "${REPO_ROOT}/hw/usb/hcd-ohci-pci.c"
  "${REPO_ROOT}/hw/usb/libhw.c"
  "${REPO_ROOT}/hw/usb/pcap.c"
  "${REPO_ROOT}/hw/misc/pci-testdev.c"
  "${REPO_ROOT}/hw/nvram/fw_cfg.c"
  "${REPO_ROOT}/hw/nvram/fw_cfg-interface.c"
  "${REPO_ROOT}/hw/nvram/fw_cfg-acpi.c"
  "${REPO_ROOT}/hw/vfio/iommufd-stubs.c"
  "${REPO_ROOT}/ui/spice-module.c"
  "${REPO_ROOT}/ui/vnc-stubs.c"
  "${REPO_ROOT}/target/i386/kvm/hyperv-stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/kvmclock_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/nv2a_vsh_emulator_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/samplerate_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/fast_hash_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/libintl_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/qmp_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/monitor_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/net_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/dump_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/nbd_server_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/qemu_input_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/watchdog_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/async_teardown_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/block_export_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/xemu_net_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/xemu_notifications_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/xemu_snapshots_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/xemu_hud_stub.c"
  "${CMAKE_CURRENT_LIST_DIR}/xemu_os_utils_android.c"
  "${CMAKE_CURRENT_LIST_DIR}/qcrypto_random_android.c"
  "${CMAKE_CURRENT_LIST_DIR}/xemu_settings_android.cc"
)

# --- QAPI/Trace generation ---
set(QAPI_GEN_DIR "${CMAKE_BINARY_DIR}/qapi")
file(MAKE_DIRECTORY "${QAPI_GEN_DIR}")

execute_process(
  COMMAND "${PYTHON_EXECUTABLE}" "${REPO_ROOT}/scripts/qapi-gen.py"
          -o "${QAPI_GEN_DIR}"
          -b "${REPO_ROOT}/qapi/qapi-schema.json"
  WORKING_DIRECTORY "${REPO_ROOT}"
  RESULT_VARIABLE QAPI_GEN_RESULT
)
if(NOT QAPI_GEN_RESULT EQUAL 0)
  message(FATAL_ERROR "QAPI generation failed with code ${QAPI_GEN_RESULT}")
endif()

file(GLOB_RECURSE QAPI_GEN_SOURCES "${QAPI_GEN_DIR}/*.c")
list(APPEND XEMU_CORE_SOURCES ${QAPI_GEN_SOURCES})

# Generate block coroutine wrapper sources (block-gen.c).
set(BLOCK_GEN_C "${CMAKE_BINARY_DIR}/block-gen.c")
execute_process(
  COMMAND "${PYTHON_EXECUTABLE}" "${REPO_ROOT}/scripts/block-coroutine-wrapper.py"
          "${BLOCK_GEN_C}"
          "${REPO_ROOT}/include/block/block-io.h"
          "${REPO_ROOT}/include/block/dirty-bitmap.h"
          "${REPO_ROOT}/include/block/block_int-io.h"
          "${REPO_ROOT}/include/block/block-global-state.h"
          "${REPO_ROOT}/include/system/block-backend-global-state.h"
          "${REPO_ROOT}/include/system/block-backend-io.h"
          "${REPO_ROOT}/block/coroutines.h"
  WORKING_DIRECTORY "${REPO_ROOT}"
  RESULT_VARIABLE BLOCK_GEN_RESULT
)
if(NOT BLOCK_GEN_RESULT EQUAL 0)
  message(FATAL_ERROR "block-coroutine-wrapper.py failed with code ${BLOCK_GEN_RESULT}")
endif()
list(APPEND XEMU_CORE_SOURCES "${BLOCK_GEN_C}")

# Generate stub trace headers for QAPI-generated trace-events files.
set(QAPI_TRACE_GEN_DIR "${CMAKE_BINARY_DIR}/trace")
file(MAKE_DIRECTORY "${QAPI_TRACE_GEN_DIR}")
file(GLOB QAPI_TRACE_EVENT_FILES "${QAPI_GEN_DIR}/*.trace-events")
foreach(TRACE_FILE ${QAPI_TRACE_EVENT_FILES})
  get_filename_component(TRACE_BASENAME "${TRACE_FILE}" NAME)
  string(REGEX REPLACE "[^A-Za-z0-9]" "_" TRACE_GROUP "${TRACE_BASENAME}")
  set(TRACE_OUT "${QAPI_TRACE_GEN_DIR}/trace-${TRACE_GROUP}.h")
  execute_process(
    COMMAND "${PYTHON_EXECUTABLE}" "${REPO_ROOT}/scripts/tracetool.py"
            --format=h --backends=nop --group "${TRACE_GROUP}"
            "${TRACE_FILE}" "${TRACE_OUT}"
    WORKING_DIRECTORY "${REPO_ROOT}"
    RESULT_VARIABLE TRACE_QAPI_RESULT
  )
  if(NOT TRACE_QAPI_RESULT EQUAL 0)
    message(FATAL_ERROR "QAPI trace header generation failed for ${TRACE_FILE}")
  endif()
endforeach()

option(XEMU_SKIP_TRACE_GEN "Skip QEMU trace header generation (use stub headers)" ON)
if(XEMU_SKIP_TRACE_GEN)
  message(STATUS "Skipping trace generation; using stub headers in ${REPO_ROOT}/trace")
else()
  set(TRACE_GEN_DIR "${CMAKE_BINARY_DIR}/trace")
  file(MAKE_DIRECTORY "${TRACE_GEN_DIR}")
  file(GLOB_RECURSE TRACE_EVENT_FILES "${REPO_ROOT}/**/trace-events")
  foreach(TRACE_FILE ${TRACE_EVENT_FILES})
    file(RELATIVE_PATH TRACE_REL "${REPO_ROOT}" "${TRACE_FILE}")
    get_filename_component(TRACE_REL_DIR "${TRACE_REL}" DIRECTORY)
    if(TRACE_REL_DIR STREQUAL ".")
      set(TRACE_GROUP "root")
    else()
      string(REPLACE "/" "_" TRACE_GROUP "${TRACE_REL_DIR}")
    endif()
    set(TRACE_OUT "${TRACE_GEN_DIR}/trace-${TRACE_GROUP}.h")
    execute_process(
      COMMAND "${PYTHON_EXECUTABLE}" "${REPO_ROOT}/scripts/tracetool.py"
              --format=h --backends=nop --group "${TRACE_GROUP}"
              "${TRACE_FILE}" "${TRACE_OUT}"
      WORKING_DIRECTORY "${REPO_ROOT}"
      RESULT_VARIABLE TRACE_GEN_RESULT
    )
    if(NOT TRACE_GEN_RESULT EQUAL 0)
      message(FATAL_ERROR "Trace generation failed for ${TRACE_FILE}")
    endif()
  endforeach()
endif()

add_library(xemu_core STATIC
  ${XEMU_CORE_SOURCES}
)

if(XEMU_ENABLE_VULKAN)
  target_sources(xemu_core PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/vma_impl.cpp"
  )
endif()

file(READ "${REPO_ROOT}/QEMU_VERSION" QEMU_VERSION_STR)
string(STRIP "${QEMU_VERSION_STR}" QEMU_VERSION_STR)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${QEMU_VERSION_STR}")
set(QEMU_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(QEMU_VERSION_MINOR "${CMAKE_MATCH_2}")
set(QEMU_VERSION_MICRO "${CMAKE_MATCH_3}")

# --- xemu version header ---
set(XEMU_VERSION_STR "")
execute_process(
  COMMAND git describe --tags --match "v*"
  WORKING_DIRECTORY "${REPO_ROOT}"
  RESULT_VARIABLE XEMU_GIT_DESCRIBE_RESULT
  OUTPUT_VARIABLE XEMU_VERSION_STR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT XEMU_GIT_DESCRIBE_RESULT EQUAL 0 OR XEMU_VERSION_STR STREQUAL "")
  if(EXISTS "${REPO_ROOT}/XEMU_VERSION")
    file(READ "${REPO_ROOT}/XEMU_VERSION" XEMU_VERSION_STR)
    string(STRIP "${XEMU_VERSION_STR}" XEMU_VERSION_STR)
  else()
    set(XEMU_VERSION_STR "0.0.0")
  endif()
endif()
if(XEMU_VERSION_STR MATCHES "^v")
  string(SUBSTRING "${XEMU_VERSION_STR}" 1 -1 XEMU_VERSION_STR)
endif()

set(XEMU_COMMIT "")
execute_process(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY "${REPO_ROOT}"
  RESULT_VARIABLE XEMU_GIT_REV_RESULT
  OUTPUT_VARIABLE XEMU_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT XEMU_GIT_REV_RESULT EQUAL 0 OR XEMU_COMMIT STREQUAL "")
  if(EXISTS "${REPO_ROOT}/XEMU_COMMIT")
    file(READ "${REPO_ROOT}/XEMU_COMMIT" XEMU_COMMIT)
    string(STRIP "${XEMU_COMMIT}" XEMU_COMMIT)
  else()
    set(XEMU_COMMIT "")
  endif()
endif()

string(TIMESTAMP XEMU_DATE "%a %b %d %H:%M:%S UTC %Y" UTC)

string(REPLACE "-" ";" XEMU_VERSION_FIELDS "${XEMU_VERSION_STR}")
list(LENGTH XEMU_VERSION_FIELDS XEMU_VERSION_FIELDS_LEN)
set(XEMU_VERSION_BASE "${XEMU_VERSION_STR}")
if(XEMU_VERSION_FIELDS_LEN GREATER 0)
  list(GET XEMU_VERSION_FIELDS 0 XEMU_VERSION_BASE)
endif()
set(XEMU_VERSION_COMMIT "0")
if(XEMU_VERSION_FIELDS_LEN GREATER 1)
  list(GET XEMU_VERSION_FIELDS 1 XEMU_VERSION_COMMIT)
endif()

string(REPLACE "." ";" XEMU_VERSION_DOTS "${XEMU_VERSION_BASE}")
list(LENGTH XEMU_VERSION_DOTS XEMU_VERSION_DOTS_LEN)
set(XEMU_VERSION_MAJOR "0")
set(XEMU_VERSION_MINOR "0")
set(XEMU_VERSION_PATCH "0")
if(XEMU_VERSION_DOTS_LEN GREATER 0)
  list(GET XEMU_VERSION_DOTS 0 XEMU_VERSION_MAJOR)
endif()
if(XEMU_VERSION_DOTS_LEN GREATER 1)
  list(GET XEMU_VERSION_DOTS 1 XEMU_VERSION_MINOR)
endif()
if(XEMU_VERSION_DOTS_LEN GREATER 2)
  list(GET XEMU_VERSION_DOTS 2 XEMU_VERSION_PATCH)
endif()

set(XEMU_VERSION_HEADER "${CMAKE_BINARY_DIR}/xemu-version-macro.h")
file(WRITE "${XEMU_VERSION_HEADER}"
"#define XEMU_VERSION       \"${XEMU_VERSION_STR}\"\n"
"#define XEMU_VERSION_MAJOR ${XEMU_VERSION_MAJOR}\n"
"#define XEMU_VERSION_MINOR ${XEMU_VERSION_MINOR}\n"
"#define XEMU_VERSION_PATCH ${XEMU_VERSION_PATCH}\n"
"#define XEMU_VERSION_COMMIT ${XEMU_VERSION_COMMIT}\n"
"#define XEMU_COMMIT        \"${XEMU_COMMIT}\"\n"
"#define XEMU_DATE          \"${XEMU_DATE}\"\n"
)

target_compile_definitions(xemu_core PRIVATE
  ANDROID
  __ANDROID__
  XBOX
  CONFIG_SDL
  CONFIG_OPENGL
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:CONFIG_VULKAN>
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:VK_NO_PROTOTYPES>
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:VMA_STATIC_VULKAN_FUNCTIONS=0>
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:VMA_DYNAMIC_VULKAN_FUNCTIONS=0>
  CONFIG_RENDERDOC
  $<$<BOOL:${XEMU_OPT_NATIVE_FLOAT}>:XEMU_OPT_NATIVE_FLOAT=1>
  $<$<BOOL:${XEMU_OPT_TCG_PEEPHOLE}>:XEMU_OPT_TCG_PEEPHOLE=1>
  $<$<BOOL:${XEMU_OPT_TB_CACHE_HINTS}>:XEMU_OPT_TB_CACHE_HINTS=1>
  CONFIG_ATOMIC64
  CONFIG_PPOLL
  CONFIG_PRCTL_PR_SET_TIMERSLACK
  COMPILING_PER_TARGET
  CONFIG_TARGET=\"config-target.h\"
  QEMU_VERSION=\"${QEMU_VERSION_STR}\"
  QEMU_VERSION_MAJOR=${QEMU_VERSION_MAJOR}
  QEMU_VERSION_MINOR=${QEMU_VERSION_MINOR}
  QEMU_VERSION_MICRO=${QEMU_VERSION_MICRO}
  CONFIG_TLS_PRIORITY=\"NORMAL\"
)

target_compile_options(xemu_core PRIVATE -g1
  $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:-UNDEBUG>
)

target_include_directories(xemu_core PRIVATE
  "${REPO_ROOT}"
  "${REPO_ROOT}/include"
  "${REPO_ROOT}/ui"
  "${REPO_ROOT}/ui/thirdparty/stb_image"
  "${REPO_ROOT}/linux-headers"
  "${REPO_ROOT}/hw/xbox/nv2a/pgraph/thirdparty/gloffscreen"
  "${REPO_ROOT}/target/i386"
  "${REPO_ROOT}/hw/xbox/nv2a/pgraph/thirdparty/gloffscreen"
  "${REPO_ROOT}/tcg/aarch64"
  "${REPO_ROOT}/host/include/aarch64"
  "${CMAKE_CURRENT_LIST_DIR}"
  "${CMAKE_BINARY_DIR}"
  "${QAPI_GEN_DIR}"
  "${GLIB_INSTALL_DIR}/include/glib-2.0"
  "${GLIB_INSTALL_DIR}/lib/glib-2.0/include"
  "${tomlplusplus_SOURCE_DIR}/include"
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:${glslang_SOURCE_DIR}>
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:${spirv_reflect_SOURCE_DIR}>
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:${vma_SOURCE_DIR}/include>
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:${volk_SOURCE_DIR}>
)

add_dependencies(xemu_core glib_ep)

if(TARGET SDL2::SDL2)
  target_link_libraries(xemu_core PRIVATE SDL2::SDL2)
endif()

add_library(glib-2.0 STATIC IMPORTED)
set_target_properties(glib-2.0 PROPERTIES
  IMPORTED_LOCATION "${GLIB_INSTALL_DIR}/lib/libglib-2.0.a"
)

add_library(gobject-2.0 STATIC IMPORTED)
set_target_properties(gobject-2.0 PROPERTIES
  IMPORTED_LOCATION "${GLIB_INSTALL_DIR}/lib/libgobject-2.0.a"
)

add_library(gio-2.0 STATIC IMPORTED)
set_target_properties(gio-2.0 PROPERTIES
  IMPORTED_LOCATION "${GLIB_INSTALL_DIR}/lib/libgio-2.0.a"
)

add_library(gmodule-2.0 STATIC IMPORTED)
set_target_properties(gmodule-2.0 PROPERTIES
  IMPORTED_LOCATION "${GLIB_INSTALL_DIR}/lib/libgmodule-2.0.a"
)

add_library(gthread-2.0 STATIC IMPORTED)
set_target_properties(gthread-2.0 PROPERTIES
  IMPORTED_LOCATION "${GLIB_INSTALL_DIR}/lib/libgthread-2.0.a"
)

add_library(xemu SHARED
  xemu_android.cpp
  android_crash_handler.cpp
  vmstate_if_android.c
)

target_compile_definitions(xemu PRIVATE ANDROID __ANDROID__ XBOX CONFIG_SDL CONFIG_OPENGL
  CONFIG_RENDERDOC
  $<$<BOOL:${XEMU_OPT_NATIVE_FLOAT}>:XEMU_OPT_NATIVE_FLOAT=1>
  $<$<BOOL:${XEMU_OPT_TCG_PEEPHOLE}>:XEMU_OPT_TCG_PEEPHOLE=1>
  $<$<BOOL:${XEMU_OPT_TB_CACHE_HINTS}>:XEMU_OPT_TB_CACHE_HINTS=1>
)
if(XEMU_ENABLE_VULKAN)
  target_compile_definitions(xemu PRIVATE CONFIG_VULKAN VK_NO_PROTOTYPES)
endif()

target_compile_options(xemu PRIVATE -fexceptions -frtti -g1
  $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:-UNDEBUG>
)

target_include_directories(xemu PRIVATE
  "${REPO_ROOT}"
  "${REPO_ROOT}/include"
  "${REPO_ROOT}/ui"
  "${REPO_ROOT}/ui/thirdparty/stb_image"
  "${REPO_ROOT}/target/i386"
  "${CMAKE_CURRENT_LIST_DIR}"
  "${CMAKE_BINARY_DIR}"
  "${QAPI_GEN_DIR}"
  "${GLIB_INSTALL_DIR}/include/glib-2.0"
  "${GLIB_INSTALL_DIR}/lib/glib-2.0/include"
  "${tomlplusplus_SOURCE_DIR}/include"
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:${volk_SOURCE_DIR}>
)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(glesv3-lib GLESv3)
find_library(vulkan-lib vulkan)
find_library(zlib-lib z)

if(XEMU_XISO_CONVERTER_BUILD_ENABLED)
  add_library(xiso_converter SHARED
    xiso_converter_jni.cpp
  )

  target_compile_definitions(xiso_converter PRIVATE ANDROID __ANDROID__)
  target_compile_options(xiso_converter PRIVATE -fexceptions -frtti -g1
    $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>:-UNDEBUG>
  )

  target_link_libraries(xiso_converter PRIVATE
    xiso_converter_rs
    ${log-lib}
  )
endif()

if(TARGET SDL2::SDL2)
  target_link_libraries(xemu PRIVATE SDL2::SDL2)
endif()

target_link_libraries(xemu PRIVATE
  xemu_core
  iconv_stub
  glib-2.0
  gobject-2.0
  gio-2.0
  gmodule-2.0
  gthread-2.0
  ${log-lib}
  ${android-lib}
  ${egl-lib}
  ${glesv3-lib}
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:volk_static>
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:adrenotools>
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:spirv_reflect_static>
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:glslang>
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:MachineIndependent>
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:GenericCodeGen>
  $<$<BOOL:${XEMU_ENABLE_VULKAN}>:SPIRV>
  ${zlib-lib}
)
