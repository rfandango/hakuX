# The Linux (aarch64) build environment for the AppImage
# (tools/native/appimage.sh). Debian 12 keeps the glibc baseline at 2.36,
# which Raspberry Pi OS (bookworm) and newer distributions run. Clang 19:
# clang 16 has no preserve_all on aarch64, with which FEX's JIT keeps guest
# registers across its helpers.
#
#   docker build --platform linux/arm64 -t hakux-linux -f tools/native/linux.Dockerfile tools/native
FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates git file curl python3 cmake ninja-build pkg-config \
      clang-19 lld-19 llvm-19 \
      zlib1g-dev libvulkan-dev \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev libxkbcommon-dev \
      libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libdrm-dev libgbm-dev \
      libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev \
    && rm -rf /var/lib/apt/lists/*

ENV CC=clang-19 CXX=clang++-19
