#!/usr/bin/env bash
# Fetches the native core's third-party sources (CMake NC_DEPS) at the
# revisions android/README.md lists.
#
#   tools/native/deps.sh [NC_DEPS dir, default build/native-deps]
set -euo pipefail
here=$(cd "$(dirname "$0")/../.." && pwd)
deps=${1:-$here/build/native-deps}
mkdir -p "$deps"

# fetch <dir> <repo> <revision> [patch]: a shallow checkout of one revision
# with its submodules, the patch applied, kept while its .hakux-rev stamp
# names that revision
fetch() {
  local dir=$deps/$1
  if [ "$(cat "$dir/.hakux-rev" 2>/dev/null)" = "$3" ]; then
    return
  fi
  rm -rf "$dir"
  git init -q "$dir"
  git -C "$dir" fetch -q --depth 1 "$2" "$3"
  git -C "$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
  git -C "$dir" submodule update -q --init --depth 1
  [ -z "${4:-}" ] || git -C "$dir" apply "$4"
  echo "$3" > "$dir/.hakux-rev"
}

fetch volk https://github.com/zeux/volk.git 1.4.304
fetch glslang https://github.com/KhronosGroup/glslang.git 16.1.0
fetch spirv_reflect https://github.com/KhronosGroup/SPIRV-Reflect.git vulkan-sdk-1.4.328.0
fetch vma https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git v3.3.0
fetch SDL3 https://github.com/libsdl-org/SDL.git release-3.4.18
fetch root/subprojects/nv2a_vsh_cpu https://github.com/abaire/nv2a_vsh_cpu.git 561fe80da57a881f89000256b459440c0178a7ce
fetch libslirp https://gitlab.freedesktop.org/slirp/libslirp.git 26be815b86e8d49add8c9a8b320239b9594ff03d
fetch adrenotools https://github.com/bylaws/libadrenotools.git 8fae8ce254dfc1344527e05301e43f37dea2df80 \
  "$here/tools/native/adrenotools-kgsl-context-priority.patch"
