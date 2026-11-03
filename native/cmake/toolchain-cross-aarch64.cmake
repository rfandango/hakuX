# Cross-compilation toolchain: clang on x86_64 → Linux aarch64
# Requires: pacman -S aarch64-linux-gnu-gcc aarch64-linux-gnu-glibc aarch64-linux-gnu-linux-api-headers
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(triple aarch64-linux-gnu)
set(CMAKE_SYSROOT /usr/${triple})

set(CMAKE_C_COMPILER       clang)
set(CMAKE_CXX_COMPILER     clang++)
set(CMAKE_ASM_COMPILER     clang)
set(CMAKE_C_COMPILER_TARGET   ${triple})
set(CMAKE_CXX_COMPILER_TARGET ${triple})
set(CMAKE_ASM_COMPILER_TARGET ${triple})

# Use lld — the GNU cross-linker works but lld is already a project dep
set(CMAKE_EXE_LINKER_FLAGS    "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS "-fuse-ld=lld")

# Don't let CMake's find_* pick up host (x86_64) libraries
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Prevent pkg-config from finding host (x86_64) packages and injecting
# /usr/include into SDL3's compile flags — only search the arm64 sysroot.
# The sysroot only has glibc, so SDL3 auto-disables wayland/dbus/pipewire/
# drm/etc. That's fine: the target device has them at runtime.
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_LIBDIR} "/usr/aarch64-linux-gnu/lib/pkgconfig:/usr/aarch64-linux-gnu/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "/usr/aarch64-linux-gnu")
