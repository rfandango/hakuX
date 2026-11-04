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

## Features

<table>
<tr>
<td width="50%" valign="top">

### Like a console
- **Console-style dashboard**: an animated green orb, drifting hexagons, 3D
  transitions and navigation sounds.
- **Gamepad, keyboard or mouse** everywhere; button prompts follow the device
  you're using.
- **Borderless full screen by default**, any resolution, any DPI.
- **Exit from the gamepad**, with a confirmation.

</td>
<td width="50%" valign="top">

### Your library
- **Automatic box art** and the discs' own title icons.
- **A page per game**: play, load a state, settings, patches, controls.
- **Memory manager**: your games' saves with their icons; export, import and
  delete.
- **Broad compatibility** with many games, inherited from hakuX.

</td>
</tr>
<tr>
<td valign="top">

### Picture
- **Internal resolution** up to 8x, aspect ratio, widescreen and video modes.
- **FXAA** and **anisotropic filtering** up to 16x.
- **Upscaling**: bilinear or AMD FSR 1.
- **CRT filters**: soft scanlines, a classic TV, an arcade monitor.
- Every setting **global or per game**.

</td>
<td valign="top">

### Patches & mods
- **Per-game patches**: 60/120 fps, widescreen, fixes and cheats, toggled per
  game.
- **Frame-rate unlock per game**, where the game allows it.
- **Add your own**: drop IPS patches into `mods/<TITLE ID>/` and they show up
  in the game's menu ([mods/README.md](mods/README.md)).

</td>
</tr>
<tr>
<td valign="top">

### Save states & in-game menu
- **10 save-state slots per game**, with pictures.
- **In-game menu** with F1, or **Back + Start** / the **Guide** button on a
  gamepad: states, settings, controls, restart, back to the dashboard.
- **Shortcuts**: F5 quick save, F8 quick load, Tab fast forward, F2 pause,
  F12 screenshot, F11 full screen; all configurable.

</td>
<td valign="top">

### Controls, online, languages
- **4 controller ports**: automatic, any gamepad or the keyboard; remap every
  button, per game too; dead zone and vibration.
- **PAL games at 60 Hz**, as with the console's PAL-60 setting on.
- **Stereo or 5.1 surround**: the games' Dolby Digital mix, six channels out.
- **Online**: Insignia (the community's Xbox Live replacement), XLink Kai and
  System Link on your network.
- **11 languages**, your system's by default.

</td>
</tr>
</table>

## Getting started

1. Download `kantaiX-<version>-aarch64.AppImage` from
   [Releases](https://github.com/kiequoo/kantaiX/releases/latest), `chmod +x`
   it and run it.
2. The dashboard opens. Go to **Settings › Interface › Add folder** and pick the
   folder with your disc images.
3. Choose a game in **Games**. Box art downloads automatically.

**Command line:** `kantaiX <image.iso> [options]` starts a game directly.
Options match the settings files' keys (`scale=2`, `uncap`, `crt=2`,
`fullscreen=0`, `mod=<patch>`, …). Settings are kept in `xbased.ini` (global)
and `games/<TITLE ID>.<version>.ini` (per game) in `~/.local/share/xbased/`.

### Default keyboard controls

| Xbox | Key | Xbox | Key |
|---|---|---|---|
| A / B / X / Y | Space / Left Shift / Q / E | Left stick | W A S D |
| Black / White | C / Z | Right stick | I J K L |
| Left / right trigger | 1 / 3 | D-pad | Arrow keys |
| Start / Back | Enter / Backspace | Stick clicks | F / H |

**Shortcuts:** F1 menu, F5 quick save, F8 quick load (Shift: slot 2), Tab fast
forward (hold), F2 pause, F12 screenshot, F11 full screen. On a gamepad,
Back + Start or the Guide button opens the menu.

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
