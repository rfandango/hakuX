# kantaiX

**kantai** (艦隊) is Japanese for *fleet* or *armada* — named for
[Armada OS](https://armada.gg), the arm64 Linux platform this targets.
It sits in the hakuX / xbased / xemu "X" family.

kantaiX runs original Xbox games on Linux arm64, with a standalone SDL3 dashboard
UI. Aimed at arm64 handhelds (AYN Odin 3, etc.) running desktop Linux (e.g.
Armada OS).

Its core runs the game's x86 code on a JIT (a FEX fork) under a
reimplementation of the Xbox kernel, and draws by decoding the push buffer
the game's own Direct3D sends the NV2A into Vulkan (Mesa/Turnip on Adreno).
Audio is the MCPX APU, ported from xemu.

The UI provides per-game mods and patches (including `refresh: 120` for 120 fps),
library browsing, saves/states, and game settings — features the libretro/RetroArch
core does not expose.

## Requirements

- A Linux arm64 device with Vulkan (Mesa/Turnip for Adreno GPUs).
- Your own game images (ISO or XISO), dumped from discs you own.

No BIOS, MCPX ROM or hard disk image is needed.

## Building

```bash
# Initialise the FEX submodule
git submodule update --init --recursive

# Fetch third-party deps (SDL3, volk, glslang, etc.) into ../deps
tools/fetch_deps.sh ../deps

# Configure and build
cmake -S native -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" \
  -DNC_DEPS=../deps
cmake --build build --target hakux
# produces build/kantaiX
```

CI (GitHub Actions, `ubuntu-24.04-arm`) produces a downloadable
`kantaiX-<ref>-aarch64.AppImage` on every push.

## Credits

kantaiX stands on the shoulders of two projects:

- **[rfandango/hakuX](https://github.com/rfandango/hakuX)** — the Xbox emulator
  core and Android app this is forked from. All emulation work is upstream's.
- **[crazyriddler/xbased](https://github.com/crazyriddler/xbased)** — the
  standalone SDL3 dashboard UI (mods, library, settings, saves) grafted on here.

kantaiX's own contribution is the Linux arm64 desktop target, the CI packaging,
and the glue between the two upstreams. If you want an Android build, use
[hakuX](https://github.com/rfandango/hakuX) directly.

The Android build inherited from hakuX is present but unmaintained here.

## License

kantaiX is free software under the GNU General Public License, version 3 or
later ([COPYING](COPYING)): anyone who distributes it, or a work based on it,
must make the complete source available under the same license. The code
ported from xemu (the MCPX APU) and the third-party components keep their
own licenses, listed in [LICENSE](LICENSE) and [licenses/](licenses/).

## Notes

- kantaiX is not affiliated with Microsoft.
