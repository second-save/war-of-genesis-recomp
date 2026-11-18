# The War of Genesis (창세기전, 1995) — native static recompilation

A native port of Softmax's DOS strategy RPG *The War of Genesis* (Korean release)
for macOS, Linux and Windows. The game's own executables are translated to C++
ahead of time (static recompilation) and run on a small DOS/hardware runtime
built on SDL2. No emulator runs at play time, and none of the game's logic is
rewritten by hand.

**This repository contains no game data.** You need your own copy of the DOS
game; point the port at its folder.

## Status

Playable from the original title screen: title music, Softmax logo voice, the
real title menu, New Game → prologue → world map → towns; saving from the map
menu and loading from the title menu; battles (briefing, deployment, battle
map, music, x87 floating point for the battle formulas). Complete battles,
the ending and later chapters are still being tested. English by default.

| Original program | What it does | Translated by |
|---|---|---|
| `TTL.EXE` | title, menus, intro, ending (32-bit, DOS/4GW) | `tools/lifter/lift_le.py` |
| `MAIN.EXE` | game driver | `tools/lifter/lift16.py` |
| `CHP.EXE` | story scenes, world map | `tools/lifter/lift16.py` |
| `PACK.EXE` | towns | `tools/lifter/lift16.py` |
| `CHRSYS.EXE` | battles, character screens | `tools/lifter/lift16.py` |

`G.COM`, the original launcher loop (title → game → title), is reproduced in
`src/main.cpp`.

## Building

Requires CMake ≥ 3.20, a C++20 compiler, SDL2 and Ninja (or Make).

```sh
cmake -S . -B build -G Ninja
ninja -C build
./build/genesis
```

On first launch the port looks for the game folder (`game_dir` in the
settings file, `The-War-of-Genesis_DOS_KO/` in the current directory, next to
the binary or next to the .app) and otherwise asks for it with the system's
folder picker. It checks the five executables against the release the code
was generated from, then copies the folder once into `game/` next to the
settings file. The game runs and saves there; the player's folder is only
read.

`--data-dir DIR` runs directly in `DIR` instead (development and tests). The
game writes saves and its config into that folder, as the original did.

Release package (macOS, universal, SDL2.framework inside, English as a patch
so no game data is shipped):

```sh
release/build-macos.sh The-War-of-Genesis_DOS_KO   # -> dist/WarOfGenesis-<VERSION>-macOS.zip
```

Regenerating the translated code (needs Python 3 with `capstone`):

```sh
python3 tools/lifter/lift_all.py   # reads the EXEs from The-War-of-Genesis_DOS_KO/
```

Never edit `src/modules/generated/` by hand. Fixes go into the translator or
its hint files (`tools/lifter/hints/*.json`).

## Controls

- The mouse works as in the original. The game draws its own pointer.
- **F12** toggles the *keyboard pointer*: arrow keys or the numpad move the
  pointer (Shift = faster), **Return/Z** left-click, **X/Backspace**
  right-click. Other keys go to the game as usual (Space skips story text).
- **F11** or **Alt+Enter**: fullscreen. **Cmd+Q** / **Alt+F4**: quit.
- Battles: right-click opens the next unit's command ring, Space ends the
  turn, Esc opens the battle options. The full battle guide (worked out from
  the game's code, as the manual is lost) is in
  [`release/README-player.txt`](release/README-player.txt).

The settings file (game folder, key bindings, volumes, language, fullscreen)
is created on first launch, next to the installed `game/` folder:

- macOS: `~/Library/Application Support/Softmax/WarOfGenesis/settings.ini`
- Linux: `~/.local/share/Softmax/WarOfGenesis/settings.ini`
- Windows: `%APPDATA%\Softmax\WarOfGenesis\settings.ini`

## Translation (work in progress)

English is the default (`language = en` in the settings file; `ko` plays the
original Korean). Translated files are served from `lang/en/` (next to the
binary, in the .app's Resources, or `../lang/en` from `build/`) in place of
the game's own, and the game folder is never modified. A release ships
`lang/en` as `TEXT.TXT`, `STRINGS.TXT` and `FILES.PAT`
(`tools/text/make_patch.py`): the rebuilt game files as copy/insert
operations against the player's own files, applied on first launch. With a translation active,
the dialogue typesetter is replaced by a native one (`src/runtime/text.cpp`)
that sets ASCII text proportionally (VWF) with word wrap and page breaks,
still using the game's own font and drawing routines. Korean mode runs the
original code unchanged.

```sh
# Korean source for translators (local only, not committed):
python3 tools/text/visual.py extract The-War-of-Genesis_DOS_KO work     # story dialogue
python3 tools/text/prologue.py extract The-War-of-Genesis_DOS_KO work   # chapter prologues
python3 tools/text/talk.py extract The-War-of-Genesis_DOS_KO work       # town talk
python3 tools/text/battle.py extract The-War-of-Genesis_DOS_KO work     # battle scenarios
# Build the overlay from translations/en:
tools/text/build_lang.sh The-War-of-Genesis_DOS_KO en
```

`translations/en/` holds the English text (no Korean game text): story
dialogue (`CHAPDATA/VISUAL.*.json`), chapter prologues (`CHAPDATA/PE.json`),
town talk (`CSTLDATA/TAK.json`), names and labels (`strings.tsv`), and the
naming choices (`glossary.tsv`). Town talk lines don't fit the game's 26-byte
records, so the built files hold `@@id` markers and the text lives in
`lang/en/TEXT.TXT`; the PACK text hook swaps them in.

Status: everything the game shows as text is in English: story dialogue,
chapter prologues, town talk, battle scenarios (briefings and dialogue), NPC,
unit, character and mercenary names, item names and descriptions
(`tools/text/items.py`), the menus, prompts and labels of all five programs
(`tools/text/exe_strings.py` lists them), and the title program's menu,
character gallery and story scroll. Text drawn into pictures is redrawn in
English by `tools/gfx/`: the chapter title cards (`titles.py`, committed as
`translations/en/CHAPDATA/PERIOD*.VSP`), the character gallery pages
(`ttl_pages.py`), the "Story" scroll (`story_scroll.py`) and the staff names
in the ending credits (`ttl_staff.py`, romanised; needs the Copperplate font
that ships with macOS, otherwise the names stay in Hangul) and the battle
technique icons, single Hanja in the original (`special_icons.py`, built-in
pixel font). The Hangul in the
title logo and the publisher's logo is part of the artwork and stays.
`tools/gfx/spr.py` reads and writes Softmax's sprite formats.

## Debugging aids

These are environment variables:

| Variable | Effect |
|---|---|
| `GENESIS_SHOT=dir` | screenshot every 2 s |
| `GENESIS_INPUT="ms:Key;ms:click@x,y;…"` | scripted input: keys, `click@`, `rclick@`, `move@` (hover only); x,y in game-screen pixels |
| `GENESIS_WAV=file` | dump rendered audio (raw s16le stereo 44.1 kHz) |
| `GENESIS_SETTINGS=file` | use another settings file |
| `--battle N` (argument) | start CHRSYS on battle N of BATTLE.IDX, as MAIN would |
| `GENESIS_TRACE_{DOS,FILES,ALLOC,INT,IRQ,MOUSE,DPMI,SB,OPL,PORTS}` | subsystem traces |
| `GENESIS_WATCH=lo-hi` | log writes to a linear memory range |

## Legal

*The War of Genesis* is © Softmax. This project contains only original code and
code generated from it. It does not include, and is not a substitute for, the
game.
