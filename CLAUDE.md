# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

kantaiX runs original Xbox games on Linux arm64. It is a fork of [hakuX](https://github.com/rfandango/hakuX) (Xbox emulator core + Android app) with the [xbased](https://github.com/crazyriddler/xbased) SDL3 dashboard UI grafted on, targeting arm64 handhelds (AYN Odin 3 etc.) running desktop Linux (e.g. Armada OS).

The Android build inherited from hakuX is present but unmaintained. The only maintained target is the Linux arm64 desktop executable.

## Building

One-time setup:
```bash
git submodule update --init --recursive   # FEX submodule (external/fex, branch hakuX)
tools/fetch_deps.sh ../deps               # SDL3, volk, glslang, VMA, libslirp, etc. → ../deps
```

Build on the target device (Linux arm64):
```bash
cmake -S native -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" \
  -DNC_DEPS=../deps
cmake --build build --target hakux
# output: build/kantaiX
```

Cross-compile from x86_64 (catches errors without waiting for CI):
```bash
# One-time: sudo pacman -S aarch64-linux-gnu-gcc aarch64-linux-gnu-glibc aarch64-linux-gnu-linux-api-headers
tools/build_local.sh          # full build into build-arm64/kantaiX
tools/build_local.sh build    # skip dep fetch, just build
tools/build_local.sh clean    # wipe build-arm64/
```

CI runs on `ubuntu-24.04-arm` (GitHub Actions) and produces a downloadable AppImage on every push/PR; a GitHub Release is created on version tags.

## Running the HLE unit tests

The pure HLE tests run on any host (no Vulkan, no FEX needed):
```bash
cmake -S tests/hle -B build-tests -DNC_DEPS=../deps
cmake --build build-tests
ctest --test-dir build-tests          # all tests
ctest --test-dir build-tests -R test_pusher   # single test
```

## Code style

Clang-format (`.clang-format` at repo root) for C/C++. C11 for C, C++20 for C++. All sources build with `-Wall`; the HLE and APU layers suppress a few specific warnings but not broadly.

## PR workflow

Always work on a feature branch and open a PR — never push directly to master. CI must pass before merging.

## Architecture

The build produces three static libraries linked into the final executable:

**`hakux_native`** (`native/core/`, `native/kernel/`, `native/dev/`, `native/net/`, `native/state/`, `native/cpu/`)
The core emulator: VM layout (guest 4 GiB at host address 0), XBE loader, full Xbox kernel reimplementation (pool, objects, processes, threads, memory, paging, I/O, timers, APC, SEH, RTL), hardware device MMIO (NV2A registers, USB/OHCI, nvnet, ACI), networking (NAT/libslirp, LAN, KAI), savestates, cheats, mods, and the bridge to FEXCore (`native/cpu/cpu.cpp`).

On Linux arm64: a SIGSEGV/SIGBUS fault handler (`native/core/fault_linux_arm64.c`) and a Linux shim for Android system-properties and thread priorities (`native/compat/linux/`).

**`hakux_hle`** (`hw/xbox/hle/`)
The GPU HLE layer. The Xbox game writes a Direct3D push buffer to the NV2A; the HLE layer's pusher thread reads and decodes it (`pb/pusher.c`, `pb/pbdraw.c`, `pb/pbmode.c`) and drives a Vulkan backend (`vk/`). Fixed-function and programmable vertex/pixel shaders are translated to GLSL and compiled with glslang (`shader/`). Textures are decoded via Xbox SDK formats (`tex/`). Guest RAM access goes through the guest-memory abstraction (`mem/`).

**`hakux_apu`** (`native/dev/apu/`)
The MCPX APU, ported from xemu. GP/EP DSPs, voice processor, HRTF. On Linux arm64 it outputs via SDL3 audio; on Android it used AAudio.

**CPU JIT: FEX fork** (`external/fex`, submodule pointing at `rfandango/FEX` branch `hakuX`)
FEXCore is the x86→arm64 JIT. Only FEXCore is used (no FEX Linux front-end, no thunks). The guest's 4 GiB address space lives at host address 0; `-fno-delete-null-pointer-checks` is set project-wide because null-base pointer arithmetic is routine.

**Desktop executable** (`native/app/`)
The SDL3 dashboard UI (`app/desk_main.c`, `app/ui/`): library browser, per-game mods and settings, saves/savestates, presentation (`app/present.c`, `app/vsync.c`). Mods and dashboard fonts are copied beside the executable at build time (`cmake/app_desk.cmake`).

## Mods and cheats

`mods/<TITLEID>.<VERSION>/` holds per-game mods (XBE patches in `mod.txt`, optional replacement files in `files/`) and cheats (`cheats.txt`). The build copies the whole `mods/` tree next to the executable. `refresh: 120` in `mod.txt` overclocks the vblank to 120 Hz. See `mods/README.md`.

## Key CMake variables

| Variable | Default | Meaning |
|---|---|---|
| `NC_DEPS` | `../../deps` | Path to third-party sources fetched by `tools/fetch_deps.sh` |
| `HAKUX_FEX_DIR` | `external/fex` | Path to the FEX submodule |
| `HAKUX_ARCH` | (empty) | `-march` flag for FEX and core (empty = generic armv8-a) |
| `HAKUX_LTO` | ON | ThinLTO in Release builds (FEX excluded) |

## Important: do not commit

- `mods/4D53002D.1/` — the DOA3 120 fps mod is in progress and broken; never commit it.
