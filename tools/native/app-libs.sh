#!/usr/bin/env bash
# Puts the native core's app libraries (libhakuxcore.so, libSDL3.so and
# adrenotools' hook libraries from the native build) into the app's jniLibs,
# stripped, for assembleDebug to pack.
# They are build outputs (gitignored), refreshed by running this after
# `cmake --build build/native/core --target hakuxcore`.
#
#   tools/native/app-libs.sh [native build dir]
set -euo pipefail
here=$(cd "$(dirname "$0")/../.." && pwd)
build=${1:-$here/build/native/core}
ndk=${ANDROID_NDK:-/c/Users/USER/AppData/Local/Android/Sdk/ndk/29.0.14206865}
# The NDK's strip, from the host's prebuilt directory: the Windows one this
# script was written on, or the Linux or macOS one a build machine has.
strip=
for h in linux-x86_64 windows-x86_64 darwin-x86_64; do
  for s in llvm-strip llvm-strip.exe; do
    if [ -x "$ndk/toolchains/llvm/prebuilt/$h/bin/$s" ]; then
      strip="$ndk/toolchains/llvm/prebuilt/$h/bin/$s"
      break 2
    fi
  done
done
if [ -z "$strip" ]; then
  echo "app-libs.sh: no llvm-strip under $ndk (set ANDROID_NDK)" >&2
  exit 1
fi
out="$here/android/app/src/main/jniLibs/arm64-v8a"
mkdir -p "$out"
"$strip" -o "$out/libhakuxcore.so" "$build/libhakuxcore.so"
"$strip" -o "$out/libSDL3.so" "$build/sdl3/libSDL3.so"
# adrenotools' hook libraries: custom Vulkan drivers (Turnip) load through
# them from the app's native library directory.
for h in main_hook file_redirect_hook gsl_alloc_hook kgsl_context_hook hook_impl; do
  if [ -f "$build/adrenotools/src/hook/lib$h.so" ]; then
    "$strip" -o "$out/lib$h.so" "$build/adrenotools/src/hook/lib$h.so"
  fi
done
ls -la "$out"
