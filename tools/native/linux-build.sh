#!/bin/sh
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
deps=${HAKUX_DEPS:-$repo/build/deps}
build=${HAKUX_BUILD:-$repo/build/linux}
python3 tools/native/linux-deps.py --dest "$deps" --verify
cmake -S native -B "$build" -G Ninja \
    -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHAKUX_LTO=OFF \
    -DHAKUX_FEX_DIR="$deps/fex" -DNC_DEPS="$deps" \
    -DHAKUX_BUILD_LIBRETRO=ON -DHAKUX_VERSION="${HAKUX_VERSION:-dev}" \
    -DHAKUX_LR_END_PROCESS=ON "$@"
cmake --build "$build" --target xboxrun hakux_libretro libretro-harness linux-smoke audio-smoke apu-reinit-smoke usb-smoke \
    vulkan-probe kms-grab -j "${JOBS:-4}"
ctest --test-dir "$build" --output-on-failure
