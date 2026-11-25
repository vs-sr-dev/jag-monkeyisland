# The Secret of Monkey Island on the Atari Jaguar — feasibility study

Status: **feasible; research only, no hardware yet.** The engine is an interpreter (SCUMM), so
the port runs the game's own bytecode: gameplay fidelity comes from the data, not from
hand-porting logic. **Base: DOS VGA floppy (v4)**, recommended in §2 and now on disk in English
and Italian (§0). Measured on that data (§8): **4.10 MiB once the unused MT-32 and speaker data
is dropped, so a 6 MB cartridge, which fits both languages (4.45 MiB).** Still to decide: **where
the save game lives** (§7).

Session 1, 2026-11-24. Sources: the JaguarDocs knowledge base (the author's own notes,
`hw/01`, `hw/04`-`hw/08`), the two most recent ports (JagGOT, JagSkyroads), ScummVM's SCUMM
engine source (GitHub master, November 2026: `detection_tables.h`, `scumm-md5.h`, `scumm.cpp`,
`sound.cpp`, `camera.cpp`, `gfx.cpp`, `players/`), and release histories (Nerdly Pleasures,
scummbar.com, exotica.org.uk, Wikipedia; URLs in §9). §0 and §8 were added the same day, once
the data arrived; their numbers are measured with `tools/lec_survey.py`. The rest of §1-§3 was
written before the data and is corrected where the measures disagree.

---

## 0. Which data, and which copy of it

Two copies of the **DOS VGA floppy release**, both known to ScummVM by the MD5 of `000.LFL`
(`scumm-md5.h`):

| folder | what | `000.LFL` MD5 | ScummVM entry |
|---|---|---|---|
| `MI_1/` | English, installed files (`DISK01-04.LEC`, `000.LFL`, `901-904.LFL`, `MONKEY.EXE`) | `15e03ffbfeddb9c2aebc13dcb2a4a8f4` | `monkey`, VGA, `EN_ANY`, DOS |
| `MI_2/` | Italian, the four 1.44 MB disk images; files dated 24 May 1991 | `66fd5ff9a810dfeb6d6bdada18221140` | `monkey`, VGA, `IT_ITA`, DOS |

- The Italian disks hold the game files as they are (no installer compression): 7-Zip extracts
  them one disk per folder, and the survey reads that layout directly.
- **The two copies have the same structure resource by resource**: same 83 rooms, 120
  costumes, 170 scripts, 90 sounds. They differ in texts and charsets (§8).
- `MI_1/passwd.txt` is a transcription of the Dial-A-Pirate wheel (§6.5).
- **Distribution model: BYOA**, as the other ports: the repository ships the engine and the
  tools, and the build reads the user's own files.

---

## 1. What MI1 is, technically

| | |
|---|---|
| Engine | SCUMM **v4** (all floppy releases) or **v5** (CD, Mac, FM Towns, Sega CD, Special Edition) |
| Screen | 320×200, 256 colours (VGA/CD/Mac/Towns), 32 (Amiga), 16 (EGA/ST) |
| Room | a background of any width (multiple of 8), drawn in **8-pixel strips**, each compressed on its own (SMAP); 1-bit **z-planes** per strip hide actors behind foreground; object images (OBIM, same codecs) are drawn over it when their state changes |
| Camera | scrolls **in 8-pixel steps**: `camera._cur.x &= 0xFFF8; … += 8` (`camera.cpp`, `moveCamera`) |
| Actors | **13** (`_numActors = 13`, `scumm.cpp`, `setupScumm`); costumes are RLE frames built from limbs, masked by the z-plane of the actor's walkbox; v4 scales actors through scale slots (`actor.cpp`, `boxes.cpp`) |
| Time base | jiffies of 1/60 s; the main loop advances `VAR_TIMER_NEXT` jiffies per step, default 4 (~15 Hz logic: `scumm.cpp`, `go()`) |
| Logic | everything in scripts: bytecode run by the interpreter (global and room scripts, ~25 slots), walkboxes and path finding, verbs and sentence, inventory, dialogue |

The game logic is data. A Jaguar SCUMM interpreter that runs the original scripts reproduces the
gameplay by construction, the same way ScummVM does. **ScummVM is the reference
implementation and the oracle** (GPL v2+: an engine derived from it is GPL as well, which suits
a BYOA project).

---

## 2. Which release to start from

### The releases, side by side

| release | SCUMM | colours / art | verbs | music without a CD | data (upper bound) | can it be bought today |
|---|---|---|---|---|---|---|
| DOS EGA (1990) | v4 | 16, EGA art | 12 + text inventory | AdLib, MT-32 (upgrade), speaker | 4×720 KB | only via the Limited Run set |
| **DOS VGA floppy (Dec 1990)** | **v4** | **256**, EGA art recoloured, close-ups redrawn | **12** + text inventory | **AdLib** (MIDI + instruments), MT-32 | **4.46 MB measured** (§8) | no (original disks) |
| Amiga (1991) | v4 | 32, VGA art reduced (strip codec 10) | 12 | **TFMX** (Hülsbeck/Stember): `music.dat` 47 KB + `sample.dat` 179 KB | 4×880 KB ≈ 3.5 MB | no |
| Atari ST (1991) | v4 | 16, EGA art | 12 | YM2149 (unconfirmed) | 4 DS disks | no |
| Macintosh (1992) | v5 | 256, VGA art | **9 + icons** | sequenced, 4 voices, sampled instruments in the executable (`Mac0`/`Mac1`) | ? | no |
| DOS CD (1992) | v5 | 256, VGA art | 9 + icons | **none**: music is CD-DA (24 tracks) | ? | no |
| FM Towns (1992) | v5 | 256 | 9 + icons | CD-DA (+ Euphony FM/PCM effects) | CD | no |
| Sega CD (1993) | v5 | 64, Amiga backgrounds | 9 + icons | CD-DA; cut scenes | CD | no |
| Special Edition (2009) | v5 | the DOS CD data, repacked in `MONKEY1.PAK` | 9 + icons | `MusicOriginal.xwb` = the CD tracks as a wave bank | PAK 1.24 GB | **yes** (Steam, GOG) |

Content also differs: the stump joke ("insert disk 22…") survives only in the DOS floppy
releases; the CD release fixes texts, adds music to Stan's and ambience to silent rooms.

### Correcting the premise: 9 verbs are v5 only

The 9-verb panel with icons belongs to the CD-derived releases. **Every release that fits on
floppies and has no CD audio uses 12 verbs (v4), except the Mac.** The floppy panel is three
columns of four (Open/Close/Push/Pull, Walk to/Pick up/Talk to/Give, Use/Look at/Turn on/Turn off;
layout to be checked against a screenshot). **The Jaguar keypad is also three columns of four**
(1-2-3 … `*`-0-`#`), so the 12 verbs map onto it spatially one to one, even more literally than
the 9. A keypad overlay printed with the verbs is exactly what the Jaguar overlays were made for.

### The music decides

- **v5 CD / Special Edition: no music without CD audio.** Its music resources are `SOUN` type 2,
  i.e. pointers to CD tracks (`sound.cpp`, `triggerSound`). Shipping the SE tracks as samples
  does not fit: even 4-bit ADPCM at 11 kHz is ~5.5 KB/s, i.e. **20 minutes of music ≈ 6.6 MB,
  the whole cartridge**. The total length of the 24 tracks is still to be measured, but no
  plausible length makes it fit.
- **VGA floppy: the AdLib music is a YM3812 driven by MIDI.** ScummVM plays it through iMUSE with
  an AdLib MIDI driver (`scumm.cpp`, `setupMusic`; Player_AD is for Loom and Indy3 only). The
  `AD` chunks of each sound resource are converted to a format-0 SMF with the instruments as
  SysEx 0x7D, plus a loop jump; **sound effects are separate `ASFX` resources played through the
  same driver, on the same nine OPL2 channels** (`sound.cpp`, `convertADResource`). The YM3812
  already runs on the Jaguar DSP, on hardware (JagSkyroads M4, reused by JagGOT).
  *Revised in session 2:* the port does not use that path. ScummVM's `Player_AD`, which it
  keeps for Loom and Indy3, is the reverse-engineered original driver and by its own comment
  the one Monkey Island DOS used; every MI1 `AD` chunk is exactly its grammar. The port plays
  the `AD` data with it, register by register (`csrc/scumm/player_ad.c`, `docs/todo.md`).
- **Amiga: TFMX.** A Paula-style sample player is light work for the DSP (~37 wavetable voices
  measured, `hw/07` §5), the whole score is 226 KB, and the arrangement is Hülsbeck's. But the
  art is 32 colours, on a machine that shows 256 at no cost.
- **Mac: 9 verbs without a CD**, 256 colours, its own sequenced score. But the instruments live
  in the executable's resource fork, the data is the hardest to obtain, and 4 voices is the
  thinnest score of the lot.

### Recommendation: DOS VGA floppy (v4)

1. **The best art of any non-CD release**, the same 256-colour backgrounds as the CD.
2. **Its audio is one OPL2**, music and effects together, which is the chip the KB already
   runs on the DSP. What is new is the AdLib driver (the original's, session 2), specified by
   ScummVM's code.
3. **12 verbs on a 12-key keypad.**
4. **The data fits**: 4.46 MB measured, 4.10 MiB without the MT-32 (`RO`) and PC-speaker (`WA`)
   data the port does not use. That is over 4 MiB before any code, so **a 6 MB cartridge**,
   with room for **both languages** (§8).
5. The full floppy content, stump joke included.

**Against it:** it cannot be bought today. BYOA means "your own disks". And the text
inventory is less handsome than the icons.

**Runner-up: Mac (v5)**, if 9 verbs and icons matter more than data availability.
**Not recommended:** CD/SE as the only source (no music); EGA, ST (16 colours); Sega CD (cuts).
**A later option, not a base:** VGA art with the Amiga TFMX score as an alternative soundtrack,
if the two share the sound numbering (same v4 scripts: to be checked).

**Settled (2026-11-24): the VGA floppy is the base**, with the English and Italian copies of §0.

---

## 3. Who does what

The criterion is the house one: **the game runs while the whole Jaguar is at work** (Keen Dreams
memory `criterio-vedere-il-ferro-intero`). It maps onto SCUMM unusually well.

| silicon | job in Monkey Island |
|---|---|
| **68000** | **the SCUMM interpreter**, in C: script VM, walkboxes and path finding, sentence and verbs, inventory, object states, camera logic, room changes, pad and cursor. Resources are read **in place from the cartridge** (6 MB, no banking: `hw/01` §1.4), so the original's resource heap, disk swaps and LRU disappear. Hot code is copied to DRAM (`hw/01` rule 5) |
| **GPU** | **On room entry:** decodes the SMAP strips of the whole room into one DRAM bitmap, the z-planes into 1-bit masks, and the object images in their current state. **Every frame:** the **costume renderer** (RLE limbs → one sprite buffer per actor, with z-plane masking and v4 scale-slot scaling, as the original does them); builds the **OP list**; runs the **blit lists** |
| **Blitter** | driven by the GPU: **text via `BCOMPEN`**, the bit-to-pixel "character painting" mode, from the 1-bit SCUMM charsets (`hw/05` §DCOMPEN: never used by our projects, so a new KB fact); verb panel and inventory; `PATDSEL` fills to clear overlays (rule 16); `drawBox`; object images over the room; dissolve transitions as block-blit lists (the JagGOT M0 pattern, ~20 µs per blit) |
| **Object Processor** | the room bitmap with the **camera as a `DATA` offset**: one SCUMM strip = 8 px at 8 bpp = **one phrase**, so scrolling is free and exact; up to 13 actor sprites (`TRANS`) in the interpreter's draw order; a text overlay limited to the rows holding text; the verb/inventory panel; the cursor; `shake` via `XPOS`/`YPOS`; optionally the aspect correction (§4) |
| **CLUT** | room palettes, palette cycling (`CYCL`), fades and `darkenPalette`, the flashing cursor, verb highlight on its own index (`hw/04` rule 10: never redraw to change a colour) |
| **DSP (Jerry)** | the **YM3812** (SkyRoads' `dsp_audio.s`) plus the **original AdLib driver** (`Player_AD`: music with loop and effects sharing the nine channels, 472 Hz; revised in session 2, it was the iMUSE subset). If the driver does not fit beside the OPL2 in 8 KB, it runs on the 68000 from a JERRY timer and hands register writes over in cells (`hw/07` rule 8) |

### Why the 68000 is the right home for the interpreter

The interpreter is branchy, large and pointer-heavy. GPU and DSP run reliably only from their
4/8 KB local RAM (`hw/06` rule 1), and their designer said they "were never meant to control the
whole system". **The Atari ST and the Amiga ran this same v4 interpreter on a 7-8 MHz 68000 that
also did all the drawing (planar, at that).** The Jaguar's 68000 runs at 13.3 MHz with no drawing
at all, so the interpreter is at home there.

### Budgets, with the KB's numbers

- **OP per line** (`hw/04` rule 7, ~1500 clk): 320 px of room at 8 bpp ≈ 164 clk; 13 actors
  ~60 px wide on the same row ≈ 13 × (10 + 30) ≈ 520; a 4 bpp text overlay ≈ 164; cursor and
  headers ~60. **≈ 900 clk worst case, 60% of the budget.** The room and the overlay are the only
  wide objects on the same row: two works, three froze SkyRoads (`hw/04` §7.3). The verb panel
  sits on rows of its own. `RELEASE=0` wherever possible (rule 14).
- **GPU per room**: the largest room (room 33, 1008×144, measured) is 145 KB of output; at
  10-20 clk per pixel through a bit reader (estimate) that is 1.5-3 M clk, i.e. **55-110 ms, 4-7
  frames**, hidden by the room transition; a 320×144 room is a third of that. **The rooms use a
  single codec family** (§8), so the decoder is one small kernel. Kernels are loaded by phase
  (room decode / per frame), as JagDoom does; per-frame kernels (costumes, OP list, blit runner)
  must share the 4 KB.
- **DRAM** (2 MB): room bitmap ≤ 145 KB (measured), z-planes ≤ 2 per room, i.e. ≤ 36 KB
  decoded (measured: ≤ 2 planes × 1008/8 × 144), 13 actor buffers ~100 KB, overlays ~45 KB,
  interpreter state (variables, bit variables, 1000 objects, arrays, script slots) < 100 KB,
  68000 hot code ~150 KB. **Under 1 MB.** Scripts, costumes, charsets and sounds stay in ROM.

---

## 4. Display questions with a Jaguar answer

- **Aspect ratio.** 320 px at `PWIDTH4` are square pixels on the Jaguar (640 at `PWIDTH2` are,
  `hw/03`). VGA stretched 200 lines to the full 4:3 height. Shown line for line, MI1 comes out
  **20% too wide**, letterboxed in 200 of 240 lines. The OP can fix it in hardware: scaled
  objects with **`VSCALE` ≈ 1.2** (`$26` = 1.1875 → 237 lines) fill the screen at the right
  aspect. The price is uneven line doubling (one line in five) and every object being a scaled
  one (~1 clk/px instead of 0.5), with `REMAINDER` set per object so that sprites stay in phase
  with the background. Both are one header field apart: **decided by looking at it**, not by
  argument.
- **Smooth scrolling** is free on the OP, but SCUMM scrolls in strips by design (`camera.cpp`).
  It stays in strips unless the user wants otherwise; FM Towns did smooth it.
- **Transitions** (iris, box wipes, dissolve, fades): fades in CLUT; wipes as OP clipping or
  `HEIGHT`/`DATA` changes on the outgoing frame; dissolve as GPU-driven block blits.
- **Palette**: 6-bit VGA → RGB16, red and blue lose one bit (the GOT finding). Judged on screen.

---

## 5. What the build does

- Reads the user's `000.LFL` + `DISKnn.LEC`: XOR-decodes them, walks the v4 containers (6-byte
  small headers, LFLF rooms), keeps every resource **byte-identical** (scripts stay
  little-endian; the interpreter reads them as the original does), and drops the `RO`/`WA` sound
  chunks.
- Lays the resources out in the ROM with an index the interpreter reads directly, aligned for
  the GPU (`hw/01` rule 1) wherever the GPU reads them (SMAP, z-planes, costumes, charsets).
- Generates the expected frames offline from the packed ROM (the Keen Dreams/GOT method), checked
  against ScummVM screenshots of the same rooms.

---

## 6. The real traps, in the order they would cost if found late

1. **Interpreter semantics.** SCUMM v4 has quirks that ScummVM documents in its code (opcode
   variants, the v4 `SO`/`AD` sound nesting, workarounds for script bugs in specific releases).
   The interpreter is written against ScummVM's v4 path, not from a generic SCUMM spec.
2. **Costume decoding and masking must be exact.** Scale-slot scaling drops pixels by a table:
   OP scaling would not reproduce it, so the GPU does it. **34 of the 81 rooms carry non-zero
   scale slots** (measured), so scaling is everyday work, not an exception. The z-plane of the
   actor's walkbox decides what hides it.
3b. **The data has quirks the reader must accept.** Sound resources nest inside each other,
   and the outer `SO` size covers part of the next one (room 10: sounds 1, 2 and 3), so sounds
   are walked flat as ScummVM does. Some directory entries in `000.LFL` point at random bytes:
   26 script, 15 sound and 6 costume slots. They are stale slots the game never loads, and the
   build must skip them, not fail on them. Sounds 150-168 are MT-32-only resources (bare `RO`
   chunks, gathered in the image-less room 94): real data, and exactly the 119 KB the port drops.
3. **The music driver is the original's** (revised in session 2; it read "iMUSE-shaped"). Its
   hardware-channel allocation between music and effects shapes what the player hears. It is
   ported from ScummVM's `Player_AD`, not reinvented.
4. **Uninitialised memory** (Keen Dreams rule): SCUMM variables start at zero in the original.
5. **Copy protection.** The floppy releases open with the Dial-A-Pirate code wheel.
   *Decided in session 9 (the user): kept.*  Not only BYOA (the owner has the wheel;
   `MI_1/passwd.txt` transcribes it): it is part of the game, and an iconic one; skipping it
   would remove a piece of the game.  The pad has to be able to answer it (docs/todo.md).
6. **Input.** D-pad = cursor, A = left button, B = right button, keypad = verbs, Pause = pause,
   Option = save/load menu; skipping a cutscene (ESC) needs a button of its own. The keypad is
   game input here, so the debug tools need another home (Keen Dreams rule: tools never on game
   buttons).
7. **PAL.** NTSC 60 Hz matches the 60 Hz jiffy; PAL needs the tick decoupled from the frame.

---

## 7. The save game: the likely real constraint

A SCUMM save is the whole VM. **1000 global objects** (measured: `0O` in `000.LFL`, 4 bytes each:
owner and state, class bits), and, by the v5 family's defaults in ScummVM (`readMAXS`; v4 has no
`MAXS` block, so its values are to be confirmed), 800 variables (1.6 KB) and 2048 bit variables
(256 B), plus actors, verbs, renamed objects, the running script slots and their stacks. Raw,
that is **~8-12 KB**: far past a 128-byte EEPROM and past a 2 KB one. **Against the initial
state** the delta is much smaller (most objects never change class), so the 2 KB EEPROM that
JagSkyroads is validating now may hold it. That gets measured on real states from the PC build
of the interpreter (M2), not argued. Options:

1. **A file on the GameDrive's SD card** (GD BIOS `FOpen`/`FWrite`, T-GD-09, never tried): no
   limit, but tied to the GameDrive.
2. **2 KB EEPROM with a delta against the initial state**, if a measured worst-case save fits.
3. Checkpoint passwords like the Sega CD's 4-digit codes: they change the game (progress lost
   since the checkpoint), so they are out under the house rules.

Decided later, after measuring real saves. It does not block M0-M2.

---

## 8. Measured on the data (2026-11-24)

`python tools/lec_survey.py MI_1 <extracted MI_2>`, English figures (Italian in brackets where
they differ).

| what | measured |
|---|---|
| data as shipped | 4 462 344 bytes (4 463 322): LEC 4 442 001, charsets 11 986 (13 949), index 8 357 |
| droppable | MT-32 `RO` 119 336 (19 chunks) + speaker `WA` 46 265 (90) |
| **kept** | **4 296 743 bytes = 4.10 MiB**: over 4 MiB before any code, so **6 MB cartridge** |
| AdLib `AD` | 93 219 bytes in 90 chunks: the whole score and all effects |
| rooms | 83 (2 without an image): 81 real, **56 of them 320 wide**; widest **1008×144** (room 33), then 960 (×2), 800, 640 (×7) |
| room heights | **144 (64 rooms), 200 (17)**: the 200-line rooms are the full-screen ones (close-ups, no verb panel) |
| backgrounds | 2 330 945 bytes compressed (`BM`) for 5 014 656 at 8 bpp (×2.15): pre-decoding into ROM would not fit, so **the GPU decodes them** |
| object images | 586 021 bytes (`OI`), 606 images |
| costumes | 786 833 bytes, 120 |
| scripts | 123 241 bytes global (170) + 148 813 room-local (`LS`) + 66 310 object code (`OC`) |
| **strip codecs** | rooms: only **15-18 and 26-28**; object images add **1** (raw) and **14**. **Nothing outside the two basic families 14-18 / 24-28 plus raw**: no transparent variants (34-48), no major-minor (64+). One decoder kernel |
| z-planes | 0 in 24 rooms, 1 in 51, **2 in 6**; 53 687 bytes in all |
| scaling | **34 rooms** with non-zero scale slots |
| global objects | 1000 |

### English and Italian on one cartridge

Compared resource by resource: **4 093 888 bytes are identical, 344 959 differ** (Italian
side): room-local scripts 125 563, global scripts 111 121, object code 58 658, a few background
strips 40 372 (painted signs), one costume 5 057, entry scripts 3 908. Both languages together
come to **~4.45 MiB** (English kept + Italian differences + Italian charsets and index), still
inside 6 MB with ~1.5 MB for engine and kernels. A language choice at boot costs nothing at run
time: each language is its own resource table pointing into a shared pool.

### Still to measure

- The size of a real save state, raw and as a delta (§7), from the PC build of the interpreter.
- How strongly the scale slots shrink actors (min/max scale) in each of the 34 rooms. This only
  sizes the costume kernel, it does not decide anything.

---

## 9. Where to start

The order that worked on Keen Dreams, SkyRoads and GOT:

- **M0 — closed on hardware (2026-11-24)**: boot; one room (the Mêlée lookout) decoded by the
  GPU from the game's own compressed strips, shown by the OP, identical to the expected frame;
  the camera scrolling across it from the pad. Details in §10.
- **M1**: the interpreter runs the opening scripts. Guybrush walks with costumes, walkboxes and
  z-planes; dialogue text over the room; title and credits.
- **M2**: verbs on the keypad, inventory, dialogue choices, object states. Part I playable.
- **M3**: audio, i.e. the AdLib driver and sequencer in front of SkyRoads' YM3812.
- **M4**: transitions and effects, saves (§7), all four parts, aspect correction decided.

## 10. M0 — closed on hardware (2026-11-24, session 1)

Room 38 (the lookout) on the user's NTSC Jaguar, as `docs/m0_expected_38.png`; the D-pad walked
all 81 rooms and scrolled the 1008-wide dock, "perfect" in the user's words.

- **The oracle**: `tools/scumm_v4.py` holds ScummVM's `drawStripBasicH/V` transcribed line by
  line; `tools/m0_ref.py` decodes every room of the packed ROM with a model of the GPU kernel
  and compares it with the oracle on every pixel: **81 rooms identical**, at every build.
- **The pack** (`tools/mi2jag.py`): 2.34 MB; each strip's bytes reversed in groups of four so
  that a long load gives the bit stream least significant bit first; the walk of both strip
  directions precomputed per room, so one GPU loop does both codecs with no multiply.
- **The kernel** (`src/gpu_room.s`, ~620 bytes of local RAM): decodes straight from the
  cartridge into the off-screen room buffer. **Cost on hardware: 81 rooms in 344 297
  half-lines, 4.25 s in all; 320×144 rooms in 60-180 ms, the 1008×144 dock in 343 ms.** A new
  colour read in one go (`and` + `shrq #k`, one variant per width) took 39% off the first
  version; a one-long-ahead prefetch of the stream only 3% more (JaguarDocs `hw/06` §12).
  The remaining cost is not worth chasing before the game runs: a room is decoded into the
  buffer that is not on screen, so in M1 the decode overlaps the fade-out SCUMM performs anyway.
- **The camera** is `DATA` = buffer + `CAM_X`, `CAM_X` a multiple of 8: one SCUMM strip = one
  phrase at 8 bpp. Room hand-over in the video interrupt: CLUT first, then the object, in the
  same vertical blank (`hw/04` rule 11).
- **Instrument** (keypad `#`): builds every room in turn and logs each cost on COM8.
- **BigPEmu** places the room at a different height depending on 2 bytes of 68000 code; the
  hardware does not (`emu/bigpemu.md` §6). The screen of record is the hardware's.
- Room 58 (`damnfores`) is a black background by design: the forest is 34 object images,
  which arrive with M1.

ROM `build/jaguarmi.j64`: the swept build (before the prefetch) `3526f06ce1e21d61…`, the final
one (with it, lookout 3312 half-lines) `c1519ff849f08001…`; logs in `build/logs/`.

### Sources

- ScummVM: `engines/scumm/detection_tables.h`, `scumm-md5.h`, `scumm.cpp` (`setupMusic`,
  `setupScumm`, `go`), `sound.cpp` (`readSoundResourceSmallHeader`, `convertADResource`,
  `triggerSound`), `camera.cpp` (`moveCamera`), `gfx.cpp`/`gfx.h` (strip codecs),
  `players/player_v4a.cpp` (Amiga TFMX), `players/player_mac_loom_monkey.cpp` (Mac),
  https://github.com/scummvm/scummvm
- Releases, interfaces, content: http://nerdlypleasures.blogspot.com/2015/10/pc-16-color-showcase-secret-of-monkey.html,
  https://nerdlypleasures.blogspot.com/2015/04/scumm-console-ports-turbo-duo-loom-vs.html,
  https://scummbar.com/game/the-secret-of-monkey-island/versions, https://scummbar.com/easter-eggs/mi1,
  https://en.wikipedia.org/wiki/The_Secret_of_Monkey_Island
- Amiga TFMX: https://www.exotica.org.uk/wiki/The_Secret_of_Monkey_Island,
  https://blogs.scummvm.org/marwanhilmi/2008/05/04/monkey-island-sound-files/
- Special Edition data: https://sourceforge.net/p/scummvm/feature-requests/708, ScummVM `NEWS.md`
  (2026.1.0: classic SE variants supported)
