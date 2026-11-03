#!/bin/bash
# Local cross-compile: x86_64 host → Linux arm64 target.
# The binary won't run locally, but compilation errors are caught immediately
# without waiting for GitHub Actions.
#
# One-time setup:
#   sudo pacman -S aarch64-linux-gnu-gcc aarch64-linux-gnu-glibc aarch64-linux-gnu-linux-api-headers
#
# Usage:
#   tools/build_local.sh          # full build (fetch deps if needed, configure, build)
#   tools/build_local.sh build    # skip fetch, just build
#   tools/build_local.sh clean    # wipe build dir
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$REPO/build-arm64"
DEPS="$REPO/../deps"
ACTION=${1:-full}

if [ "$ACTION" = clean ]; then
  rm -rf "$BUILD"
  echo "Removed $BUILD"
  exit 0
fi

if [ "$ACTION" = full ] || [ ! -d "$DEPS/SDL3" ]; then
  echo "=== Fetching third-party sources ==="
  "$REPO/tools/fetch_deps.sh" "$DEPS"
fi

echo "=== Configuring for Linux arm64 (cross) ==="
cmake -S "$REPO/native" -B "$BUILD" -G Ninja \
  --toolchain "$REPO/native/cmake/toolchain-cross-aarch64.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNC_DEPS="$DEPS" \
  -DSDL_UNIX_CONSOLE_BUILD=ON

echo "=== Building ==="
cmake --build "$BUILD" --target hakux
echo "=== Done: $BUILD/kantaiX ==="
