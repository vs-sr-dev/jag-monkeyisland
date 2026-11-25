# JaguarMI — what to build next

State (2026-11-25, session 8): **M0 closed on hardware; M1 holds on the PC; M2a holds on
the hardware; M2b's reference renderer holds on the PC: the picture equals ScummVM's screen
pixel for pixel; on the Jaguar ScummVM's screens are transcribed (back, front, the system
screen the OP shows) and the room, objects, z-planes and costumes equal the PC's on the
whole screen, drawn by the GPU and the Blitter from a command queue the 68000 never waits
on, the text, the verbs and the cursor too; played with the pad, the clicked runs replayed and checked** (start at "Next (session 9)") (four runs, 973 screens: the lookout, a clicked run in the bar, a clicked
run from the lookout down to the dock, a room with colour cycles; see "M2b on the PC").
Earlier: (session 3) On the PC the interpreter runs the boot identically to ScummVM, plays the
music with the original AdLib driver, runs the whole intro in ScummVM's order of events, and
**every room of the game, entered through MI1's debug jump (boot param N = room N), runs idle
line for line like ScummVM** (99 of 99, variable writes included, 90 s each: 1.1 million
lines), as do nine clicked sessions. **On the Jaguar's 68000 the same C runs the lookout and
the bar line for line like ScummVM** (trace CRCs, 10 132 and 40 188 lines) with the game image
read in place from the cartridge, at 21-28 % of the 68000 on average. Read first:
`docs/feasibility.md` §3, §6 and §10.

## M2a: the interpreter on the 68000 (session 3, hardware)

- `csrc/build_jag.sh` (marsdev-32x in Docker, `build.bat` step 5): `csrc/scumm/` + `csrc/jag/`
  with 32-bit int (no `-mshort`: the interpreter is our C99), linked with the toolchain's
  libgcc, which is built for the 68000 (`--with-cpu=m68000`). 80 KB of code, 77 KB of `.bss`
  (DBLOB 256 KB, CBSS 128 KB). `csrc/jag/jag_main.c`: the libc the interpreter uses (memset,
  memcpy, a small vsnprintf), the platform hooks, the entry table (`crt0.s`, JagGOT's
  pattern: `c_init` answers `C_MAGIC`, `c_main` never returns).
- The game image: `tools/mi2jag.py` writes `cart/game.bin` (`000.LFL`, the four disks
  decrypted, the charsets; a big-endian table of offsets), and `csrc/scumm/image.c` (shared
  with the PC) keeps pointers into it. Nothing is copied: every resource is read in place.
  The 68000 is big-endian and all game data goes through `rd16`/`rd32`, byte by byte.
- **rooms.bin is gone from the cartridge** (`SHOW_ROOMS = 0`): M0's re-encoded rooms (2.3 MB)
  and the game image (4.4 MB) do not fit 6 MB together. The GPU will decode the strips from
  the game image itself (feasibility §5 said so); M0's room code assembles only with
  `SHOW_ROOMS = 1`.
- The check: phase 1 runs 900 rounds flat out with the trace on and sends a running CRC32
  every 256 lines (`T <lines> <crc>`); `tools/trace_crc.py LOG REF...` checks them against
  ScummVM's oracle and scumm_pc (the whole trace on COM8 would cost ~2 ms a line). Phase 2
  runs in real time (one round per `VAR_TIMER_NEXT` frames), trace off, and reports the time
  per round and per section of `scummLoop` (`PROF(n)` marks, nothing on the PC), four times;
  then **`Reset(2)` sends the Jaguar back to the GameDrive's stub, and the next upload needs
  no power cycle** (a ROM that stays in its loop left the GameDrive deaf to uploads every
  time). Boot room: `JMI_BOOT=N` in the environment of `build.bat` (default 38).
- Measured (half-lines per round, budget 3 150 = 6 frames at NTSC): the lookout 675 average
  (scripts 126, the costume limbs 262), the bar 875 average and 1 328 worst (scripts 568).
  The 68000 is slow on C (a trivial loop iteration ~100 cycles): the gfx usage bits rewritten
  word by word went from 320 to 45 half-lines a round. Left for later: the costume scale
  loops (~570 iterations a round at the lookout; a per-scale count table would make them
  O(1)), the 48 `drawLimb` calls a round.
- **M2b, the room (session 3, hardware)**: `startScene` calls `plat_room()`; on the Jaguar it
  writes a job for the GPU in DRAM (M0's record header and walks, then per strip the address
  of its stream in the game's own data in the cartridge, and its codec), converts the room's
  palette to the CLUT, runs `src/gpu_room.s` through `ROOM_FN` (`RoomJobC`, C ABI) into the
  buffer not on screen, and hands it to the video interrupt; the camera goes to `CAM_X`
  (`g.mainXStart`) after every round. The GPU reads the unaligned strips as aligned longs
  from the address rounded down, skips the bytes before, and byte-swaps each long (7
  instructions). The decoded buffer's CRC32 is logged (`ROOM_CRC`) and
  `tools/room_crc.py --log LOG` checks it against ScummVM's decoder: the lookout and the
  1008-wide dock identical, and identical to the eye (the user, against
  docs/m0_expected_33.png). Cost as M0's from rooms.bin: lookout 3 410 half-lines, dock
  10 707 (~340 ms; to overlap with the fade later). With the room on screen the OP shares the
  bus: the lookout's round went from 675 to 756 half-lines.
- `TimeNow` in `src/main.s` has the flaw `time_now` lost: between VC reaching VI and the ISR
  counting IRQ_TICK a reading falls a frame behind.

## M1 so far

- `csrc/scumm/`: portable C99, no allocation, every function named after the ScummVM function
  it transcribes (ScummVM commit `f0e3f05`, sparse clone at `refs/scummvm-src`).
  - `script.c` the VM; `ops_v5.c` 98 of 101 opcode functions (`opcodes_gen.h` generated by
    `tools/gen_opcodes.py`; a missing opcode halts with its name); `world.c` rooms, objects,
    `findObject`, the inventory (an inventory object keeps a pointer to its OC block: the game
    image is read-only and permanent, so nothing is copied); `loop.c` `scummLoop`.
  - `actor.c`: the base `Actor` class (v4 does not use `Actor_v3`): placement, walking along
    the boxes, turning, the animation state the scripts see (`_frame`, `_moving`, `_facing`),
    and the camera (`moveCamera`, `setCameraFollows`, `cameraMoved`).
  - `costume.c`: the classic costumes as far as the logic sees them. The loader, the limbs'
    animation (`costumeDecodeData`, `increaseAnim`, `animateCostume`), and the renderer's
    **geometry without pixels** (`drawCostume` -> `drawLimb` -> `paintCelByleRLECommon`: the
    scaled rectangle, the edge clipping, the draw flag). **Drawing reaches the logic in three
    ways**, all kept: (1) the drawn rectangle: `_top`/`_bottom` and the strips an actor covers,
    which `o5_actorFromPos` reads; (2) the draw result: an actor cut by the screen edge is
    redrawn next frame, and every redraw recomputes its scale from its box (`setupActorScale`),
    so a walking actor's speed follows its drawing; (3) the animation commands: `0x78` asks for
    the actor's sound (`playActorSounds`, which `isSoundRunning` sees). The pixels
    (`byleRLEDecode`) are the platform's: on the Jaguar, the GPU.
  - `gfx.c`: the drawing bookkeeping. The gfx usage bits (one per actor and strip, plus DIRTY
    and RESTORED), `redrawBGAreas`/`redrawBGStrip` (which strips), the object draw queue and
    `drawObject`'s strip loop, `markObjectRectAsDirty`, `setActorRedrawFlags` (an actor sharing a
    strip with another actor or a redrawn strip is redrawn: scale again), `resetActorBgs`, and
    `processActors` (the y sort, draw, animate). On the Jaguar this stays on the 68000.
  - `boxes.c`: walk boxes copied to RAM per room (scripts change their flags and scale), the
    matrix read in place until a script rebuilds it (`createBoxMatrix`: ScummVM's Kleene
    routes, not the original's "mangled Dijkstra", which ScummVM says missed shortest paths),
    `getNextBox`, `findPathTowards`, scale slots from `SA`.
  - `text.c`: messages (`actorTalk`, `displayDialog`, `stopTalk`, the message codes,
    `convertMessageToString`), `drawString` and `drawVerb` as layout without pixels, and the
    v4 charset metrics read from `90x.LFL`. The talk delay is 60 + `VAR_CHARINC` per printed
    character, and a line break replaces a space: the widths decide the timing. A verb's
    rectangle is the box of its drawn text (`findVerbAtPos`, dialogue choices).
  - `player_ad.c` + `sound.c`: **the original AdLib driver** (ScummVM's `Player_AD`, see below)
    behind ScummVM's sound queue (LIFO, as `processSoundQueues`). `ad_onTimer` is the 472 Hz
    callback; `plat_opl_write` the one platform hook.
- `csrc/pc/pc_main.c`: `--soundcard`, `--debugmode`, `--bootparam`, `--wav FILE` (Nuked OPL3 at
  49 716 Hz, writes buffered like the card's: for listening), `--opl` (register writes in the
  trace). Each loop round runs `VAR_TIMER_NEXT` jiffies of audio first (472 callbacks per 60).
  `--frames` marks the loop rounds, `--state` logs camera and message state per round, `--ego`
  the actors, `--verbs` the verbs', objects' and actors' rectangles at the end (to place
  clicks in game pixels). The trace prints `Loading room N` as ScummVM does.
- The oracle: `tools/oracle_scummvm.ps1` `-BootParam 1` (deterministic path) or `-Music adlib`
  (the intro; real time, so compare the order of events); `tools/trace_cmp.py --vars` also
  compares the `writeVar` values. **The mouse is part of the game state**: the scripts read it
  every frame (`findObject` under it), and SDL feeds ScummVM the Windows cursor whenever it lies
  over the window. So the cursor is parked off the window (right of it, half-way down: the
  bottom-right corner is Windows 11's "show desktop") and ScummVM keeps (104, 56), as scumm_pc
  does. Do not touch the mouse while it runs; a minimized ScummVM quits at once.
- **Input, line for line.** `-Clicks "T:x,y"` moves the cursor to game pixel (x, y) at T s and
  clicks; `-Burst "a,b"` captures the window frame by frame (320x240 PNGs). Real input lands on
  whatever frame it lands on, so `tools/input_fit.py ORACLE STEPS "move x y" "down L" "up L"`
  finds the frames (without an event our trace departs in the very frame ScummVM got it;
  `scumm_pc --frames` marks them) and refines the coordinates (ScummVM's window-to-game
  rounding can shift a pixel: 240 arrived as 241), writing an `--input` file for scumm_pc.
  ScummVM's own record/playback (Shift+F10, `playback.cpp`) would be cleaner, but it is enabled
  for `GID_MONKEY` (the CD) only, not `GID_MONKEY_VGA`.
- **Room sweep, hands free.** `-Offscreen` (runs without input) moves ScummVM's window under the
  taskbar as soon as it exists: the cursor cannot reach it, so the mouse stays free (after one
  jump at the start) and runs go in parallel. `tools/oracle_sweep.ps1 -Rooms "1-99" -Jobs 4`
  runs each room idle (boot param N), `tools/sweep_cmp.py [ROOMS] [--show N]` compares them
  with scumm_pc. Each run has its own `<Tag>.ini` (ScummVM writes its config back on exit), with
  `updates_check=0`: without it the update prompt holds ScummVM until someone closes it. Not
  wholly off the desktop (a window no monitor shows stalls), and clicks cannot be posted to it
  (SDL2's `TrackMouseEvent` answers a posted move with a leave that clamps the mouse to the
  window's edge in the same frame). A run differing on a mouse value (x = 319) saw the cursor:
  rerun it. Rooms 13, 22 and 24 do not exist (ScummVM asks for disk 0; scumm_pc stops cleanly).
- Reference runs (`build/oracle/X.out` + `X.in`, boot param 1), each identical to the end of
  ScummVM's run, variable writes included: `click2` (3 923 lines: a click on the beach,
  Guybrush walks, the pirate walks to him), `talk1` (7 924: the whole conversation, a line
  broken in two by the charset widths), `talk2` (10 007: a dialogue choice clicked, the
  conversation ends). Boot param 38: `look38` (7 073 lines, six clicks: Guybrush walks to the
  lookout scaled, looks at him, takes the stairs to the dock, walks right and left while the
  camera scrolls 160 -> 296 -> 176). `street35` (14 601 lines, nine clicks: Look at the clock
  and the sign, walks with the scroll, Pick up the rat), `bar28` (18 245: a walk among the
  pirates, Look at the dog and the pirates, a conversation with the dog left open) and
  `bar28b` (21 159: "Woof." chosen, the conversation runs to its end, the camera follows).
  `kitchen41` (9 891: the hunk of meat and the pot picked up, drawn as inventory verbs, Look
  at the meat from the inventory) and `kitchen41b` (10 752: Use the meat with the pot o' stew,
  the meat leaves the inventory and "meat in stew" appears).
  Plan clicks with `scumm_pc --verbs` at the frame wanted: ScummVM's frame is about
  (seconds - 5) x 10. With the music, the intro's events match per script (all 50 runScript
  in order; scripts 149 and 202 interleave by the music's timing, as they may).
- `input_fit.py` places a move that changes nothing visible (into the verb area, where the
  virtual mouse is -1) together with its click, and refines each move as soon as its click
  acted. If a run differs at frame 0, ScummVM saw the cursor before the first click: rerun.

### Decided in session 2

- **Music: the original driver, not iMUSE.** ScummVM plays MI1 through iMUSE (the AD chunks
  converted to MIDI) only to offer multi-MIDI; its `Player_AD` is the reverse-engineered
  original driver, which its own comment says Monkey Island DOS used. Every AD chunk of MI1
  (20 songs, 68 effects) is exactly `Player_AD`'s grammar: note on/off, end of track, VLQs of
  at most 2 bytes, no tempo or rhythm events, effects of at most 3 channels. So the port has
  the original's sound and the original's song timer: `VAR_MUSIC_TIMER` = 121 driver steps,
  one step per 256/ticks of the 472 Hz clock. Against ScummVM's iMUSE the intro waits run 1.6 %
  longer (826 loops of script 202 against 813), in the expected direction. Smaller, too: about
  700 lines of C that write OPL2 registers directly, which is what the DSP wants.
- Boot param N (non-zero) is MI1's debug jump: the boot passes through room 38 and lands in
  room N (1 is Monkey Island's beach, not the game's start); with null music it is
  deterministic, which makes every room an oracle.

### Found in session 3

- **`fadeOut` touches the camera**: `startScene` calls it before the room changes, and for
  v < 7 it sets `camera._last.x = camera._cur.x`. So a new room starts "not scrolled" from the
  old room's camera, and a message printed in its first frame shows at once when the camera
  lands where it was (`displayDialog` waits while the camera moves). Room 74 found it: one
  frame on a `wait for message`.
- `Actor._boxscale` is 16-bit: a box's scale may be `0x8000 | slot`, which the drawing reads.
- `o5_setObjectName` keeps a pointer to the name inline in the script (the game image is
  permanent), 50 slots as `resource_v4.cpp`; names of room objects go at a room change.

## A base for other SCUMM games (the user, end of session 3)

Once Monkey Island runs well, other SCUMM titles follow, on other versions of the engine
(MI2 and Indy4 are v5, DOTT and Sam & Max v6). This port is their base, so from here on:

- **Transcribe ScummVM's version paths, not MI1's choices.** Where ScummVM branches on
  `_game.version` or `GF_*` features, keep the branch point visible (a comment or a test on a
  game descriptor), so v5/v6 paths slot in where ScummVM has them. Today v4 is assumed in
  places silently: small headers (`rd32` sizes, 2-letter tags), the opcode table
  (`gen_opcodes.py` reads ScummVM's v5 table with the v4 overrides), `Actor` without v6
  fields, the classic costume only (no AKOS), the charset v4 metrics.
- **One game descriptor** instead of constants scattered: version, features, numbers
  (rooms, scripts, objects, actors, verbs, variables: ScummVM's `readMAXS`/defaults per
  version), the data files and their XOR key, the music driver. The interpreter and the
  tools read it; MI1 is its first entry.
- **The data pipeline per version**: v4 small headers (`LE`/`LF`/`RO`), v5 big headers
  (`LECF`/`LFLF`/`ROOM`, big-endian sizes). `image.c` and `tools/scumm_v4.py` are the v4
  readers; a v5 reader sits beside them, not inside.
- **The music is per game**: MI1 DOS plays through the original AdLib driver (`player_ad.c`);
  MI2 and Indy4 use iMUSE. The sound queue (`sound.c`) is ScummVM's and shared; the drivers
  are separate pieces behind it.
- **The GPU decoders by codec family**: rooms use ZIGZAG_V/H in MI1; v5 adds the MAJMIN
  family, v6 more. Each family its own kernel or variant, chosen per strip by its codec.
- **The tools take the game as a parameter**: `oracle_scummvm.ps1` writes `gameid=monkey`,
  `extra=VGA` today; `trace_cmp`, `sweep_cmp`, `input_fit`, `room_crc` assume `MI_1`.
- **The platform layer stays thin**: `csrc/jag/` and the `plat_*` hooks know nothing of
  Monkey Island; the renderer seams of session 4 (strips, objects, costume cels, chars,
  verbs, boxes) are ScummVM's own functions, so they hold for every version that has them.

**The ceiling (the user, session 8)**: the floppy versions of MI2 and Indy4 (v5), and with
some hammering DOTT and Sam & Max (v6); nothing beyond: Full Throttle (v7: AKOS costumes,
SMUSH video, digital speech) is out, and the CD versions belong to a Jaguar CD homebrew
(the GameDrive runs CD titles, with quirks: the whole set copied onto its SD).
**Space, the first measure of each new title**: the Jaguar sees 6 MB of cartridge
($800000-$DFFFFF; MI1 uses 4.4 of them, its image read in place).  Beyond that, two ways:
- the GameDrive's bank switching (up to 16 banks of 1 MB, the user): **never tried on
  hardware by any of our projects** (JaguarDocs metodi/gamedrive_usb.md T-GD-08: a ROM over
  6 MB that maps a bank and reads back a known value, a line on COM8).  Declared to work by
  a homebrew: SKYLAR 1.0 (shown at JagFest 2024, AtariAge post), a 16 MB bank-switching
  ROM that also reads assets through the GameDrive's file system: both ways together
  [DICHIARATO]; how it maps a bank (the register, the window, the cost of a switch) is to
  be found (Retro HQ's GameDrive documentation first), then the probe on our hardware, then a
  layout where the cartridge window holds the code and the rooms bank in;
- the GD BIOS's file functions (FOpen/FRead from the SD, ~215 KB/s, measured): after an
  FRead the next USB upload is refused until a power cycle (T-GD-11, open).
Then what a title needs at once (the largest room, a scene's costumes) against the 2 MB of
DRAM decides whether rooms are read in place, copied, or decompressed.
**The order of choice** (agreed with the user, session 8): stay within 6 MB when a title fits
(MI1 does: any cartridge, no dependency); then compression (rooms unpacked into DRAM); then
cartridge banks (a mapper is cartridge hardware, as the NES's: a banked game can still be a
real cartridge; the GameDrive emulates it); the GameDrive's SD files last, and declared
"GameDrive only" (a game that needs them cannot be a real cartridge: the real stretch, e.g.
SKYLAR's "unlimited assets").  The SD would earn its place only for the CD versions' voices
and music, i.e. Jaguar CD territory.  So the engine must not know where the data lives:
image.c / res.c become "give me room N", and in place / banked / streamed is the title's
choice, not wired in.

## M2b on the PC: the picture, pixel for pixel (session 4)

- **The pixel oracle**: `tools/oracle_scummvm.ps1 -Pixels`: OpenGL, `stretch_mode=center`, the
  1x `normal` scaler, no aspect correction, no filtering, a 320x200 window: one game pixel, one
  window pixel, the palette's 8-bit values exact (all colours of a shot are in the room's
  palette).  Not the SDL surface: its hardware screen is 16-bit (`SDL_SetVideoMode(..., 16,
  ...)`) and RGB565 loses the low bits.  The shots are cropped at ((W-320)/2, (H-200)/2)
  (`populateDisplayAreaDrawRect`).  `-Burst "a,b"` keeps only the distinct screens.
  **Compared in RGB** (two palette entries can hold one colour: indices cannot be recovered
  from a shot); the PC renderer keeps indices for the Jaguar's CRCs.
- **The cursor pin rounds down**: PowerShell's `[int]` rounds half to even, so the middle of an
  odd pixel landed one below (the old "240 arrived as 241").  `[Math]::Floor` now.
- `tools/shot_cmp.py BURST -- SCUMM_PC_ARGS`: each ScummVM screen looked up by the CRC32 of its
  RGB among our frames (`scumm_pc --shots`: every round, and every screen an effect or a mouse
  move shows inside one), in order; a miss is diffed against our frames near its time
  (`--ppm`), with the count and the box of the differing pixels, `--diff DIR` for images.
- **The screen sees input the trace cannot**: the backend draws the cursor where the mouse is
  at once, the scripts see a move at the next round.  So scumm_pc shows the previous round's
  screen again when a move arrives, and `input_fit.py` places a move the scripts cannot see 4
  rounds (the oracle's 0.4 s) before its click.  A remaining cursor miss names the round.
- `csrc/ref/` (PC only, never in the ROM): ScummVM's renderer for the v4 path, its memory
  layout included (the pitch-320 virtual screens, xstart, the z-plane buffers 40 strips wide):
  `gfx_ref.c` (virtual screens, `drawBitmap`, the codecs, `decodeMask`, `resetBackground`,
  `restoreBackground`, `drawBox`, the text surface composited by `drawStripToScreen`, the dirty
  strips, `transitionEffect`, the cursor over the screen), `costume_ref.c` (`skipCelLines`,
  `byleRLEDecode`).
- **The seams** (scumm.h, `plat_*`): ScummVM's own functions where they touch pixels, with
  ScummVM's arguments; everything the logic can see stays in the logic: the gfx usage bits
  (`markRectAsDirty`, `restoreBackground` and `restoreCharsetBg` mark RESTORED: actors on
  those strips are redrawn and their scale recomputed), the z-plane count, the costume's
  `ByleRLEData` and palette (`CelDraw`), the charset colour map, the verbs' rectangles and
  highlight, which screen effect runs.  New shared files: `palette.c` (the CLUT, the shadow
  palette, the cycles, `darkenPalette`, `setShadowPalette`, `palManipulate`, `updatePalette`
  -> `plat_setPalette`), `cursor.c` (the v5 built-in cursors and their colour cycle).
- Checked: the lookout idle (39 screens), `px28` (the bar clicked: walks, z-planes, talk,
  dialogue choices highlighted, the cursor; 531 screens), `px38c` (the lookout, the stairs,
  the dock scrolling; 324), `px65` (two colour cycles; 79), all equal and in order, their
  traces identical; the 99 rooms and the nine clicked runs still identical to ScummVM; on the
  Jaguar the lookout's trace CRCs match, 940 half-lines a round (the cursor image rebuilt on
  the 68000 every other round costs ~40).
- **Logic fixes the drawing brought** (each checked against the traces): `SO_ROOM_SCALE` calls
  `setScaleSlot`; `o5_loadRoom`, `SO_ROOM_COLOR` and `SO_PALETTE` set the redraw flags;
  `o5_doSentence(0xFE)` and `handleEffects` (after a fadeIn) clear the clicked status; room 0
  runs `displayDialog`; the costume bounds are 320 wide (pitch 320 before v7, not 328); the
  verb and cursor ops call `drawVerb`/`verbMouseOver` as ScummVM does.
- Rooms 88 and 94 have a BM of 2 bytes (height 0): ScummVM reads an LE32 there and walks out of
  the resource (94 dies, 88 survives by luck).  Here they have no picture: nothing drawn.

## Session 8: ScummVM's screens on the Jaguar (hardware, 2026-11-25)

- **The OP shows the system screen, not the front buffer** (a change to session 7's plan):
  the actors are restored and redrawn in the front buffer during a round, and an OP reading
  it would show them half done.  ScummVM's own model is transcribed (csrc/jag/jag_gfx.c,
  the bookkeeping line for line from csrc/ref/gfx_ref.c): the virtual screens (main with
  its back buffer, text, verbs, banner), each with its dirty strips, and
  drawDirtyScreenParts copying the dirty parts into the system screen, 320 x 200, double
  (SCREEN0/1): the OP shows one, the copies go into the other, and after each hand-over the
  rectangles it showed are copied screen to screen into the other one.  The back buffer
  (BACKBUF) is single now: nobody shows it.  The front buffer (FRONTBUF) has ScummVM's
  layout exactly (pitch 320, the room wrapped, the window at + xstart).  The text surface
  will be composited by the Blitter (DCOMPEN + DSTEN against 0xFD, JaguarDocs hw/05 rule 5),
  not by an OP layer with 0 transparent.
- The OP object is now the whole 320 x 200 screen: the room starts at row 16, under the
  16-row message line (black while the text is not drawn), the verbs below row 144; the
  user saw the placement right on hardware.
- **GQ_BLIT** (copy or fill, any alignment: phrase mode when both ends and the width are
  whole phrases, pixel mode otherwise; split into bands: a long blit stops the 68000) and
  **GQ_RECTS** (a list in DRAM of 3-long entries, walked by the GPU: the screen update and,
  replaying the same list screen to screen, the sync).  **GQ_STRIP** copies the decoded
  strip into the front buffer itself (copy8Col, or clear8Col with the lights off).  One
  colour table in the GPU (255 marked; a transparent strip's mask skips it): 1 KB freed.
- The checks: `scumm_pc --fbcrc` (the front buffer's window) and `--sccrc` (the system
  screen), `--no-cels` / `--no-chars` (the reference renderer without the costumes' or the
  glyphs' pixels, for the steps before they are drawn); `pal_crc.py LOG --fb|--sc`; the
  Jaguar logs `fb`/`sc` with `bb`, `z`, `pal` in the trace phase, `JMI_CHECKS` (bits 1 pal,
  2 bb, 4 z, 8 fb, 16 sc; each costs a CRC a round on the 68000).  The bar idle, 200
  rounds: back buffer, masks, front buffer, system screen, palette and trace all equal the
  PC, BigPEmu and hardware.
- **Mistake of mine, seen by the user at once**: the WIDTH field of 320 written by hand as
  $19 (80 pixels): tiny broken copies of the room.  The field is `(4 + m) << (e - 2)`: 320 is
  e = 8, m = 1, $21; now computed by blit_pitch / memory_map's blit_field, never by hand.
- **Measured: on the 68000 every command costs** (the bar, hardware, half-lines a round of
  3 150): session 7 1 479; the first front buffer 1 905 (31 GQ_BLITs a round); the Blitter
  not started 1 872 (the blits themselves: ~30); q_blit returning at once 1 496.  So ~8
  half-lines a command were the 68000 writing it (a libgcc `__udivsi3`, `pitch_flags` calls,
  the CLUT copied at every hand-over, a byte `memcpy`): the 68000 here runs at a fraction of
  its speed (the bus is shared with the GPU, the Blitter and the OP; even two reads of the
  clock per call cost ~2.5 half-lines).  Without them: 1 760.  Then the work moved to the GPU
  (GQ_STRIP's own front copy, GQ_RECTS).
- **The costumes on the GPU** (GQ_CEL, src/gpu_room.s gq_cel): byleRLEDecode as
  csrc/ref/costume_ref.c has it (skipCelLines, the scale table, v4's x step, the z-plane's
  bit, the bounds).  plat_drawCel fills a block of CELBUF (CEL_* offsets from
  tools/memory_map.py, one truth for C and RMAC) with the addresses worked out (destPtr,
  getMaskBuffer(0, y, zbuf)) and the palette; the GPU copies it into its RAM, reads the RLE
  from the cartridge as aligned longs, the mask byte and the scale table with loadb from
  DRAM (T-GPU-11).  Checked on hardware with the costumes drawn (scumm_pc --no-chars only):
  the bar (120 rounds: front buffer, system screen, back buffer, masks, palette, trace all
  equal; against --no-cels it fails: the actors are there), the lookout (actors scaled 128,
  239, 241: 80 and 80), room 8, the ship's hold (scale 171, two z-planes: 80 and 80; the user
  saw Guybrush scaled right and behind the right things).  GPU RAM: 3 692 of 4 096 bytes.
- Time, hardware, half-lines a round of 3 150, the costumes drawn: the bar 1 630 (session 7,
  without costumes, 1 479), the lookout 1 381 (session 6: 964), room 8 822.  The lookout's
  rise is not measured yet: the cels a round, plat_drawCel's 26 longs each, the GPU drawing
  beside the 68000 (start item 4 below from there).
- **BigPEmu leaves the cycle from session 9** (the user): the hardware is the reference and
  COM8 carries the same log; if something works on hardware and not in BigPEmu, it is
  BigPEmu's problem.  tools/emu_log.py stays as a tool, not a step.

- **Text and verbs on the GPU** (GQ_CHAR, gq_char: drawBitsN, 1/2/4/8 bpp, one bit stream
  across the rows, the colour map in the command): the glyphs go to the text surface
  (TEXTSURF, 0xFD = none) over the room, or into the text/verb virtual screens; the charsets
  are copied into DRAM at boot (CHARSETS) so the GPU reads them with loadb.  The composite:
  GQ_RECTS follows each copy with the text surface's rectangle (DCOMPEN + DSTEN, phrase,
  B_PATD $FD), skipped while the surface is all 0xFD (textClean: the same picture).  The
  text-surface fills of restoreBackground / drawBox / restoreCharsetBg as gfx_ref.c.
  Checked on hardware against scumm_pc without --no-chars: the bar (the verbs: 60 screens),
  room 30 (the store: the storekeeper talks from round 10, 47 rounds; 24 screens over 80,
  trace equal); the user saw the text in its purple.  The PC's glyph census (session 8): idle
  rooms draw only the verbs (1 bpp, 8 high, ~180 at room entry); talk goes to the text
  surface at 2 bpp; idle rooms with talk: 0, 2-6, 15, 19, 30, 32, 34, 45, 51, 57, 71, 72, 74,
  80, 85 (scumm_pc --state prints "mask 1" while it is up).
- **The cursor**: a second OP object (TRANS, 16 x 16, 8 bpp) over the screen, handed over
  with it (NEXT_CUR, NEXT_CURX/Y: the mouse minus the hotspot); the ISR clips it at the top
  (DATA moved on: the OP does not clip there) and the bottom.  The image goes into one of
  three CURSORn buffers by the Blitter (a fill with 0, then the logic's image copied with
  DCOMPEN + DSTEN against 0xFF), only the columns left of x = 320 (the NTSC display shows
  352 pixels).  The data: the v5 built-in cursors in colours 15, 7, 8, never 0 (OP TRANS is
  safe).  `scumm_pc --curcrc` / `pal_crc.py --cur` (bit 32 of JMI_CHECKS): the bar, 22
  cursor states equal on hardware, the user saw the colour cycle (white, two greys).  Cost:
  the bar 1 642 -> 1 695 (a hand-over every other round for the colour, two blits).
- **GPU RAM is full**: 4 064 of 4 096 bytes.  The next GPU kernel needs room first: code
  loaded on demand (an overlay copied by the Blitter into the GPU's RAM: JaguarDocs hw/05,
  the fastest way in), or a table moved to DRAM.

- **Input on the Jaguar**: `JMI_INPUT=build/oracle/X.in` builds the oracle's clicks into
  the ROM (tools/input_table.py -> csrc/jag/input_table.h), applied before the round they
  name (as scumm_pc --input), in the trace phase and again in the time phase.  look38 on
  hardware (360 rounds: Guybrush walks, the stairs, the dock, the camera 160 -> 296 and back):
  272 system screens, 272 front buffers, 34 mask buffers (the scroll with z-planes), 138
  cursor states (the mouse in the CRC now), the trace (6 417 lines) all equal the PC.
- **The pad** (`JMI_PLAY=1`: no trace phase, endless): the video interrupt reads the pad
  (ReadPad, Keen Dreams') and moves MOUSE_X/Y (1 pixel a frame, up to 4 held); the cursor is
  drawn there every frame (the hotspot comes with the image); A / B the buttons, C the '.'
  key, OPTION ESC (presses counted by the interrupt: none lost between rounds); '*' + '#'
  back to the GameDrive's stub.  The user played several rooms (LeChuck's ship included).
  EDGE: a black 16-bpp object right of the screen (the NTSC display shows 352 pixels; the
  cursor clipped at 320 as ScummVM's), the border black once a picture is up.
- **Logic**: processKeyboard ('.' ends the line: _talkDelay = 0; ESC: abortCutscene and
  VAR_CUTSCENEEXIT_KEY; both mouse buttons are ESC in v4, as ScummVM; Return / Tab clicks)
  and **dissolveEffect** (128 = 8 x 4 in DOS, 134, 135: the order from the game's random
  source, in the logic; plat_dissolve draws it: the PC reference block by block with its
  waits, the Jaguar its last picture for now).  The user hit one at the Loom pirate (it
  stopped the game before); the five clicked runs still equal ScummVM's traces.
- **The "jumps" the user saw (dialogues, the bar's scroll) were the OP list re-armed late**:
  blits back to back stop the 68000 inside the video interrupt.  Two spies in the time
  report: op-miss (the rows on which the OP did not reach EDGE, the list's last object as
  tall as the screen: Keen Dreams' spy), and how late after VI the list was armed (VC wraps
  at 524; the screen starts 82 half-lines after VI).  bar28b replayed in real time: before,
  armed up to 131 half-lines after VI and rows lost; with ISR_BUSY (set by the interrupt,
  the GPU starts no blit while it is up) 34 at most, no row lost, the same round times.
- GPU RAM: 4 084 of 4 096 bytes.
- Rooms entered: rounds of 10 000 - 28 000 half-lines (up to 9 frames x 6): the game stops a
  moment at room changes; to measure (item 4 below).

## Session 9 (hardware, 2026-11-25)

- **The keypad is the verbs** (csrc/scumm/keypad.c, the port's own): a key becomes the key
  the game gave a verb (SO_VERB_KEY; MI1 EN: o c s y / w p t g / u l n f), so the press runs
  the original keyboard shortcut through checkExecVerbs, as a PC keyboard would.  Which verb
  comes from the verbs up, not a table (the Italian letters differ, the places do not): if an
  active verb's key is a digit (the dialogue choices: '1' '2' ...), the keypad sends its own
  digit; otherwise the active keyed verbs' left edges and tops, sorted, make the grid (3 x 4
  in v4; a v5 nine-verb panel takes the top three rows).  "Active" as checkExecVerbs: in a
  dialogue the 12 verbs are saved (saveid 1).  ScummVM's processKeyboard for v4 does nothing
  else with a letter or a digit (input.cpp), so a key equals a click on its verb.
- The video interrupt queues each keypad press (KEYQ, KEYQ_HEAD: ISR only); pad_input takes
  one a round after C and OPTION (ScummVM's _keyPressed is one key): none is lost.  The input
  files take "N key ASCII" and "N keypad K" (scumm_pc --input, tools/input_table.py).
- Checked: talk2 with "keypad 1" for the clicked choice equals the click; bar28 with "keypad
  6" for the click on Look at equals ScummVM's whole trace (18 245 lines) on the PC, and on
  hardware 200 rounds (9 270 lines, 37 checkpoints) equal ScummVM's and the PC's.  The user
  played the verbs and the dialogue choices on the pad (JMI_PLAY): all working.
- '*' + '#' stays the way back to the stub until the repository is published (the user: no
  second pad yet; the shortcut goes before a release).  The '*' reaches the game first as
  Pull: harmless.
- `scumm_pc --verbs` prints each verb's key and saveid.

- **The audio: MI1's AdLib music on the Jaguar.**  The original driver (player_ad.c, 472 Hz)
  on the 68000, JagGOT's OPL2 on the DSP (src/dsp_audio.s, its C model and checks in
  tools/opl2/), between them a ring of JagGOT's stream words in DRAM (AD_RING: reg << 8 | val,
  $0001 the end of a tick, $FF00 back to the start).  **The DSP is the clock**: it stops where
  the ring is written to (MUS_WR, whole ticks) and counts the ticks played (MUS_PLAYED), the
  ticks it found nothing (MUS_STALL) and where it has read (MUS_RD).  The video interrupt
  (CAudio -> C_ENTRY_AUDIO -> csrc/jag/jag_audio.c c_audio) runs ad_onTimer until it is
  AD_LEAD (24) ticks ahead, ~8 a frame; the main loop's own calls (a script starts a sound)
  write into the same tick buffer under Player_AD's mutex (plat_ad_lock -> AD_LOCK, held where
  ScummVM holds _mutex), and the interrupt leaves the driver alone while it is held.
  JagGOT's voice is gone from the DSP (no digitised sound in MI1 floppy, and Jerry's 8 KB
  wanted the room: 8 056 bytes now; v5's voices will need it back somehow).
- Checks: `tools/opl2/dsp_check.py [SECS] [ROOM]` (build step 4b, 0.5 s of the bar): the DSP
  binary in the JRISC interpreter against opl2_model.c sample for sample, the stream from
  scumm_pc --opl grouped into ticks as jag_audio.c groups them, the model being the 68000
  too (lead, a wrap, four late frames: stalls, a hold); MUS_PLAYED/STALL/RD equal at the end.
  `JMI_SOUND=3` (default with PLAY) puts the AdLib in a checks' build: the trace phase runs the
  driver as scumm_pc (jiffies x 472 / 60 before each round) and logs "A lines crc" of the
  "OPL tick reg val" lines: `tools/trace_crc.py --opl LOG REF` against `scumm_pc --soundcard 3
  --opl`.  On hardware: the bar (7 127 writes) and room 77 (LeChuck's ship, the skeletons'
  band: 6 009) equal the PC, and their script traces with the AdLib too.  The user heard the
  bar: clean.
- **SCLK 40 (10 134 Hz), not JagGOT's 36**: at 36 the bar starved the I2S ~10 times a second
  (the user heard nothing, but it would grow).  SND_SPIN told why: the DSP ~18 % idle on
  average (35 clk a spin), so not the bus but bursts (nine channels and the drums) longer than
  the 64-sample ring; the hardware costs ~1.17 x dsp_check's estimate (`DSP_SLOW=1.17`
  replays the ring with that), which reproduced the starving and found 40 the first rate
  without it (26 samples left at worst; rooms 45, 77, 8, 51 too).  On hardware at 40: the bar
  and room 77, starved 0, 471.6 ticks a second.  The time report's "audio:" line gives each
  counter per report.
- Cost: the driver on the 68000 ~400 half-lines a round (the bar 1 600 -> 2 000 of 3 150;
  room 77 2 386).

## Session 10 (2026-11-25): what stands between the port and the end credits

- **The static scan** (item 0a): `tools/script_dump.py GAME_DIR OUT DESCUMM` writes every
  script of the game (global SC by the index's 0S number, rooms' EN / EX / LS, objects' OC)
  as its own block and descumm's listing beside it; `tools/script_scan.py` reads the listings
  of both languages and reports the opcodes used and not implemented, where each path that
  stops (or stopped) the ROM appears, and every sub-opcode the game uses per family.
  descumm is scummvm-tools' (GPL, a PC tool only): cloned into
  `refs/scummvm-tools` (16f178f) and built with MSYS2's g++ into
  `descumm.exe (DESCUMM in local.ini)` (the sources listed in its Makefile.common
  descumm_OBJS + tool.cpp, version.cpp and common/, no config.h).  1 651 scripts a language,
  all read to their end.  The Italian disks are extracted into `build/MI_2x/d1..d4` (7-Zip).
  **What the data says** (English and Italian alike, no sub-opcode only one of them uses):
  - not one SO_LOAD_OBJECT (so no flobject is ever loaded: o4_pickupObject of an object not
    in the room returns, as ScummVM's), no saveLoadGame / saveLoadVars, no image verb, no
    verb with an object, no SO_ERASE / SO_SAY_VOICE, no cursor image, no room op other than
    scroll, colour, screen, palette colour, shake on / off; every effect number in range;
    every sub-opcode used has its case in ops_v5.c; our tables' sizes are ScummVM's for v4;
  - the one opcode used and missing was **o5_systemOps(3), quit**: script 152, the third
    wrong answer at the Dial-A-Pirate wheel (sc_001's is in the debug boot path);
  - **banners** (print(252, text): showMessageDialog): the stump in the forest (room 58,
    object 660, Look at: "Insert Disk 22 and Press Button to Continue." three times) is the
    one a player meets; the rest are the verb script's keys (sc_004: F1 machine speed, ')'
    one / two button interface, '?' machine rating, **Ctrl-W "Are you sure you want to win?
    (Y/N)"**, 'y' starts script 127, the ending), not guarded by the debug mode, and debug
    messages ("THIS IS BAD", "Pirate id is invalid");
  - room 98 and 130 / 215 are pseudo-rooms (PseudoRoom: 215 is the Melee forest, room 58),
    already handled; ShakeOn in rooms 11 and 53 (the port shows no shake yet);
  - descumm's "unknown8(8224)" in dialogue choices is a text's code 8 (a newline in a verb:
    drawString's case 8) followed by two spaces, and "x88" a character in a text.
- **The wheel, from boot param 0** (item 0b): sc_001 starts script 152 when boot param 0,
  then 149 (the intro).  Script 151 says "Press ENTER to continue" and takes any input
  (its input script 153 sets Var[105] = -1 for anything but a verb click); then 152 asks a
  year: input script 155 takes '0'..'9' (Var[105] = the digit) and Backspace (8: start the
  year again), four digits; three wrong years: systemOps(3).  The answer is Var[266]
  (string 29's table, from getRandomNr drawn VAR_TMR_1 times after the Enter: so it depends
  on the round of the Enter).  '.' would skip in debug mode, but processKeyboard eats '.'
  (it ends the line being said) in ScummVM and here: never reached.
  - `g.copyProtection` (scumm_pc `--copyprot N`, build switch `JMI_COPYPROT`, on by default
    with PLAY): without it startScript(152) returns, as ScummVM's copy_protection=false;
  - o5_systemOps: SO_QUIT sets g.quit; the round runs on (the scripts to their breakHere),
    scummLoop returns after checkAndRunSentenceScript, as ScummVM (_quitRequested at once,
    shouldQuit there); scumm_pc prints "QUIT (step N)", the ROM logs "QUIT (round N)" and
    goes back to the GameDrive's stub (for a release: a restart of the cartridge, to decide);
  - VAR_DEBUGMODE as a switch too (`JMI_DEBUG`): 0 only for PLAY from boot param 0 (the
    game as sold, which also starts script 156: a random number every round); a debug
    jump needs 1 (without it boot param N quits with "Debugging mode not active");
  - keypad.c: no active verb with a key at all = a keyboard: the digits, '*' Backspace,
    '#' Return;
  - the oracle presses keys: `oracle_scummvm.ps1 -Keys "T:ASCII;..."` (keybd_event, ScummVM
    brought to the foreground; 1-26 are Ctrl + the letter) and `-CopyProtection`.
  - Checked: wheel4 (Enter, then 1234, 1 Backspace 1234, 1234: three wrong years, quit):
    ScummVM's trace 5 792 lines = scumm_pc's with the keys and with the keypad (wheel4k.in:
    '#', digits, '*'); on hardware (JMI_BOOT=0 JMI_COPYPROT=1 JMI_INPUT=wheel4k.in) the
    same 5 793 lines, 23 checkpoints, "trace: 670 rounds, QUIT".  wheel5 (-Pixels): all 34
    of ScummVM's screens equal scumm_pc's frames.
- **The message banner** (showMessageDialog -> showOldStyleBannerAndPause, ScummVM's
  original GUI, its default): text.c transcribes it (the surfaces kept, restoreCharsetBg,
  charset 1, the banner virtual screen at 80, 320 x 12, two lines and the text centred in
  string slot 5, drawDirtyScreenParts + the banner's update, the wait, the banner cleared,
  everything back, _postGUICharMask, VAR_KEYPRESS); gfx.c drawLine / drawPixel;
  findVirtScreen forced to the banner while it is drawn; clearClickedStatus resets the key
  as ScummVM's does.  The pixels and the wait are the platform's: plat_saveSurfacesPreGUI /
  plat_restoreSurfacesPostGUI (the text stamped onto the main screen), plat_fillVirtScreen,
  plat_drawPixel, plat_setDirtyRange, plat_updateDirtyScreen, plat_drawDirtyScreenPartsOnly,
  plat_pauseEngine, plat_waitBannerInput.  scumm_pc: the banner's screen is a shot "N.k",
  and the input event after the round's own ends it (input_fit places it there).  Jaguar:
  three DRAM buffers (GUI_TEXT / MAIN / VERB), Blitter copies, the stamp as the text's
  composite (B_TEXT against 0xFD), drawPixel by the 68000 after gq_drain (the engine is
  paused), a hand-over now (gfx_present), the pad read frame by frame (A / B a click, C '.',
  OPTION ESC, the keypad's key), the cursor animated every 4 frames; the music held (AD_LOCK
  + MUS_HOLD: JagGOT's hold keys the notes off, ScummVM's mixer pause does not: a held note
  does not come back after a banner).
  - Checked on the PC: win38 (the lookout, Ctrl-W, 'y'): ScummVM's trace 2 124 lines equal,
    and **all 86 of ScummVM's screens equal ours, the banner and every screen of the end
    credits** to "Turn your computer off and go to sleep!" (script 127, round ~1 008).
  - Checked on hardware (JMI_BOOT=38 JMI_INPUT=win38.in): the trace of the banner and the
    whole ending equals the PC's (2 137 lines, 1 300 rounds), then the real-time phase and
    back to the stub; with JMI_CHECKS=63 over 200 rounds, the screen the banner shows
    ("banner 72 crc": gfx_present logs it, scumm_pc --sccrc prints it) equals the PC's, and
    the palette, buffers, front buffer (80 states), system screen (80) and cursor too.
  - The Italian release keeps the Ctrl-W banner ("Vuoi vincere la partita? (S/N)") but no
    longer reads the answer: no ending by Ctrl-W there.  The debug key '*' (42, debug mode)
    starts script 127 in both: on the PC the Italian credits also run to their last loop.
  - The wheel's checks build on hardware (CHECKS=63, 670 rounds: ~3 s a round, ~33 min):
    trace, palette, buffers, 34 system screens equal the PC's.  comlog's 1 600 s were not
    enough: the rest was heard with `comlog.py listen` (never an upload in between), and
    the two rounds lost at the seam show as a cursor mismatch only.
- `tools/oracle_scummvm.ps1`: click1 (session 2's first click) differs from today's
  scumm_pc at line 2 783, as it did before this session: click2 is the same click redone.

## Next (session 11): start here (items 0a, 0b done in session 10, above)

1. ~~The keypad: the verbs~~ (done, above).  The inventory scroll: seen working on hardware
   by the user in the first play-through (session 10: the arrows appear and scroll); no
   oracle run has it yet.
1b. ~~The audio~~ (done, above).  Left: by ear against `scumm_pc --wav` (Nuked) in the
   heavier rooms (77, 45, 51); pause (MUS_HOLD is there, unused: SCUMM's pause is a banner);
   the driver's ~400 half-lines a round on the 68000 into item 4's account; if a song ever
   starves at SCLK 40, the DSP's channel loop is the work (not a lower rate).
0. **First of all: what stands between the port and the end credits** (the user, end of
   session 9: "can it be played, at face value?" - not yet).  The ROMs so far start with a
   debug jump (JMI_BOOT), the game was never played through (not even on the PC), and some
   paths of the interpreter stop the ROM (plat_error / halt: FATAL or HALT on COM8).  So:
   a. **A static scan of every script of the game** (global scripts, room scripts, objects'
      verb scripts, both languages): where the opcodes and sub-opcodes that stop us today
      appear - SO_LOAD_OBJECT and addObjectToInventory of another room's object
      (flobjects, the likeliest to block a game), showMessageDialog banners, SO_VERB_IMAGE,
      the verb's assigned object, SO_ERASE, SO_SAY_VOICE, o5_systemOps (restart / quit:
      the end?), o4_saveLoadGame / o4_saveLoadVars, SO_ROOM_SAVEGAME, the unknown
      subopcodes.  The list of what is missing comes from the data, not from what the runs
      happened to click.  Then each path transcribed from ScummVM and checked against it
      as usual (an oracle run that reaches it).
   b. **The start, from boot param 0**: the intro, then the **Dial-A-Pirate wheel, which
      stays** (the user: part of the game, an iconic one, not only BYOA; feasibility.md 5).
      The pad must answer it: find in the scripts how the screen reads its input (the
      year typed: digits, then Return? kKeyClickArea in the input script), and give the
      keypad its digits there (keypad.c's digit mode is keyed on the dialogue choices'
      keys today; on that screen no verb has a key) and a button for Return / backspace if
      it takes them.  Checked against a ScummVM run with the same keys (the oracle needs
      keys: keybd_event in oracle_scummvm.ps1, "N key ASCII" in input_fit / scumm_pc).
   a, b: done in session 10 (above): the scan, the wheel, quit, the banner, the credits.
   c. **Done (session 10, the user on hardware): the whole game played**, parts 1-3 from
      boot param 0 (the wheel, ~2 h 15 of play), part 4 from debug jump 1212 to the end
      credits.  Found and fixed on the way: the keypad with an inventory (c2), the cut
      dialogue choices (c3), the church (c6, fixed on the PC, not replayed on hardware since).
      Left: the jumps of the picture (c1), the slow flashes and the catacombs (c4, c5).
      The user: considered done.  The original text of the item follows.
   c (as written). **A whole play-through**, the Jaguar on COM8 (any FATAL / HALT logged), part by
      part: the PLAY ROM from boot param 0 (`JMI_PLAY=1 JMI_BOOT=0`: the wheel, debug mode
      0, the AdLib).  What the scan cannot see is left: limits of the port's own (the
      68000's heaps, the resource caches), and time.  The debug jumps of sc_001 (boot
      params 6767, 444, ... with their inventories) reach the later parts without playing
      the earlier ones.
   d0. **Saving, step 1 done on the PC (session 10)**: `csrc/scumm/saveload.c`, ScummVM's
      saveLoadWithSerializer and loadState for a v4 game: one serializer both ways
      (little-endian fields of their C width, the same file on the PC and the Jaguar;
      pointers into the game image as (disk, offset)), g field by field and each module's
      state (loop: initScreens' b / h and the mouse; boxes, which matrix and the scale
      slots; gfx usage bits; the charset renderer and the message being said; new names;
      the sound queue; Player_AD as ScummVM's: the songs, timers and position, the music
      started again and musicSeekTo'd with its key ons muted, then its notes on).  Taken
      at handleSaveLoad (loop.c, between the music timer and runAllScripts); a load
      rebuilds as loadState (sounds stopped, setDirtyColors, the room's static data, the
      screens, a complete redraw, the text surface cleared, cameraMoved, initBGBuffers,
      updateCursor and the mouse warped) and ScummEngine_v5's redrawVerbs.
      Check: `tools/saveload_check.py STEPS N,... -- scumm_pc args` saves at round N in a
      run and loads it at N in a fresh scumm_pc: the trace after the point must be the
      same (scumm_pc --save-at N FILE / --load FILE N), and the screen's CRCs are compared
      apart (a load clears the text being said, as ScummVM's: the screen is the same again
      when it is said anew).  **36 save points, the logic identical in all**: bar28,
      talk1, talk2, kitchen41, street35, look38, px38c (null music), bar28 with the AdLib,
      the user's part 4 replayed (5 points, up to 97 298 lines after the load), the wheel,
      the banner and the ending; the screen equal again in all; the AdLib's 10 276 OPL
      writes after a load in the bar equal the continuous run's.  Found on the way: the
      box matrix pointer (the room's own or createBoxMatrix's) and the cursor's image to
      the platform.
      **Size**: 86 KB raw; against the boot state (XOR) and zlib 1.6 KB early to 4.5 KB in
      part 4 (RLE of zeros only: 2-7.5 KB).  So not the 2 KB EEPROM: the GameDrive's SD
      card (GD BIOS FWrite, never tried yet: T-GD-09), with the user's agreement to require
      the GameDrive.  Next: FWrite on hardware, the save in DRAM -> SD and back; then the
      pad (Pause the game's own pause; held, the menu) and the original's menu (F5).
   d1. **The SD card from the ROM (session 10, hardware)**: src/gd.s GdCallC (C ABI, all
      registers but d0 kept, a pending log answer collected first) behind GD_CALL_FN.
      `JMI_SDTEST=1`: FOpen mode $0A (FatFs WRITE | CREATE_ALWAYS) -> handle 0 (~120 ms),
      **FWrite of 64 KB works** (~1.46 s, ~45 KB/s), FClose ~15 ms, FSize as written;
      `JMI_SDTEST=2`: FRead of it ~1 s, the CRC as written (T-GD-09 closed for FWrite).
      **The game saved to the SD card and loaded back on hardware**: plat_saveLoad on the
      Jaguar writes /JMISAVE0.SAV raw (SAVE_BUF); `JMI_SAVEAT=200 JMI_LOADAT=500` in the
      bar's replay: saved (85 953 bytes, ~3.3 s with the serializer and a CRC), loaded
      back 300 rounds later (~1.3 s), and the Jaguar's trace equals scumm_pc's (--save-at
      200 F --load-at 500 F) over all 24 590 lines, 97 checkpoints.  Every ROM that
      FReads wants a power cycle before the next upload.
      With every picture check (JMI_CHECKS=63, saved at 100, loaded at 160, 220 rounds of
      the bar): palette, room buffer (220), z-planes (65), front buffer (220), system screen
      (220), cursor (86) and the trace (10 898 lines) all equal the PC's across the load.
      The file packed: "JMIZ", the state's size, runs of zeros as 0, n: 13-15.5 KB (~0.35 s
      to write); the save/load checks unchanged with it.
   d2. **The pause** (session 10): processKeyboard's pause key with the original GUI
      (VAR_PAUSE_KEY, ' ' in MI1): the cursor off, "Game Paused.  Press SPACE to Continue."
      (string 4) in the banner, colour 12; checked against ScummVM (pause28, the space at
      round 118 and again: all 79 screens equal; the trace does not see a pause).  The pad:
      PAUSE let go within a second = the pause key; PAUSE in a banner closes it (its
      release no pause again).  **Decided with the user**: the original's save screen (F5)
      transcribed, the names made automatically for now; opened by holding PAUSE.  Until
      then a shortcut: PAUSE held + keypad 1 saves to /JMISAVE0.SAV, + 2 loads it; the music
      held while the SD card is used.
      **Played by the user on hardware**: saved by the dock (12 695 bytes, ~2.5 s), went into
      the Scumm Bar and took the meat, loaded (~1.6 s): back at the dock without it, the
      bar's music stopped as it should.  The time is mostly the 68000 serializing and
      packing 86 KB and a CRC for the log only (the SD card ~0.4 s): to trim.  Still to see
      on hardware: a save and a load both with music playing.
   d. **Saving: a question for the user** (the game is hours long).  MI1 saves through the
      interpreter's own menu (F5, VAR_MAINMENU_KEY 319; ScummVM's original GUI draws it),
      none from the scripts.  Where would a save go on the Jaguar (the cartridge's EEPROM
      is 128 bytes; the GameDrive's SD card; a password-like code is not MI1's), and what
      is in one (ScummVM's saveload.cpp: the whole engine state)?
      The user (end of session 10): the GameDrive supports saves up to 2 KB, likely not
      enough if MI1's save is the whole VM.  An idea to weigh later: whoever plays this on a
      Jaguar will have a GameDrive anyway, so requiring it and writing a larger save than a
      cartridge game would is acceptable, though not the most faithful to the hardware.
      First step when it comes up: measure what a save must hold (the state the scripts can
      see: variables, bit variables, objects' owner / state / class, inventory, actors,
      script slots, verbs, strings, the room), against ScummVM's saveload.cpp.
   c1. Seen in the first play-through (the PLAY ROM, boot param 0): one jump of the picture in
      the intro (room 10, rounds 2100-2200): op-miss rows 27 in one frame, the interrupt
      149 half-lines late, the list armed 178 half-lines after VI (the window is 82; session
      8's ISR_BUSY brought the worst to 36 in the runs then).  Something in that scene holds
      the interrupt off that long (a blit, or a section with the interrupts off): reproduce
      with an input replay of the intro in real time and the spies.  The time reports there:
      worst 6 983 half-lines a round, 29 rounds late of 200.  Again, milder, at the Melee
      map (room 85) going to room 43 (second play-through, rounds 7900-8000): the interrupt
      105 late, the list armed 120 after VI, no row missed, a 21 189 half-line round (the
      room change) in the same block: the user saw a small jump.
   c2. Found in the first play-through, fixed: the keypad's verbs stopped with the first item
      picked up.  Script 22 gives the inventory's six visible items the keys '1'..'6' (verbs
      101-106) and its arrows 'q' 'a' (107, 108, x 149); keypad.c's digit rule came first
      and took the items for dialogue choices.  Now the grid is made of the letter-keyed
      verbs (the leftmost three columns, the topmost four rows among them), digits only
      with no letter-keyed verb up (a dialogue: script 17 saves verbs 1-13 and 101-108),
      a keyboard with none.  Checked on the PC: boot param 444 (two items) every keypad key
      equals its verb's letter; talk2 with "keypad 1" for the choice equals ScummVM's
      10 007 lines; wheel4k unchanged.
   c3. Found in the first play-through, fixed: a dialogue choice of the Fettucini brothers
      cut at "...What sort of idiot do you ta", in the list and when said (getVerb reads the
      verb's name).  VerbSlot.name was 64 bytes, MI1's verb texts reach ~111 (a choice with
      its newline code); the actor names were 32, the Italian "i pirati dall'aspetto
      importante" is 33.  Now STRING_MAX (256) and 64, and a name that does not fit is a
      FATAL, not cut in silence.  Lengths measured from the listings of both languages.
   c3b. Dialogues with more than six choices (script 17, mode 6): the choices are made
      again at x 16 WITHOUT keys, and two arrows 109 'q' / 110 'a' at x 0 scroll them: the
      original has no number keys there either.  The keypad does the original's: the arrows
      are the grid, '1' scrolls up, '4' down (the user checked it).  Decided with the user:
      stays so (no port extension clicking the n-th visible choice).
   c4. Seen in the first play-through (item 4's account): the Voodoo Lady's flashes (room 29)
      slow the game.  Local script 209 rewrites the whole shadow palette one SetPalColor at a
      time (256 iterations, ~770 opcodes a flash), started several times in a row by 207.
      The time reports there: avg 1 100-2 800 half-lines, worst 9 000-15 000 (3-5 rounds),
      runAllScripts 860-1 140 a round (120-150 elsewhere), resetBackground ~17 a round; no
      op-miss.  To measure: the cost of SetPalColor -> setDirtyColors -> updatePalette on
      the 68000, and the opcode dispatch itself.  A second case, the same kind to the eye:
      the potion's flash on the ship (room 14, then room 87), rounds 48100-48200: runAllScripts
      ~910 half-lines a round on average over the block, PROF section 7 ~170, worst 21 798
      (the room change in it); the script that flashes not identified yet (replay the scene).
   c5. Seen in the first play-through (item 4's account): **the catacombs load slowly**
      (the user: 2-3 s a screen).  Room 39 "hellmaze" (640 x 144, 55 objects) is entered
      again at each passage of the maze; each entry costs one round of 160 000-200 000
      half-lines (rounds 65300-65800, four of them), where the bar (640 x 144, 52 objects)
      costs 20 000-35 000 and other room changes the same.  Not the scripts (the entry runs
      a few dozen opcodes on the PC).  The 100-round PROF line points at section 13
      (resetActorBgs -> processActors and on) and section 5, too coarse: measure one entry
      round by itself (a JMI_BOOT=39 build with an input replay through a passage, the
      sections per round).
   c6. **The first play-through stopped**: FATAL "drawStrip: screen strip out of range" in
      part 4, entering the church from High Street (room 34, 800 wide; the door, object
      447 at x 528, does loadRoomWithEgo(819, 78, 145, 142) when open; room 78 is 320 wide).
      ScummVM has the same check (assertRange "screen strip"), so our state differs from
      ScummVM's somewhere on that path.  Ruled out: the data (every one of the 939 object
      images has as many strips as its width), the Jaguar's drawBitmap against the PC's
      (the same lines; roomW follows g.roomWidth), the debug jump 8989 straight into the
      church (hardware = PC, 20 070 lines, no FATAL).  Suspect: something drawn with High
      Street's camera after the change (a stale camera / screenStartStrip, or an object of
      the old room).  Next: the PLAY ROMs now log every input ("I ROUND ..." on COM8,
      tools/play_inputs.py makes the .in) and the FATAL says the room, the object, its
      state, the strip asked and the strips there, the camera; build/jmi_play_1212.j64 (the
      debug jump 1212: part 4's start, cu-dock with the seltzer, debug mode 1) gets there in
      minutes; then replay on the PC and compare with ScummVM on that path.
      **Found and fixed (same evening)**: the user remembered the church door already open
      when arriving; replaying from 1212 with the door opened by hand went through.  High
      Street's local script 200 walks a ghost through its exits; through the church it
      opens the door with global script 25 [447, 819] and closes it with 26 two rounds
      later: 819 (the church's left exit, inside) goes to state 1 with 447 (it lacks class
      11, so classOfIs(819, [11]) is true: raw bytes 0B 00, ScummVM's rule and ours).  The
      player who walks in while it is open enters room 78 with 819 in state 1; 819's OI
      block is 8 bytes (no image: 333 objects of MI1 are so), and drawObject drew its three
      strips from the next block.  ScummVM's code does the same (getObjectImage = OBIM + 8,
      assertRange "screen strip" in drawStrip): not checked on ScummVM itself (its debugger
      opened, the typed "object 819 state 1" did not reach it).  The port now draws nothing
      for an empty OI (gfx.c drawObject), as the original evidently did.  `scumm_pc
      --bootparam 8989 --state-of 819:1` reproduced the FATAL before and runs clean after;
      the reference runs and px28's 531 screens unchanged.
      ScummVM knows this crash: bug #1191, its loadState puts 819 to state 0 after a load
      ("for unknown reasons, object 819 sometimes is in state 1 in old save games ... a
      crash when entering the church"), believing the state came from old saves.  The
      cause is the ghost's race in play: ScummVM would stop there too.  Worth telling its
      developers (the user's call).
   e. The shake (rooms 11 and 53: ShakeOn / ShakeOff are no-ops today) and the banners'
      pause (MUS_HOLD keys the notes off; ScummVM's mixer pause keeps them).
2. **Effects**: transitionEffect's iterations are copied but not shown one by one (the
   screen is shown at the round's hand-over); ScummVM shows each (waitForTimer): a
   hand-over per iteration with its wait.  Then fadeIn 130-133 are v5.
3. **The scroll with z-planes and walking costumes**: what no idle room does; checked by
   the clicked runs once the input is in (item 1).
4. **Time, measured first**: the lookout 964 -> 1 381 with the costumes; room 15 (2 530 of
   3 150 in session 6) not measured since; the 68000's own drawing logic (gfx.c: the object
   queue and drawObject over 63 objects in room 15).  The rule from this session: on the
   68000 every call and every command costs several half-lines; move work to the GPU
   (lists it walks) rather than polishing the C.
5. **Done at the end of session 10 (the repository made ready to publish, as
   jag-monkeyisland)**: LICENSE (MIT, Samuele Voltan), COPYING (GPLv2), COPYING.LGPL (LGPL 2.1),
   an SPDX header in every source file (GPL-2.0-or-later: what is transcribed from ScummVM;
   LGPL-2.1-or-later: the OPL2 on the DSP, which follows Nuked OPL3: src/dsp_audio.s,
   tools/opl2.py, tools/opl2/opl2_model.c/.h, decided with the user; MIT: the rest, the
   user's other projects' code included), the README rewritten (BYOA, what is needed, the
   pad, the checks, the licences, the credits, a trademark notice); this machine's paths out
   of the repository into local.ini (git-ignored; local.ini.example; tools/localcfg.py),
   gen_opcodes keeps the committed table without ScummVM's sources.  No game data was ever
   committed (all 56 commits checked).  Before publishing: the history still has this
   machine's paths in old documents and the author's e-mail in every commit (the user's
   call: a fresh history, or an e-mail to use).  The original text of the item follows.
5 (as written). **Licences, before anything is distributed (the user: MIT for the project's own code, the
   original licence for whatever is transcribed).**  ScummVM is GPLv2-or-later, so what is
   transcribed from it is a derivative and stays GPL, with ScummVM's attribution; a ROM that
   links it is GPL as a whole, so its sources are published with it (BYOA as today: the
   game's data never in the repository).  To check file by file, the map so far:
   - transcribed from ScummVM (GPL): `csrc/scumm/` (the interpreter, opcodes_gen.h generated
     from ScummVM's table), `csrc/ref/` (the reference renderer), the screens' bookkeeping
     in `csrc/jag/jag_gfx.c`, the GPU kernels in `src/gpu_room.s` (drawStripBasicH/V,
     decompressMaskImg, byleRLEDecode, drawBitsN: ScummVM's algorithms in JRISC), and
     `tools/m0_ref.py` / `tools/scumm_v4.py` if they model ScummVM's decoders line by line;
   - the project's own (MIT): the Jaguar platform around them (boot, video, OP list, the
     queue, the HAL, the tools that only compare against ScummVM's output: oracle_*,
     shot_cmp, trace_cmp/crc, pal_crc, input_fit, memory_map, cart_build, comlog...), and
     what comes from the user's other Jaguar projects (Keen Dreams, JagGOT, JagSkyroads,
     JagLBA: their own licences, to confirm);
   - third party, PC tool only (not in the ROM): Nuked OPL3 (LGPL 2.1) behind `--wav`.
   Then: a LICENSE per part (COPYING for the GPL, LICENSE for MIT), a header line in each
   file saying which, and the README naming ScummVM and the other sources.
Build switches: `JMI_BOOT=N` (room), `JMI_TRACE=N` (trace rounds), `JMI_CHECKS=N` (the
trace phase's CRCs, bits 1 pal 2 bb 4 z 8 fb 16 sc; all five cost ~2.4 s a round in the bar
on hardware: give comlog the seconds).  The cycle: build, `python tools/comlog.py run
build/jaguarmi.j64 SECS`, `tools/pal_crc.py LOG --fb|--sc|--bb|--z -- scumm_pc args`
(`--no-chars` until the text is drawn), `tools/trace_crc.py LOG REF` (REF: scumm_pc's
trace of the same boot param).  Rooms whose back buffer changes every round idle: 8, 15, 28,
31, 37, 41, 42, 70, 71, 74, 75, 77.  Rooms with two z-planes: 7, 8, 30, 59, 69, 70; scaled
actors idle: 38, 33, 8, 31, 37, 75, 15 (scumm_pc --ego prints the scales on stderr).

## Session 7: the z-planes (hardware, 2026-11-24)

- **GQ_MASK** (src/gpu_room.s gq_mask): one z-plane strip, ScummVM's decodeMask /
  decompressMaskImg.  plat_drawBitmap queues one per strip and plane right after the
  strip's GQ_STRIP: the RLE's address in the game image (or 0: an empty strip, zeros), the
  destination, the height, and for the OR mode the byte's lane.  The GPU reads the RLE as
  aligned longs from the cartridge (big-endian: each byte comes down with `rorq #24`, no
  swap) and writes with storeb, 40 bytes a row.  A count of 0 is 256 (ScummVM's uint8
  do/while).  ~200 bytes of GPU RAM.
- **MASKBUF** (tools/memory_map.py): ScummVM's buffer exactly: g.numZBuffer planes of
  (height + 4) x 40 bytes, indexed by the ROOM's strip (`x + y * 40`, a wide room wraps
  into the next row), plane 0 unused; one buffer (never on screen; the GPU keeps the
  queue's order: masks, then later the costumes).  plat_initBGBuffers clears what is used,
  after draining the GPU (the old room's GQ_MASKs).
- **The OR mode (DB_ALLOW_MASK_OR) never happens in MI1**: ScummVM's v4 objects go through
  `ScummEngine_v4::resetRoomObject`, which never sets `od->flags`; `dbAllowMaskOr` is set
  only on the v5+ path (object.cpp `resetRoomObject`).  Our world.c leaves it 0 as v4 does.
  The GPU's OR path (read the aligned long, OR the byte in its lane, store) is written for
  the v5 games and **not verified by any run**: check it with the first v5 title.
- The check: `scumm_pc --zcrc` prints "z N crc X" after round N when the buffer (all
  initBGBuffers sized) changed; the Jaguar logs the same at the hand-over (after draining
  the queue, trace phase only); `tools/pal_crc.py LOG --z -- args`.  BigPEmu, then hardware:
  the bar (60 masks in 200 rounds), room 75 (96), room 70 (two planes): all equal, and the
  back-buffer windows still equal (200 each).  Not yet: a scroll (see Next, 5).
- Cost on the 68000: the per-strip address arithmetic first cost the bar ~90 half-lines a
  round (two `__mulsi3` and a reread offset per mask command); hoisted out of the strip
  loop, with the strip's own `codec % 10` (`__modsi3`) and `y * roomP`.  Hardware, half-lines
  a round of 3 150: the bar 1 550 -> 1 470 (PROF 7, the drawing, 23.5 in session 6, 32.8
  with the masks, 22.0 hoisted), room 75 ~1 000, room 70 1 102.  No GPU_FAIL, no stall.
- **Actors: front buffer, not an OP layer** (the data, counted on all 120 costumes): 111
  costumes have a palette entry 0 at an index >= 1 (the outline black), 92 of them draw it
  (161 167 pixels over their distinct cels); scripts set actor palette entries to 0 (rooms
  37, 58, 59, 83: `13 03 0d 01 00` and the like); with the lights off every costume colour
  is 8 and index 12 is 0.  An OP layer with 0 transparent would lose almost every outline;
  redirecting 0 to another black of the room is fragile (cycles, darkenPalette) and breaks
  the byte-for-byte check.  The script: the session's scratchpad `cost_zero.py` (not kept).

## Session 6: the GPU's queue (hardware, 2026-11-24)

- **The GPU serves a command queue** (src/gpu_room.s): started once at boot (src/main.s
  GpuStart), it executes the ring GQ_RING (GQ_N = 512 commands of 8 longs) while GQ_HEAD (the
  68000's count) is ahead of GQ_TAIL (its own), and waits in its own RAM otherwise (one DRAM
  look every ~450 clk).  Commands: GQ_STRIP (one strip: stream, header, place, walk),
  GQ_COPY (one Blitter copy), GQ_TABLES (the colour tables into its RAM), GQ_STOP (before
  Reset(2): a GPU left serving would read the next ROM's DRAM).  Both counters are 16-bit in
  the low word of a long cell: the 68000 reads longs as two words and cannot see one torn.
  No start/stop race: the GPU never stops on an empty queue.
- **The hand-over carries a fence**: plat_drawDirtyScreenParts writes NEXT_* (buffer, width,
  rows, pitch, camera, CLUT) and NEXT_FENCE = GQ_HEAD, and does not wait for the GPU; the
  video interrupt takes it once GQ_TAIL has reached the fence.  The camera travels with its
  buffer (NEXT_CAM/VIEW_CAM; CAM_X is gone): a scroll never shows a strip not drawn yet.  The
  68000 waits only for the previous hand-over to be taken (before the next one, before the
  copies, at a new room), for a full ring, or for the GPU to finish before the colour tables
  are rewritten (roomPalette changed: `g.roomPaletteGen`, bumped at its three writers; the
  256-byte memcmp per drawBitmap it replaces cost ~18 half-lines a call).
- **The strip copy is the Blitter's, driven by the GPU** (JagGOT's gpu_cmd.s pattern,
  B_OPAQUE $01800601 in phrase mode), one copy per strip (8 x height, ~60 us: a long blit
  stops the 68000).  The room buffers' pitch is now the narrowest width the Blitter's WIDTH
  field describes (1008 -> 1024; tools/memory_map.py blit_width, jag_main.c blit_pitch), and
  DWIDTH comes from NEXT_PITCH.  The buffers are sized pitch x max(rows, 200): the dock
  (1008 wide) did not fit the old check and runs now.
- Checked, BigPEmu then hardware, against ScummVM's trace and the PC renderer: the bar (200
  rounds: 200 back-buffer windows), the lookout, the dock, room 65 (150 palettes), rooms 74
  and 15 (150 windows each): all equal; no GPU_FAIL, no GPU_STALL.  Time, half-lines a round
  (hardware, of 3 150): the bar 2 119 -> 1 470 (PROF 7, the drawing: ~970 -> 235; ScummVM
  draws 2.9 drawBitmap and 13 strips a round there), lookout 964, dock 489, room 65 719,
  room 74 866, room 15 2 530 (30 strips a round, ~1 100 half-lines of GPU work beside it).
  The 68000's wait for the GPU: 0 at regime, 11-31 a round on the first 100 (room entry).
- The GPU's RAM: 3 180 of 4 096 bytes (the two colour tables are 2 048 of it).

## Session 5's plan: M2b on the Jaguar, the same picture

The PC renderer is now the Jaguar's oracle, buffer by buffer (CRC32 on COM8), as the room
decoder was in session 3.  In dependency order:

1. **Done: the CLUT from `updatePalette`.**  `plat_setPalette` keeps a staged CLUT (RGB16) and
   `plat_drawDirtyScreenParts`, where ScummVM shows the frame, hands the video interrupt the
   room buffer `plat_room` decoded, the CLUT and the camera in one piece (NEXT_*, NEXT_SEQ):
   a new room never shows a frame in the old colours.  The trace phase logs `pal ROUND crc`;
   `tools/pal_crc.py LOG -- args` checks it against `scumm_pc --palcrc`: room 65 (two colour
   cycles, a new palette every round) 900 hand-overs of 900 equal, and the lava animated on
   the hardware as on ScummVM (the user's eye).
2. **The room and object images and the z-planes on the GPU**, through `plat_drawBitmap`.
   **Done (session 5): the images, double buffered.**  One GPU job per drawBitmap (strips with
   their own place, the colour tables as LONGS: loadw on local RAM reads 32 bits, TRM p.49;
   the first try used words and the screen came out blue), the hidden buffer swapped in at
   the hand-over, the drawn strips copied into the other one before the next drawing.  The
   trace phase logs `bb ROUND crc`; `tools/pal_crc.py LOG --bb -- args` checks it against
   `scumm_pc --bbcrc`: the lookout (1 window) and the bar (114 windows, objects animated every
   round) equal the PC's; on screen the user sees them right.  M0's SHOW_ROOMS path is gone.
   Cost in real time, the bar: 2 166 half-lines a round of 3 150, of which ~1 000 drawing:
   the 68000 waits for each GPU job, rebuilds the 512-long tables each job, and copies the
   strips itself.  Next: queue the jobs, build the tables when roomPalette changes, the copy
   on the GPU or the Blitter; then the z-planes.  The trace phase is slow with the window's
   CRC (46 KB on the 68000 a round: ~0.6 s a round in the bar); `JMI_TRACE=N` shortens it.
   The design:
   The design (decided in session 4, from ScummVM's model and the round of 6 frames):
   - the Jaguar's room buffer IS ScummVM's back buffer: room and objects only, the room's
     whole width, pitch = width, the OP scrolling over it at CAM_X; actors, text and cursor
     are never drawn into it (their own OP objects or layers, steps 3-4);
   - strips are decoded one `drawBitmap` at a time, as ScummVM asks: the 40 visible ones at a
     room's entry, one per 8-pixel scroll, an object over its strips, a room strip again to
     restore under a removed object.  This replaces plat_room's whole-room decode (the dock:
     10 707 half-lines; 40 strips are ~3 400).  One GPU job per call: the walk (from height
     and pitch), then per strip its stream, codec and destination; transparency (codecs
     34-38, 44-48: colour 255 not written) and the roomPalette lookup (a 256-byte table in the
     GPU's RAM: SO_ROOM_COLOR changes it);
   - double buffered: a round lasts 6 frames, and drawing into the buffer on screen would show
     states ScummVM never shows (a strip restored, its object not yet redrawn).  Draw into the
     hidden buffer; plat_drawDirtyScreenParts swaps it in with the CLUT and the camera; then
     the Blitter copies the strips drawn this round into the other buffer;
   - the z-planes per strip as decodeMask (RLE, one byte = 8 pixels), next to the buffer;
   - the check: `scumm_pc --bbcrc` prints the CRC of its back buffer's visible window after
     each round, the Jaguar logs the same at each hand-over, a tool compares them (as
     pal_crc.py).
   The earlier notes:
   ScummVM draws strips into a back buffer and the z-planes beside it.  The Jaguar keeps the
   whole room decoded (the OP scrolls over it), so a room strip is already there, and an
   object strip is decoded over it by a GPU job (codecs 14-18, 24-28, 34-38, 44-48, 1);
   restoring under a removed object re-decodes the room strip.  The z-planes decode at room
   entry (1 bit a pixel, 40 bytes a row per plane in ScummVM: the Jaguar may keep the room's
   whole width).  CRC: the visible window of the room buffer against the PC's back buffer,
   and the planes.
3. **The costumes on the GPU** (`plat_drawCel` gives everything): one sprite buffer per actor
   or the frame, decided on measured OP load (feasibility §3); the z-plane mask.  CRC per cel.
4. **Text, verbs, cursor**: the text surface over the room (`plat_drawChar` toText), the verb
   screen, the cursor (`plat_setCursor`, an OP object).  `restoreCharsetBg` clears the text.
5. **Effects**: `transitionEffect` (the iris and the box wipes: dirty strips shown in an
   order), the fade-out clear.  Dissolves (128, 134, 135) stop the game today: their order
   comes from the game's random source, so a dissolve must draw its permutation in the logic
   with ScummVM's calls before any platform shows it (the 1x1 one is 40 960 numbers).  No room
   swept so far asks for one.
6. Then the plan's steps 6-8: the pad, the audio, save/load and the rest.

### Small items left open

- `darkenPalette` and `setShadowPalette` (v4) read the colours from `getPalettePtr()`, which
  points at the CLUT's LE16 count, while `setPaletteFromPtr` skips it: transcribed as ScummVM
  has it; whether the original reads them the same way shows when a room uses
  `SO_ROOM_INTENSITY` with a scale other than 0.
- Not yet seen by the pixel oracle: a transition effect captured mid-way, `palManipulate`,
  `darkenPalette`, the text in the top virtual screen, the inventory arrows.

- `TimeNow` in `src/main.s` falls a frame behind between VC = VI and the ISR (fixed in
  `time_now` in C); M0's room code (`SHOW_ROOMS = 1`, rooms.bin) can go once nothing needs
  it.
- The costume scale loops cost ~130 half-lines a round at the lookout (a per-scale count
  table would make them O(1)); not needed at today's 21-28 % of the 68000.
- Not yet seen by any run: Give to an actor, the inventory scrolling, flobjects
  (`SO_LOAD_OBJECT`), image verbs, the flashlight, a long cutscene with ESC.
- The hardware cycle: `cmd /c build.bat` from PowerShell, then
  `python tools/comlog.py run build/jaguarmi.j64 SECS`; the ROM ends with `Reset(2)`, so
  the next upload needs no power cycle (wait for the user if a cycle was needed anyway).
  `JMI_BOOT=N` picks the room. Checks: `tools/trace_crc.py LOG REF`, `tools/room_crc.py
  --log LOG`.

## Then on the Jaguar

- Done in M2a: the C as a blob for the 68000, the game image read from the cartridge.
- **Audio**: `player_ad.c` at 472 Hz from a JERRY timer, its `plat_opl_write` feeding
  JagSkyroads' OPL2 on the DSP. Whether the driver itself runs on the 68000 (writes handed over
  in cells, `hw/07` rule 8) or beside the OPL2 on the DSP is the open choice; `--opl` gives the
  write rate.
- The camera follows the actor (`moveCamera`: one strip per logic step). M0's D-pad scroll was
  a test of the OP only.

## Notes left open

- The keypad is game input in this port (12 verbs on 12 keys, from M2): the instruments need
  another home before then (`#` sweep today).
- PAL: the 60 Hz jiffy needs decoupling from the frame. The reference console is NTSC.
- The oracle's mouse pin makes the trace depend on nobody touching the mouse; scripted input
  will need the same care.
