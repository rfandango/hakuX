# hakuX

hakuX runs original Xbox games on Android (arm64), tuned for Adreno GPUs.

Its core runs the game's x86 code on a JIT (a FEX fork) under a
reimplementation of the Xbox kernel, and draws by decoding the push buffer
the game's own Direct3D sends the NV2A into Vulkan. Audio is the MCPX APU,
ported from xemu.

## hakuX is free

hakuX is free, and always will be. The only official download is
[hakuX's GitHub releases](https://github.com/rfandango/hakuX/releases).

If you paid for hakuX in any way, shape or form, you were scammed. That
includes a store purchase, a subscription, an in-app unlock, a "premium"
build, or an app that repackages it. Ask for a refund wherever you paid, and
get hakuX here for free.

## Requirements

- An arm64 Android device with Vulkan. Custom Turnip drivers can be loaded
  for Adreno GPUs.
- Or an aarch64 Linux with Vulkan (the system's loader and driver, e.g.
  Mesa's) and glibc 2.36 or later, for the AppImage.
- Your own game images (ISO or XISO), dumped from discs you own.

No BIOS, MCPX ROM or hard disk image is needed. hakuX doesn't include any
copyrighted Xbox software.

## Building

- Android: see [android/README.md](android/README.md).
- Linux (aarch64): `tools/native/appimage.sh` builds
  `build/hakuX-<version>-aarch64.AppImage`; its header has the Docker
  commands to run it from another host.

## Linux

```
./hakuX-<version>-aarch64.AppImage game.iso [fullscreen] [hdd=<folder>] [options]
```

The hard disk and shader caches live in `~/.local/share/hakuX`. Gamepads
are players 1-4 in the order they connect; the keyboard is player 1 too:

| Keys | Xbox |
|---|---|
| WASD | left stick |
| Arrows | d-pad |
| K / L / J / I | A / B / X / Y |
| U / O | white / black |
| Q / E | left / right trigger |
| Enter / Backspace | Start / Back |

F11 toggles full screen. Debug properties (`debug.hakux.*`) read at startup
come from environment variables of the same name (`env debug.hakux.caps=... ./hakuX...`);
the ones Android polls during play are not read.

Known limitation: there is no vsync source on Linux (Android uses
Choreographer), so the emulated vblank runs free of the display's refresh.

## License

hakuX is free software under the GNU General Public License, version 3 or
later ([COPYING](COPYING)): anyone who distributes it, or a work based on it,
must make the complete source available under the same license. The code
ported from xemu (the MCPX APU) and the third-party components keep their
own licenses, listed in [LICENSE](LICENSE) and [licenses/](licenses/).

## Notes

- hakuX is not affiliated with Microsoft.
