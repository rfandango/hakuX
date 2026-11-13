# Mods and cheats

The mods and cheats the app ships, one folder per game. The build packs this
folder into the APK (`android/app/build.gradle.kts`, task `syncModAssets`);
the library's long-press menu shows **Mods & cheats** for a game that has a
folder here, and the in-game menu's **Cheats** turns cheats on and off while
playing. This file and CREDITS.md are not packed.

```
mods/
  <TITLE ID>.<VERSION>/    the disc's game key (54430006.6)
  <TITLE ID>/              a title's mods that each list the regions they fit (54430006)
    cheats.txt             its cheats (optional)
    <mod>/                 one mod per folder (the folder name is its id)
      mod.txt              its name, description and XBE patches
      files/               disc files it replaces or adds (optional)
```

A folder named by a title ID and a version is one disc release: its title ID
(8 hex digits) and its certificate version (decimal), as the app keys a
game's settings and savestates. Dead or Alive Ultimate's two discs are
54430006.5 and 54430006.6; a patch or a cheat's addresses hold for one
release only.

A folder named by the title ID alone (`54430006`) holds the title's mods
that name the regions they are for (`region:`, below): the app offers a mod
of it, and applies it, only on a disc made for one of those regions. The
release's own folder is read as well, and its mod of the same name wins.
The 781 mods of it are Jay's Magic Patches (see "Jay's Magic Patches").

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
- `region:` lists the regions the mod is for: `NTSC` (North America), `NTSC-J`
  (Japan), `PAL` (the rest of the world), apart by commas (`region: NTSC, PAL`).
  The disc's region is the one its certificate says. A mod without a `region:`
  line fits every disc of its folder; one that names none of the three fits no
  disc.
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
- `offset 0x713B: D0 02 00 00` writes the bytes at a position of the XBE
  *file* (`default.xbe`), not at an address: when the game starts, the app
  turns the position into the address the file's headers or sections give it
  (bytes that cross two sections become two patches). `search 80 02 -> 00 05`
  has no address either: the app looks for the first bytes in the sections
  of the disc's `default.xbe` and writes the second ones (the same length)
  over the first place they are, in file order. Both are the ways Jay's
  Magic Patcher (`.JMP`) files say where to patch, so the patches of that
  library are kept as they are. Spaces between the hex pairs are free; there
  are no wildcards. They work in `[default.xbe]` only. A mod with one that
  doesn't fit the disc (a position the file doesn't have, bytes that are
  not there: another build of the game) is left out whole and logged, so
  half a patch never runs. Only the bytes a `search` changes are written.
  ```
  [default.xbe]
  offset 0x00713B: D0 02 00 00 68 00 05
  search DB442468 85C97D06 D805BC44 -> DB442468 90909090 D80D7444
  ```
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

A mod can also be added in the app, without a new build: the library's
long-press menu, **Mods & cheats**, **Add a mod** takes patch lines pasted in
(addresses, `offset` or `search` lines; or a whole `mod.txt`). They are kept
in the app's storage (`user_mods/<GAME KEY>/<mod>/mod.txt`, and a
`cheats.txt` beside them is read too) and listed after the packed mods, with
a Remove button.

## Jay's Magic Patches

The mods of the title folders are the patches of **Xbox Magic Patches by Jay**
(https://github.com/JayYardley/Magic-Patches-by-Jay), a collection of
patches for Jay's Magic Patcher (`.JMP`, www.jayxbox.com): widescreen, 720p,
480i fixes, 128 MB Direct3D and more, for about 480 games. They are the work
of the authors the collection names (Nemesis2000, Esppiral, iroiry, Jao12345,
Silverrock, SPPV, Zorlon and more: CREDITS.md). Each `mod.txt` says who made
it (`author:`, shown in the app as "by <author>, via Jay's Magic Patches"),
which `.JMP` file it comes from, and keeps the library's own naming for the
mod, `{Widescreen} (NTSC+JPN) [Author]` (the nature of the patch, the
region, the author; the game's name is the screen's).

`tools/mods/jmp2mod.py` writes them from a copy of the library:

```
python tools/mods/jmp2mod.py <Xbox-Magic-Patches-by-Jay> mods --report report.md
```

- One `.JMP` file is one mod, in `mods/<TITLE ID>/<mod>/`. A file's records
  keep their order and comments: a position record (`0x713B:D002`) is an
  `offset` line, a pair of lines (the bytes to find, their replacement) a
  `search` line. A record marked `[-]` (off by default in the library) is
  kept as a comment.
- The regions: a file says them in its header (`region=NTSC, JAPAN`) and in
  its name (`(NTSC+JPN)`); the mod's `region:` lists the ones both name (a
  file for `GLOBAL` or `All Regions` lists the three), and the app applies
  it only to a disc made for one of them. The library doesn't give the
  certificate version of a disc, so a mod is for the region, not for one
  release: a revised disc of the region gets the patches it can, and a mod
  whose `offset` or `search` lines don't fit it is left out whole. A
  prototype's file that names no region and only searches for bytes has no
  `region:` line, so it fits every disc of its title (American Idol's
  prototype 720p patch).
- Every mod is off until the player turns it on, in the library's
  **Mods & cheats**.
- Not imported (the report lists each file): the dashboards, Chihiro files
  and prototypes that have no title ID (00000000: the app finds such a disc
  by its path, so no folder can name it; **Add a mod** takes their lines),
  the prototypes that name no region and write at positions, the ones whose
  header and name name no region in common, the ones for a title update's XBE
  (`- TU`: the app patches the disc's `default.xbe`), the ones that append
  code to the XBE (`APPEND`), and the records that edit the header's media
  and region flags (they let a real console run another region's disc).
- The patches are imported as they are, and not every one has been tried in
  hakuX. Two mods of one game that change the same bytes (two authors'
  widescreen patches) shouldn't be on together: the later one wins.

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
