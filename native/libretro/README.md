# The libretro core

`hakux_libretro.so` runs hakuX as a RetroArch core on AArch64 Linux.
RetroArch hands it its Vulkan device, sound and controllers: the core
renders on RetroArch's device and hands each frame over as an image,
phase-locked to `retro_run` at the Xbox's 60000/1001 Hz.

## What it needs

- AArch64 Linux with glibc 2.38 or newer, libstdc++ from GCC 13 or newer,
  and zlib.
- A Vulkan 1.3 driver. Mesa's Turnip on Adreno 740 is tested.
- RetroArch with its `vulkan` video driver; 1.22.2 is tested. With another
  video driver the core copies each frame through the CPU, which is for
  diagnosis only.
- Your own game images: `.iso` (an XISO or a full disc dump), or `.chd`
  made from one with `chdman createdvd -i game.iso -o game.chd`. CD CHDs
  (`chdman createcd`) don't work: an Xbox disc is a DVD. No BIOS, MCPX ROM
  or hard disk image is needed.

## Building

On an AArch64 Linux host with Clang 19, CMake, Ninja, Git, Python 3 and
zlib's development files, from the repository root:

```sh
python3 tools/native/linux-deps.py
CC=clang-19 CXX=clang++-19 sh tools/native/linux-build.sh
```

`linux-deps.py` fetches the pinned dependencies
([linux-deps.json](../../tools/native/linux-deps.json), FEX's submodules
included) into `build/deps`, and refuses modified or mismatched checkouts.
`linux-build.sh` builds into `build/linux` and runs the checks below. Or in
the pinned Debian image:

```sh
docker build --tag hakux-linux --file tools/native/Dockerfile.linux tools/native
docker run --rm --volume "$PWD:/src" --workdir /src hakux-linux \
  sh -c 'python3 tools/native/linux-deps.py && sh tools/native/linux-build.sh'
```

`HAKUX_BUILD` selects another build directory, `HAKUX_DEPS` another
dependency directory and `JOBS` the parallelism; further arguments go to
CMake. `linux-build.sh` sets `HAKUX_LR_END_PROCESS=ON`: a stop that takes
longer than 10 s then ends RetroArch's process rather than leave it hung.
`-DHAKUX_LR_END_PROCESS=OFF` builds without it.

## Installing

1. Copy `build/linux/hakux_libretro.so` into RetroArch's cores directory
   (Settings → Directory → Cores).
2. Copy `native/libretro/hakux_libretro.info` into its core info directory
   (Settings → Directory → Core Info).

Or load it directly:

```sh
retroarch -v -L build/linux/hakux_libretro.so /path/to/game.iso 2>&1 | tee hakux.log
```

The core keeps the console's hard disk in `hdd/` and its shader and pipeline
caches in `cache/`, both under RetroArch's save directory. Leave run-ahead
off.

## Testing in RetroArch

Start RetroArch from a terminal as above. The core's log lines start with
`hakuX-`. `HAKUX_DISP_STATS=1` and `HAKUX_LR_STATS=1` in the environment add,
every 10 s, the new frames per second and the core's time in each
`retro_run`.

What to check:

1. **Booting and playing:** the game reaches its menus and plays, with sound
   and controller 1.
2. **Players 2-4:** a second controller (RetroArch's user 2) plays as player
   2, and so on up to 4. Each port has a controller plugged in unless its
   Device Type (Quick Menu → Controls → Port N Controls) is None. Setting
   player 1's to None during the game pulls the controller out: most games
   pause and ask for it. Setting it back plugs it in again.
3. **Speed:** with the statistics on, a 60 fps game shows about 59.9 new
   frames a second and a 30 fps game about 30.
4. **Restart** (Quick Menu → Restart): the game starts again from the
   beginning. The harness's reset passes; on a device it hasn't been
   confirmed yet.
5. **Close Content:** the game closes without hanging, and another game
   loads in the same RetroArch session.
6. **Core options** (Quick Menu → Core Options): Render Scale, Screen Format,
   Language and Video Region take effect the next time the game starts, and
   RetroArch says so. Crop Overscan changes at once.
7. **Game saves:** what the game saves is still there after closing and
   starting it again.
8. **Games that start another of their programs** (some compilations and
   demo discs): they continue without going back to the menu.
9. **Video driver restarts:** some RetroArch video settings restart its
   video driver. The game then starts again, with a message saying so.
10. **CHD:** the game as a `.chd` plays as it does from the `.iso`.

Core options (`options.c`). All but the crop are read once, before the
console boots:

| Key | Values | Default |
| --- | --- | --- |
| `hakux_render_scale` | `1`-`4`: the renderer's resolution, times the game's | `1` |
| `hakux_crop_overscan` | `enabled`, `disabled` (the whole frame) | `enabled` |
| `hakux_screen_format` | `normal`, `widescreen` (shown 16:9), `letterbox` | `normal` |
| `hakux_language` | `auto` (RetroArch's), `english`, `japanese`, `german`, `french`, `spanish`, `italian`, `korean`, `chinese`, `portuguese` | `auto` |
| `hakux_video_region` | `auto` (the game's region), `ntsc-m`, `ntsc-j`, `pal` | `auto` |

Filters such as FXAA and the picture's shape on screen are RetroArch's: its
shaders and its aspect ratio setting ("Core provided" shows the core's 4:3
or 16:9). Rumble and save states are not available yet.

## Checks without a device

`linux-build.sh` runs CTest: the CPU and memory, audio, USB, boundary and
libretro suites. They make their own small synthetic titles and need no game
or BIOS.

`build/linux/libretro-harness <core.so> <game.iso> <frames> <save directory>`
loads the core as RetroArch does and can play a real game headless. Its
variables are described at the top of
[harness.c](../../tests/native/libretro/harness.c): `HARNESS_VULKAN`,
`HARNESS_CYCLES`, `HARNESS_RESET_AT`, `HARNESS_REINIT_AT`,
`HARNESS_PORTS_AT`, `HARNESS_PRESS` and others.
