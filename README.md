# jag-monkeyisland — The Secret of Monkey Island on the Atari Jaguar

A port of **The Secret of Monkey Island** (Lucasfilm Games, 1990), the DOS VGA floppy release,
to the Atari Jaguar: a SCUMM v4 interpreter for the Jaguar's 68000, GPU, Blitter, Object
Processor and DSP, running the game's own data.  The interpreter is ScummVM's SCUMM engine
transcribed to C (and its picture code to the Jaguar's processors), checked line by line and
pixel by pixel against ScummVM itself.

**Bring your own adventure (BYOA)**: this repository holds no part of the game.  The build reads
your own copy of the game's files and puts them on the cartridge image it makes; that image is
for your own use and must not be shared.

## Status

The whole game has been played through on a real Jaguar (an Atari Jaguar with a RetroHQ
GameDrive): the Dial-A-Pirate wheel, the intro, all four parts, the end credits, with the
original AdLib music.  Saving and loading work (to the GameDrive's SD card).  Open items, the
development log and how each part was checked: [docs/todo.md](docs/todo.md); the feasibility
study: [docs/feasibility.md](docs/feasibility.md).

## What you need

- **The game**: The Secret of Monkey Island, DOS, VGA, floppy release (English, or the Italian
  one).  The files `000.LFL`, `901.LFL`-`904.LFL` and `DISK01.LEC`-`DISK04.LEC`, flat in one
  folder (default `MI_1/`) or one disk per folder (`d1`..`d4`, the layout 7-Zip gives when it
  extracts the four floppy images).
- **A Jaguar and a RetroHQ GameDrive**: the ROM is 6 MB, the saves go to the GameDrive's SD card
  through its BIOS, and the log goes to the PC over its USB port (COM port).
- **Tools**: RMAC (the Jaguar assembler) and the Jaguar SDK's include files; a 68000 GCC
  (the build runs `m68k-elf-gcc` from a marsdev toolchain in a Docker image named
  `marsdev-32x`, see `csrc/build_jag.sh`); Python 3 with Pillow, numpy, pyserial and pyusb;
  for the PC checks, MSYS2's gcc.
- Copy `local.ini.example` to `local.ini` and set the paths of your machine.

## Build and run

```
build.bat [GAME_DIR]                                  the ROM: build\jaguarmi.j64
python tools\comlog.py run build\jaguarmi.j64 SECS    onto the Jaguar via the GameDrive, the log from COM8
```

Build switches (environment variables, see `tools/memory_map.py`): `JMI_PLAY=1` the game played
with the pad (otherwise a check build that replays recorded input and logs CRCs), `JMI_BOOT=0`
the real start (other values are the game's own debug jumps), `JMI_COPYPROT`, `JMI_DEBUG`,
`JMI_INPUT`, `JMI_TRACE`, `JMI_CHECKS`, `JMI_SAVEAT` / `JMI_LOADAT`.  For a game:
`set JMI_PLAY=1` and `set JMI_BOOT=0` before `build.bat`.

## Playing with the pad

| pad | in the game |
|---|---|
| D-pad | the cursor |
| A / B | left / right button |
| keypad | the verbs, where they are on screen (Open Walk-to Use / Close Pick-up Look-at / Push Talk-to Turn-on / Pull Give Turn-off); the numbers of the dialogue choices; the digits of the Dial-A-Pirate wheel (`*` starts the year again, `#` is Return) |
| C | ends the line being said (`.`) |
| OPTION | skips a cutscene (ESC) |
| PAUSE | the game's pause |
| PAUSE held + 1 / 2 | save / load (one slot, until the original's save screen is in) |
| `*` + `#` | back to the GameDrive's menu (development) |

## How it is checked

`build/pc/scumm_pc` (`sh tools/build_pc.sh`) is the same interpreter on the PC with ScummVM's
own renderer transcribed (`csrc/ref/`).  `tools/oracle_scummvm.ps1` runs ScummVM itself with
fixed seeds and recorded input and captures its trace and its screens; `tools/trace_cmp.py`,
`tools/shot_cmp.py`, `tools/input_fit.py` compare them with `scumm_pc`.  On the Jaguar, the check
builds log CRCs of the trace and of every buffer over COM8 (`tools/trace_crc.py`,
`tools/pal_crc.py`), compared with `scumm_pc`'s.  `tools/script_scan.py` reads every script of
the game (with scummvm-tools' descumm) for what the interpreter does not handle;
`tools/saveload_check.py` checks that a load is invisible to the game's logic.

## Licences

- **GPL-2.0-or-later** ([COPYING](COPYING)): what is transcribed from ScummVM — `csrc/scumm/`
  (but `keypad.c`), `csrc/ref/`, `csrc/jag/jag_gfx.c`, `src/gpu_room.s`, `tools/scumm_v4.py`,
  `tools/m0_ref.py`.  ScummVM is the legal property of its developers
  (https://www.scummvm.org).  A ROM built from this repository contains GPL code: it is GPL as
  a whole, its sources are these.
- **LGPL-2.1-or-later** ([COPYING.LGPL](COPYING.LGPL)): the YM3812 (OPL2) synthesised on the DSP,
  which follows Nuked OPL3 by Nuke.YKT (its noise generator, rhythm section and envelope rule)
  — `src/dsp_audio.s`, `tools/opl2.py`, `tools/opl2/opl2_model.c`, `tools/opl2/opl2_model.h`.
- **MIT** ([LICENSE](LICENSE)), Copyright (c) 2026 Samuele Voltan: everything else (the Jaguar
  platform, the tools, the documents).

Each source file says which in its first lines (SPDX).

## Credits

- The ScummVM team: the SCUMM engine this interpreter is transcribed from, and scummvm-tools'
  descumm, used to read the game's scripts.
- Nuke.YKT: Nuked OPL3, the reference for the OPL2 on the DSP (and the `--wav` renderer of the
  PC harness, which links it when present; not part of the ROM).
- RetroHQ: the GameDrive and its BIOS, which the ROM calls (not included here).

The Secret of Monkey Island is a trademark of Lucasfilm Ltd.  This is a non-commercial fan
project, not affiliated with or endorsed by Lucasfilm, LucasArts, Disney, Atari or ScummVM.
