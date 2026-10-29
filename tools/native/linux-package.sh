#!/bin/sh
# PortareOS's hakux-lr package's input: the libretro core with its core
# info, stripped, with the licences and the revisions it was built from, as
# build/package/hakux-<version>-aarch64.tar.gz.
# Run after linux-build.sh, in the same image, on a clean checkout: the
# archive is the commit's.
set -eu

version=${1:?usage: linux-package.sh <version>}
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
build=${HAKUX_BUILD:-$repo/build/linux}
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "linux-package.sh: the checkout has changes; package a commit" >&2
    exit 1
fi

out=$repo/build/package
name=hakux-$version
rm -rf "${out:?}/$name"
mkdir -p "$out/$name/lib/libretro" "$out/$name/share/licenses/hakux"
"${STRIP:-llvm-strip-19}" -o "$out/$name/lib/libretro/hakux_libretro.so" "$build/hakux_libretro.so"
sed "s/^display_version = .*/display_version = \"$version\"/" native/libretro/hakux_libretro.info \
    > "$out/$name/lib/libretro/hakux_libretro.info"
cp LICENSE COPYING licenses/* "$out/$name/share/licenses/hakux/"
{
    echo "hakuX $version"
    echo "source $(git rev-parse HEAD) (https://github.com/portare-ch/hakuX)"
    echo "compiler $("${CC:-clang}" --version | head -1)"
    echo "runtime libz, libstdc++, libm, libgcc_s, glibc 2.38 (aarch64); libvulkan.so.1 loaded at run time"
    echo "dependencies (tools/native/linux-deps.json):"
    cat tools/native/linux-deps.json
} > "$out/$name/BUILD-INFO"
tar -C "$out" --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@$(git log -1 --format=%ct)" -cf - "$name" | gzip -n > "$out/$name-aarch64.tar.gz"
cd "$out" && sha256sum "$name-aarch64.tar.gz"
