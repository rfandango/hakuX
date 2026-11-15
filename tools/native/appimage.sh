#!/usr/bin/env bash
# Builds hakuX for Linux (aarch64) and packs it as an AppImage:
# build/hakuX-<version>-aarch64.AppImage. It runs on an aarch64 Linux with
# the packages tools/native/linux.Dockerfile installs; from elsewhere (an
# Apple Silicon Mac, any arm64 Docker host):
#
#   docker build --platform linux/arm64 -t hakux-linux -f tools/native/linux.Dockerfile tools/native
#   docker run --rm --platform linux/arm64 -v "$PWD":/src -w /src hakux-linux tools/native/appimage.sh
#
# The AppImage carries hakux and SDL3; the Vulkan loader and driver are the
# system's (libvulkan.so.1). HAKUX_ARCH (native/cmake/fex.cmake) may name the
# CPUs it targets; the default runs on any ARMv8-A.
set -euo pipefail
here=$(cd "$(dirname "$0")/../.." && pwd)
build=$here/build/linux
deps=$here/build/native-deps
tools=$here/build/appimage-tools
appdir=$here/build/AppDir
APPIMAGETOOL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-aarch64.AppImage
RUNTIME=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-aarch64

if [ "$(uname -m)" != aarch64 ]; then
  echo "appimage.sh: builds on aarch64 Linux only (see the docker lines at the top)" >&2
  exit 1
fi

"$here/tools/native/deps.sh" "$deps"
cmake -S "$here/native" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld \
  -DNC_DEPS="$deps" -DHAKUX_ARCH="${HAKUX_ARCH:-}"
ninja -C "$build" hakux

rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/lib"
strip -o "$appdir/usr/bin/hakux" "$build/hakux"
strip -o "$appdir/usr/lib/libSDL3.so.0" "$build/sdl3/libSDL3.so.0"
ln -s usr/bin/hakux "$appdir/AppRun"
cp "$here/android/app/src/main/ic_launcher-playstore.png" "$appdir/hakux.png"
cat > "$appdir/hakux.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=hakuX
Comment=Original Xbox emulator
Exec=hakux %f
Icon=hakux
Categories=Game;Emulator;
MimeType=application/x-iso9660-image;
Terminal=false
EOF

mkdir -p "$tools"
[ -x "$tools/appimagetool" ] || { curl -fsSL -o "$tools/appimagetool" "$APPIMAGETOOL" && chmod +x "$tools/appimagetool"; }
[ -f "$tools/runtime" ] || curl -fsSL -o "$tools/runtime" "$RUNTIME"
# safe.directory: the checkout may be another user's (a container's mount)
version=$(git -c safe.directory="$here" -C "$here" describe --tags --always --dirty)
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=aarch64 VERSION="$version" \
  "$tools/appimagetool" --no-appstream --runtime-file "$tools/runtime" "$appdir" "$here/build/hakuX-$version-aarch64.AppImage"
