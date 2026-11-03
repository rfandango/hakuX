#!/bin/bash
# kantaiX Linux release: an AppImage of the executable, its fonts and mods,
# the docs and licenses, in dist/kantaiX-<version>-<arch>.AppImage.
#   tools/release/package_linux.sh [build dir] [out dir] [version]
# The arch is the host's (uname -m: x86_64 or aarch64), or override with $ARCH.
# appimagetool is fetched once into the out dir (or taken from $APPIMAGETOOL).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$REPO/../build/linux}
OUT=${2:-$REPO/../dist}
VERSION=${3:-1.0.0}
ARCH=${ARCH:-$(uname -m)}
APPDIR=$OUT/kantaiX.AppDir
mkdir -p "$OUT"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps" \
         "$APPDIR/usr/share/doc/kantaiX"

# the program finds its fonts and mods beside itself
install -m 755 "$BUILD/kantaiX" "$APPDIR/usr/bin/kantaiX"
strip --strip-debug "$APPDIR/usr/bin/kantaiX" 2>/dev/null || true
cp -r "$REPO/native/app/ui/fonts" "$APPDIR/usr/bin/fonts"
cp -r "$REPO/mods" "$APPDIR/usr/bin/mods"
mv "$APPDIR/usr/bin/mods/README.md" "$APPDIR/usr/share/doc/kantaiX/MODS.md"   # how to add patches
cp "$REPO/README.md" "$APPDIR/usr/share/doc/kantaiX/"
[ -f "$REPO/COPYING" ] && cp "$REPO/COPYING" "$APPDIR/usr/share/doc/kantaiX/"
[ -f "$REPO/LICENSE" ] && cp "$REPO/LICENSE" "$APPDIR/usr/share/doc/kantaiX/"
[ -d "$REPO/licenses" ] && cp -r "$REPO/licenses" "$APPDIR/usr/share/doc/kantaiX/licenses"

cat > "$APPDIR/usr/share/applications/kantaiX.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=kantaiX
GenericName=Xbox Emulator
Comment=Original Xbox emulator for Linux arm64 (based on hakuX + xbased)
Exec=kantaiX %f
Icon=kantaiX
Terminal=false
Categories=Game;Emulator;
Keywords=xbox;emulator;
X-AppImage-Version=$VERSION
EOF
cp "$REPO/native/app/desk/icon256.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/kantaiX.png"
ln -s usr/share/applications/kantaiX.desktop "$APPDIR/kantaiX.desktop"
ln -s usr/share/icons/hicolor/256x256/apps/kantaiX.png "$APPDIR/kantaiX.png"
ln -s kantaiX.png "$APPDIR/.DirIcon"

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
# settings, saves and states live in ~/.local/share/kantaiX (the image is read-only)
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/bin/kantaiX" "$@"
EOF
chmod 755 "$APPDIR/AppRun"

TOOL=${APPIMAGETOOL:-$OUT/appimagetool-$ARCH.AppImage}
if [ ! -x "$TOOL" ]; then
  curl -fL -o "$TOOL" "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage"
  chmod 755 "$TOOL"
fi
RUNTIME=${APPIMAGE_RUNTIME:-$OUT/runtime-$ARCH}
if [ ! -s "$RUNTIME" ]; then
  curl -fL -o "$RUNTIME" "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$ARCH"
fi
IMG=$OUT/kantaiX-$VERSION-$ARCH.AppImage
rm -f "$IMG"
# extract-and-run: no FUSE needed where it's built
ARCH=$ARCH "$TOOL" --appimage-extract-and-run --no-appstream --runtime-file "$RUNTIME" "$APPDIR" "$IMG"
ls -la "$IMG"
