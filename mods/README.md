# Mods and cheats

The mods and cheats the app ships, one folder per game. The build packs this
folder into the APK (`android/app/build.gradle.kts`, task `syncModAssets`);
the library's long-press menu shows **Mods & cheats** for a game that has a
folder here, and the in-game menu's **Cheats** turns cheats on and off while
playing. This file is not packed.

```
mods/
  <TITLE ID>.<VERSION>/    the disc's game key (54430006.6)
    cheats.txt             its cheats (optional)
    <mod>/                 one mod per folder (the folder name is its id)
      mod.txt              its name, description and XBE patches
      files/               disc files it replaces or adds (optional)
```

A folder is one disc release: its title ID (8 hex digits) and its
certificate version (decimal), as the app keys a game's settings and
savestates. Dead or Alive Ultimate's two discs are 54430006.5 and
54430006.6; a patch or a cheat's addresses hold for one release only.

Text files are UTF-8. A line whose first non-blank character is `#` is a
comment. Numbers are hexadecimal with `0x`, else decimal.

## Mods (`mod.txt`, `files/`)

```
name: Widescreen
description: 16:9 menus and fights.
author: someone

[default.xbe]
0x0011A2B4: 90 90
0x0011A2C0: 74 05 -> EB 05
```

- `name:` (the folder name if missing), `description:`, `author:`.
- `refresh: 120` runs the console's vblank at that rate (for a 60 Hz video
  mode; a 50 Hz one scales with it): a game paced by vblanks presents that
  many frames a second. A frame-locked game then runs that much faster
  unless the mod's patches halve its steps.
- `[<file>]` starts the patches of an XBE on the disc, by its path there
  (`default.xbe`, `doa2.xbe`, `bin\game.xbe`). Each line is a virtual
  address, a colon and the bytes written there (hex pairs). `old -> new`
  checks the bytes first: a patch whose bytes differ is skipped and logged
  (the wrong disc or another mod's patch). Patches change the XBE as it is
  read, so sections the game loads later are patched too.
- `[$u\default.xbe]` starts the patches of the game's Xbox Live title
  update (`E:\TDATA\<title ID>\$u\default.xbe`, which boots instead of the
  disc's once installed), in the same mod: its addresses differ from the
  disc's. Without an update installed the section does nothing. The game
  reading its XBE as a file (an updater building the update) gets the
  disc's bytes, without the patches.
- `files/` mirrors the disc: `files/media/title.xpr` replaces `media\title.xpr`;
  a path the disc doesn't have is added (folders too). The game sees the
  file's new size, also in directory listings.
- Mods apply when the game starts (a change in the menu takes the next
  start). Enabled mods apply in folder-name order; when two replace the same
  file or patch the same bytes, the later one wins.
- A mod with injected code is better generated than written by hand:
  `tools/mods/` holds the generators (`doa2_120fps.py` writes
  `54430006.6/120fps/mod.txt` from the game's XBE).

## Cheats (`cheats.txt`)

```
[Infinite time]
note: The round timer stays at 99.
w32 0x0034A5C8 99

[Player 1 always at full health]
if16 0x00ABCDE0 != 0
  w16 [0x00456700]+0x1C 0x00C8
end

[Unlock everything]
once
w8 0x0060F100 0xFF
```

- `[<name>]` starts a cheat. `note:` is shown under its name; `once` writes
  it once when it is turned on (or the game starts with it on) instead of
  every frame.
- Code lines run in order every frame (each vblank) while the cheat is on:

| Line | Does |
|---|---|
| `w8 A V`, `w16 A V`, `w32 A V` | writes the 8 / 16 / 32-bit value V at A |
| `wf A F` | writes the float F at A |
| `if8 A C V`, `if16`, `if32`, `iff` | runs the lines up to `else` / `end` only if the value at A compares to V; C is `==`, `!=`, `<`, `>`, `<=`, `>=` (unsigned; signed floats for `iff`) or `&` (all of V's bits set) |
| `else`, `end` | the other branch; the end of an `if` (up to 8 deep) |

- An address A is a virtual address of the game (as a debugger or Cheat
  Engine on the console shows it: the XBE at 0x00010000, its heap above, the
  contiguous memory at 0x80000000). `[A]+N` (or `-N`) reads the 32-bit
  pointer at A and adds N; they nest: `[[0x00456700]+4]+0x10`. A pointer to
  unmapped memory skips the line; an unmapped address skips a write and
  makes an `if` false.
- Values are written only when they differ, little-endian as the Xbox
  stores them.
