#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""The ONE place every address of the project is decided.

    python tools/memory_map.py

Generates, and nobody edits by hand:
  include/map.inc     the EQUs RMAC assembles against
  build/layout.txt    a readable dump

DRAM regions are laid out in order, each aligned to its own need; the HAL cells are aligned to
THEIR size (a long on a multiple of 4), because the GPU and the DSP ignore the two low address
bits on a long access and would silently read four bytes earlier (Keen Dreams lost its audio
to exactly that).  Cartridge regions follow the REAL sizes of the converted blobs in cart/, so
this runs after tools/mi2jag.py.

Structure and conventions are JagGOT's and JagSkyroads' tools/memory_map.py, verified on
hardware.
"""
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CART_DIR = os.path.join(REPO, 'cart')

DRAM_SIZE = 0x200000
ROM_BASE = 0x800000
ROM_CODE_OFS = 0x2000            # the $F6 fastboot header jumps here
ROM_CODE_SIZE = 0x10000
ROM_CBLOB_SIZE = 0x40000         # the interpreter, compiled C (csrc/, M2): build/engine.bin
CBSS_SIZE = 0x20000              # its .bss
# The GPU's command queue (src/gpu_room.s, csrc/jag/jag_main.c gq_put): a ring of GQ_N
# commands of GQ_CMD bytes in DRAM.  GQ_HEAD counts the commands the 68000 has written, GQ_TAIL
# the ones the GPU has finished, both modulo 65536 in long cells whose high word stays 0 (the
# 68000 reads a long as two words: a counter that only moves in its low word cannot tear).
GQ_N = 512
GQ_CMD = 32
GQ_TYPES = {'GQ_STRIP': 1, 'GQ_BLIT': 2, 'GQ_TABLES': 3, 'GQ_STOP': 4, 'GQ_MASK': 5,
            'GQ_RECTS': 6, 'GQ_CEL': 7, 'GQ_CHAR': 8}
# A costume cel's block (GQ_CEL; csrc/jag/jag_gfx.c plat_drawCel fills it, src/gpu_room.s gq_cel
# reads it): the long offsets.  CEL_PAL is 8 longs (32 bytes of palette).
CEL_FIELDS = ['SRC', 'SKIPN', 'DEST', 'MROW', 'X', 'Y', 'H', 'SKIPW', 'SCALEX', 'SCALEY',
              'SXI', 'SYI', 'STEP', 'CMASK', 'CSHR', 'BR', 'BB', 'PAL'] + [None] * 7 + ['TABLE']
CEL_LONGS = len(CEL_FIELDS)
CEL_RING = 64                    # blocks in CELBUF, reused in turn
MASK_PLANES = 4                  # the z-plane buffer: numZBuffer at most 4
SCR_RECTS = 256                  # a screen list's entries (jag_gfx.c: then the whole screen)
C_MAGIC = 0x4D49324A             # 'MI2J': c_init's answer, checked by the boot

# Jerry's I2S rate: Fs = clk / (64 (SCLK + 1)) (JaguarDocs hw/07 rule 1).  The OPL2
# synthesiser and its tables (tools/opl2.py) follow this ONE number.  JagSkyroads and JagGOT
# run 36 (11,229 Hz); MI1's bar starved the I2S ~10 times a second there on hardware (bursts
# of nine channels and the drums longer than the 64-sample ring; the DSP ~1.17 x dsp_check's
# estimate, from SND_SPIN), so 40 = 10,134 Hz: no starve simulated at 1.17 x, 26 samples left
# at the worst (session 9, tools/opl2/dsp_check.py with DSP_SLOW=1.17).
AUDIO_SCLK = 40
# The AdLib driver's output (csrc/jag/jag_audio.c): a ring of 16-bit stream words in DRAM
# that the DSP reads as JagGOT's streams (src/dsp_audio.s), and how many of the driver's
# 472 Hz ticks the video interrupt keeps written ahead of the ones the DSP has played.
AD_RING_WORDS = 4096
AD_LEAD = 24                     # ~51 ms, three frames

# Switches more than one party reads (RMAC, linker script, C).
SWITCHES = {
    # The boot parameter the interpreter starts with (MI1's debug jump to room N, as the
    # oracle's -BootParam N).  Environment JMI_BOOT=N for a one-off build.
    'BOOT_PARAM': int(os.environ.get('JMI_BOOT', '38')),
    # The rounds of the trace phase (csrc/jag/jag_main.c): JMI_TRACE=N for a one-off build,
    # e.g. a short trace to reach the real-time phase sooner.
    'TRACE_ROUNDS': int(os.environ.get('JMI_TRACE', '900')),
    # The trace phase's picture checks (csrc/jag/jag_gfx.c), a bit each: 1 the palette, 2 the
    # back buffer's window, 4 the z-planes, 8 the front buffer's window, 16 the system screen,
    # 32 the cursor.  Each costs the 68000 a CRC over its buffer every round.  JMI_CHECKS=N for
    # a one-off build.
    'CHECKS': int(os.environ.get('JMI_CHECKS', '63')),
    # PLAY=1: the game played with the pad, endlessly (no trace phase, no time reports):
    # the video interrupt reads the pad and moves the mouse (MOUSE_X/Y).  0: the checks'
    # build, the mouse moved by the input table (JMI_INPUT).  JMI_PLAY=1 for a one-off build.
    'PLAY': int(os.environ.get('JMI_PLAY', '0')),
    # VAR_SOUNDCARD: 3 the AdLib (the music, through the original driver and the DSP), 0 none
    # (ScummVM's music_driver=null: the oracle's traces).  By default the AdLib when PLAY,
    # none for the checks' builds; JMI_SOUND=N for a one-off build.
    'SOUNDCARD': int(os.environ.get('JMI_SOUND', '3' if os.environ.get('JMI_PLAY', '0') != '0'
                                    else '0')),
    # The Dial-A-Pirate wheel (script 152): 1 it runs (the game as sold; the user: it stays),
    # 0 skipped as ScummVM's copy_protection=false (the oracle's runs).  By default on when
    # PLAY; JMI_COPYPROT=N for a one-off build (scumm_pc --copyprot N alike).
    # A test of the GameDrive's SD card files (csrc/jag/jag_main.c sd_test): 1 writes
    # /JMITEST.BIN (64 KB known) and checks its size; 2 also reads it back (FRead: the next
    # upload needs a power cycle).  JMI_SDTEST=N for a one-off build; 0 the game.
    'SD_TEST': int(os.environ.get('JMI_SDTEST', '0')),
    # The trace phase saves the game to the SD card at round SAVE_AT and loads it back at
    # round LOAD_AT (scumm_pc --save-at / --load-at alike); 65535: never.  JMI_SAVEAT,
    # JMI_LOADAT for a one-off build.
    'SAVE_AT': int(os.environ.get('JMI_SAVEAT', '65535')),
    'LOAD_AT': int(os.environ.get('JMI_LOADAT', '65535')),
    'COPY_PROT': int(os.environ.get('JMI_COPYPROT', '1' if os.environ.get('JMI_PLAY', '0') != '0'
                                    else '0')),
    # VAR_DEBUGMODE: 1 as a ScummVM run with a debug level (the oracle's traces; MI1's boot
    # script needs it for a debug jump: without it, boot param N quits with "Debugging mode not
    # active"), 0 the game as sold (it also starts script 156, which draws a random number every
    # round).  By default 0 only when PLAY from boot param 0; JMI_DEBUG=N for a one-off build.
    'DEBUG_MODE': int(os.environ.get('JMI_DEBUG', '0' if os.environ.get('JMI_PLAY', '0') != '0'
                                     and os.environ.get('JMI_BOOT', '38') == '0' else '1')),
}

# --- the screen ------------------------------------------------------------
# SCUMM's 320x200 frame, letterboxed in 240 lines: rows 20..219.  The room is shown from the
# frame's top (the v4 main screen starts at row 0), the verb area below it stays empty in M0.
SCR_W = 320
SCR_H = 240
FRAME_Y = 20
START_ROOM = 38                  # 'lookout', the first room the game shows after the logo

# --- HAL cells: (name, size). Aligned to their size. --------------------------
# One writer per cell; who writes is said next to it.
HAL_CELLS = [
    ('IRQ_TICK', 4),             # ISR: video interrupts since boot
    ('BOOT_MARK', 4),            # boot: last stage reached
    ('VID_NTSC', 2),             # boot
    ('VDB_VAL', 2),              # boot
    ('VDE_VAL', 2),              # boot
    ('VI_VAL', 2),               # boot / LockVi
    ('VI_FLIP', 2),              # LockVi: 1 = VI moved to the other parity
    ('PAD_STATE', 2),            # ReadPad: game buttons, PAD_B_* bits
    ('PAD_KEYS', 2),             # ReadPad: keypad, KEY_B_* bits
    ('PAD_PREV', 2),             # main loop: last frame's PAD_STATE
    ('KEYS_PREV', 2),            # main loop: last frame's PAD_KEYS
    ('CUR_ROOM', 2),             # main loop: the room shown (or being built)
    ('LOOP_TICK', 4),            # main loop: the IRQ_TICK it last ran for
    ('BUILD_HL', 4),             # main loop: last room build, half-lines
    # The GPU: started once at boot (src/main.s GpuStart), it serves the command queue until a
    # GQ_STOP.  GPU_DONE: 0 at the start, the GPU sets it as its last act.  GPU_STALL: the
    # GPU, Blitter waits that ran out; GPU_FAIL: the 68000, waits on the GPU that ran out.
    ('GPU_DONE', 4),
    ('GPU_STALL', 4),
    ('GPU_FAIL', 4),
    ('GQ_HEAD', 4),              # main loop (C): commands written, mod 65536
    ('GQ_TAIL', 4),              # GPU: commands done, mod 65536
    # The picture to show next: the main loop writes NEXT_BUF (a system screen, SCREEN0 or
    # SCREEN1), NEXT_CLUT and NEXT_FENCE, then NEXT_SEQ + 1.  The video interrupt takes it
    # once the GPU has done every command written before it (GQ_TAIL reached NEXT_FENCE):
    # loads NEXT_CLUT into the CLUT, copies NEXT_BUF into VIEW_BUF and NEXT_SEQ into
    # VIEW_SEQ, all in the same vertical blank (JaguarDocs hw/04 rule 11: palette first).
    ('NEXT_BUF', 4),
    ('NEXT_CLUT', 4),
    ('NEXT_FENCE', 4),           # GQ_HEAD when handed over (low word)
    ('NEXT_SEQ', 4),
    ('VIEW_BUF', 4),             # ISR
    ('VIEW_SEQ', 4),             # ISR
    # The cursor, handed over with the screen: its image (a CURSORn buffer, 16 x 16, 8 bpp,
    # 0 transparent; 0 = not shown) and its hotspot.  The ISR copies NEXT_CUR* into
    # VIEW_CUR* with the rest, and draws it every frame at the mouse minus the hotspot, as
    # ScummVM's backend draws it where the mouse is at once.
    ('NEXT_CUR', 4),
    ('NEXT_CURHX', 4),
    ('NEXT_CURHY', 4),
    ('VIEW_CUR', 4),             # ISR
    ('VIEW_CURHX', 4),           # ISR
    ('VIEW_CURHY', 4),           # ISR
    # The mouse, game screen coordinates: PLAY, the ISR from the pad (MovePad); otherwise
    # the main loop, from the input table.  One writer per build.
    ('MOUSE_X', 4),
    ('MOUSE_Y', 4),
    # PLAY: the presses of A and B the ISR has seen (counts, mod 2^32): a press and release
    # between two loop rounds is not lost.  ISR only.
    ('PAD_ADOWN', 4),
    ('PAD_BDOWN', 4),
    ('PAD_CDOWN', 4),            # C: the '.' key (the line being said ends)
    ('PAD_ODOWN', 4),            # OPTION: ESC (the cutscene's override)
    ('PAD_HELD', 2),             # ISR: frames the D-pad has been held (the mouse's speed)
    # PLAY: the keypad presses the ISR has seen, in order: KEYQ[n & 15] holds the key (KEY_B_*)
    # of press n, KEYQ_HEAD counts the presses (mod 2^32).  ISR only: the main loop keeps its
    # own tail and takes one a round (ScummVM's _keyPressed is one key), so none is lost.
    ('KEYQ_HEAD', 4),
    ('KEYQ', 16),
    # The video interrupt's two spies (ISR only; the main loop reports them): OP_MISS the
    # rows, summed, on which the OP did not reach the list's last object (EDGE, as tall as the
    # screen: its HEIGHT left at the interrupt, Keen Dreams' spy); OP_MISSMAX the most in one
    # frame; ISR_LATE the most half-lines the interrupt started after VI.
    ('OP_MISS', 4),
    ('OP_MISSMAX', 2),
    ('ISR_LATE', 2),
    ('ISR_END', 2),              # ISR: the most half-lines after VI the list was re-armed
    # ISR: 1 while the video interrupt re-arms the list: the GPU starts no blit meanwhile (a
    # blit stops the 68000 for its whole length; blits back to back made the re-arm late and
    # the OP lost rows, session 8).  The GPU only reads it.
    ('ISR_BUSY', 4),
    # The GameDrive's debug channel (src/gd.s): RMAC only.
    ('GD_BIOS', 4),
    ('GD_SEND_FN', 4),
    ('GD_CALL_FN', 4),           # boot (src/gd.s GdInstall): GdCallC, or 0 without a GameDrive
    ('GD_DROP', 4),
    ('GD_TIMEOUT', 4),
    ('GD_VER', 4),
    ('GD_STATE', 2),
    ('GD_PEND', 2),
    # Without a GameDrive (BigPEmu) the C's log lines go to EMU_LOG instead of COM8: EMU_LOG_N
    # bytes written so far, EMU_END = EMU_END_MAGIC once the run is over (back_to_stub).
    # tools/emu_log.py's BigPEmu script waits for it and saves the lines.  C only.
    ('EMU_LOG_N', 4),
    ('EMU_END', 4),
    # Jerry (src/dsp_audio.s, JagSkyroads' and JagGOT's protocol, without JagGOT's voice).
    # The 68000 writes the stream to play with MUS_SEQ LAST; MUS_HOLD non-zero
    # holds the music ($B0..$B8 = 0 once, the stream frozen) until it goes back to 0.  The
    # DSP writes the counters, every 64 samples.  JaguarMI's stream is a ring (AD_RING, its
    # loop point its start) the AdLib driver writes as it runs: the DSP stops at MUS_WR and
    # tells where it is (MUS_RD) and how many ticks it has played (MUS_PLAYED).
    ('MUS_ADR', 4),              # 68000: the stream's start
    ('MUS_LOOP', 4),             # 68000: its loop point
    ('MUS_SEQ', 4),              # 68000: +1 = play the two above
    ('MUS_HOLD', 4),             # 68000: 1 = paused, 0 = playing
    ('MUS_WR', 4),               # 68000: the stream written up to here (whole ticks)
    ('MUS_RD', 4),               # DSP: the stream read up to here
    ('MUS_PLAYED', 4),           # DSP: ticks played (the stream's waits run down)
    ('MUS_STALL', 4),            # DSP: ticks that found nothing written (the 68000 late)
    ('SND_STARVE', 4),           # DSP: interrupts that found the ring empty
    ('SND_IRQ', 4),              # DSP: I2S interrupts served
    ('SND_DONE', 4),             # DSP: samples produced
    ('SND_SPIN', 4),             # DSP: idle-loop turns with the ring full
    # The AdLib driver's lock (ScummVM's Player_AD _mutex): the main loop holds it inside
    # ad_startSound / ad_stopSound / ad_stopAllSounds / ad_init; the video interrupt does not
    # run the driver while it is held (it catches up the frame after).  Main loop only.
    ('AD_LOCK', 4),
]
EMU_END_MAGIC = 0x454E4421       # 'END!'
EMU_LOG_SIZE = 0x10000

# --- OP objects, in LIST ORDER = priority (whoever is drawn last wins) -------
OBJECTS = [
    'ROOM',                      # the system screen (SCREEN0/1)
    'CURSOR',                    # the cursor over it (CURSOR0..2)
    'EDGE',                      # black right of the screen (x 320..351, over the cursor:
                                 # the NTSC display shows 352 pixels, ScummVM clips at 320)
]


def blit_width(w):
    """The narrowest width >= w the Blitter's WIDTH field can describe: (4 + m) << (e - 2),
    m 0..3, e 0..11 (TRM p.66, JaguarDocs hw/05).  The room buffers' pitch: the Blitter copies
    between them with pixel pointers, which need the row length as WIDTH.  The same rule is
    csrc/jag/jag_main.c blit_pitch(), which checks the buffer's size at every room."""
    for e in range(2, 12):
        for m in range(4):
            v = (4 + m) << (e - 2)
            if v >= w:
                return v
    raise SystemExit(f'no Blitter width for {w}')


def blit_field(w):
    """The WIDTH field (e << 2 | m) of a width blit_width gives exactly."""
    for e in range(2, 12):
        for m in range(4):
            if (4 + m) << (e - 2) == w:
                return (e << 2) | m
    raise SystemExit(f'{w} is not a Blitter width')


def room_buffer_size():
    path = os.path.join(CART_DIR, 'rooms_max.txt')
    if not os.path.isfile(path):
        raise SystemExit('cart/rooms_max.txt missing: run tools/mi2jag.py first')
    return align(int(open(path).read()), 64)


def dram_regions():
    n_obj = len(OBJECTS)
    rb = room_buffer_size()
    return [
        ('VECTORS', 0x400, 8),
        ('HAL', 0x400, 8),
        # The list the OP reads: two branches, the objects, the final STOP.
        ('OP_LIST', 16 + 32 * n_obj + 16, 32),
        # The truth the video interrupt re-arms the list from (the OP consumes HEIGHT and DATA
        # as it draws).
        ('OBJ_SHADOW', 32 * n_obj, 32),
        ('STACK', 0x8000, 8),
        ('DCODE', ROM_CODE_SIZE, 8),
        # The interpreter runs from DRAM: ColdBoot copies the whole image (code, rodata,
        # data) here; .bss lives apart in CBSS, which the DRAM clear has already zeroed
        # (JagGOT's layout).  The game image itself stays in the cartridge (ROM_GAME).
        ('DBLOB', ROM_CBLOB_SIZE, 8),
        ('CBSS', CBSS_SIZE, 8),
        # The GPU's command queue (GQ_N above), and the colour table a GQ_TABLES command
        # copies into the GPU's RAM: 256 longs (roomPalette, 255 marked: the transparent
        # strips do not write it).  And the CLUT handed over, RGB16.
        ('GQ_RING', GQ_N * GQ_CMD, 32),
        ('GQ_COLTAB', 1024, 8),
        ('ROOM_CLUT', 512, 8),
        # ScummVM's z-plane buffer (rtBuffer 9): g.numZBuffer planes of (the room's height + 4)
        # rows of 40 bytes (NUM_STRIPS: one byte per visible strip, indexed by the ROOM's strip,
        # so a wide room wraps into the next row: initBGBuffers, getMaskBuffer).  The GPU writes
        # it (GQ_MASK), the costumes will read it; never on screen, so one buffer.  At most 4
        # planes counted (csrc/scumm/world.c initBGBuffers) of rows <= 200 (jag_gfx.c checks).
        ('MASKBUF', MASK_PLANES * (200 + 4) * 40, 8),
        # ScummVM's screens (gfx.cpp), none of them shown itself (csrc/jag/jag_gfx.c):
        # - BACKBUF, the main virtual screen's back buffer: the room and its objects, pitch =
        #   blit_width(the room's width), rows = the room's or 200, 8 bpp, sized for the
        #   largest room of the game (cart/rooms_max.txt, tools/mi2jag.py).  The GPU decodes
        #   the strips into it (GQ_STRIP), the Blitter copies from it.  A whole number of
        #   1024-byte rows, so that one fill clears it (initVirtScreen).
        # - FRONTBUF, the main virtual screen's pixels in ScummVM's layout exactly: pitch 320,
        #   room pixel (X, Y) at Y * 320 + X (a wide room wraps into the next row), 4 rows of
        #   slack for the camera, so the visible window is the 320-pitch bitmap at + xstart.
        #   The strips and the actors' backgrounds are copied into it from BACKBUF, the actors
        #   drawn over it.
        # - TEXTVS, VERBVS, BANNERVS: the text, verb and banner virtual screens, pitch 320.
        # Phrase aligned: the Blitter moves phrases, the camera moves in strips of 8.
        ('BACKBUF', align(rb, 1024), 64),
        ('FRONTBUF', SCR_W * (200 + 4), 64),
        ('TEXTVS', SCR_W * 200, 64),
        ('VERBVS', SCR_W * 200, 64),
        ('BANNERVS', SCR_W * 12, 64),
        # The surfaces a message banner keeps (gfx_gui.cpp saveSurfacesPreGUI): the text
        # surface, the main screen's visible rows (320 x its height) and the verb screen,
        # copied back when the banner goes (csrc/jag/jag_gfx.c).
        ('GUI_TEXT', SCR_W * 200, 64),
        ('GUI_MAIN', SCR_W * 200, 64),
        ('GUI_VERB', SCR_W * 200, 64),
        # A save of the game (csrc/scumm/saveload.c: 86 KB raw), and the SD card's buffer.
        ('SAVE_BUF', 128 * 1024, 64),
        ('SAVE_RAW', 128 * 1024, 64),       # the state itself, before packing / after
        # The text surface (ScummVM's _textSurface): the glyphs printed over the room, screen
        # coordinates, pitch 320, CHARSET_MASK_TRANSPARENCY (0xFD) where there is none;
        # composited over every rectangle copied into the system screen.
        ('TEXTSURF', SCR_W * 200, 64),
        # The charsets (90x.LFL), copied from the cartridge at boot: the GPU reads the glyphs
        # a byte at a time (loadb is right from DRAM, JaguarDocs hw/06 T-GPU-11).
        ('CHARSETS', 0x4000, 8),
        # dissolveEffect's order (csrc/scumm/loop.c): one 16-bit offset per block, a 1 x 1
        # dissolve of the main screen at most.
        ('DISSOLVE', 2 * 320 * 200, 8),
        # The system screen (OSystem's, what drawDirtyScreenParts copies the virtual screens'
        # dirty parts into), 320 x 200, twice: the OP shows one while the Blitter copies into
        # the other; the hand-over swaps them.
        # The cursor's images as the OP shows them (16 x 16, 8 bpp, 0 transparent), three:
        # one on screen, one handed over, one to write (csrc/jag/jag_gfx.c plat_setCursor).
        ('CURSOR0', 256, 64),
        ('CURSOR1', 256, 64),
        ('CURSOR2', 256, 64),
        # EDGE's pixels: 32 x 200 at 16 bpp, black ($0000, no TRANS): never written (the
        # boot clears DRAM).
        ('EDGE_BUF', 64 * 200, 64),
        ('SCREEN0', SCR_W * 200, 64),
        ('SCREEN1', SCR_W * 200, 64),
        # The rectangles copied into a system screen since the hand-over before (GQ_RECTS:
        # 3 longs each), two lists: the one being written, and the one the last hand-over
        # showed, replayed screen to screen into the other screen (csrc/jag/jag_gfx.c).
        ('SCR_LIST0', SCR_RECTS * 12, 8),
        ('SCR_LIST1', SCR_RECTS * 12, 8),
        # The costume cels' blocks (GQ_CEL), a ring of CEL_RING: a block is reused once the
        # GPU has done the command that read it.
        ('CELBUF', CEL_RING * CEL_LONGS * 4, 8),
        ('LOG_BUF', 128, 8),
        # The log without a GameDrive (see EMU_LOG_N): text lines, '\n' after each.
        ('EMU_LOG', EMU_LOG_SIZE, 8),
        # The GameDrive's BIOS image, fetched at boot (src/gd.s GdInstall).
        ('GDB_BUF', 0x1000, 8),
        # The OPL2's slow tables (tools/opl2.py opl2_rom.inc: DINC, AINC, MULT, KSL), copied
        # here by InitDspAudio.  At an ABSOLUTE address because the DSP cannot name a 68000
        # label: RMAC relocates a movei of one wrongly (JagSkyroads, seen in main.bin).
        ('OPL_TABS', 640, 8),
        # The AdLib driver's stream (AD_RING_WORDS above), read by the DSP.
        ('AD_RING', AD_RING_WORDS * 2, 8),
    ]


# --- cartridge regions: (name, file) in order; sizes from cart/ ---------------
def cart_blobs():
    return [('ROM_GAME', 'game.bin')]       # tools/mi2jag.py: the game's files, decrypted


def align(v, a):
    return (v + a - 1) // a * a


def dram_layout():
    out, addr = [], 0
    for name, size, al in dram_regions():
        addr = align(addr, al)
        out.append((name, addr, size))
        addr += size
    if addr > DRAM_SIZE:
        raise SystemExit(f"DRAM overflow: ${addr:X} > ${DRAM_SIZE:X}")
    return out, addr


def hal_layout(base, size):
    out, addr = [], base
    for name, sz in sorted(HAL_CELLS, key=lambda c: -c[1]):
        addr = align(addr, sz)
        out.append((name, addr, sz))
        addr += sz
    if addr > base + size:
        raise SystemExit("HAL cells overflow their region")
    for name, a, sz in out:
        assert a % sz == 0, name
    return out


def cart_layout():
    """[(sym, cpu_addr, file_offset, size, file)], ROM_CODE first."""
    out = [('ROM_CODE', ROM_BASE + ROM_CODE_OFS, ROM_CODE_OFS, ROM_CODE_SIZE, None)]
    ofs = ROM_CODE_OFS + ROM_CODE_SIZE
    out.append(('ROM_CBLOB', ROM_BASE + ofs, ofs, ROM_CBLOB_SIZE, None))
    ofs += ROM_CBLOB_SIZE
    for sym, fn in cart_blobs():
        path = os.path.join(CART_DIR, fn)
        if not os.path.isfile(path):
            raise SystemExit(f"cart/{fn} missing: run tools/mi2jag.py first")
        size = align(os.path.getsize(path), 32)
        ofs = align(ofs, 32)
        out.append((sym, ROM_BASE + ofs, ofs, size, fn))
        ofs += size
    return out, ofs


def cart_size():
    _, end = cart_layout()
    for mb in (2, 4, 6):
        if end <= mb << 20:
            return mb << 20
    raise SystemExit(f"cartridge content {end:,} bytes exceeds 6 MB")


def main():
    dram, dram_end = dram_layout()
    regs = dict((n, (a, s)) for n, a, s in dram)
    hal = hal_layout(*regs['HAL'])
    cart, cart_end = cart_layout()

    eq = []           # (name, value, comment)
    for n, a, s in dram:
        eq.append((n, a, f'DRAM, {s} bytes'))
        eq.append((n + '_SIZE', s, None))
    for n, a, s in hal:
        eq.append((n, a, f'HAL cell, {s} bytes'))
    for sym, addr, ofs, size, fn in cart:
        eq.append((sym, addr, f'cartridge, {size} bytes' + (f' <- cart/{fn}' if fn else '')))
        eq.append((sym + '_SIZE', size, None))
    op = regs['OP_LIST'][0]
    # A branch whose condition is false falls through to the NEXT PHRASE, not to its link:
    # the two branches and the first object must be adjacent (JagSkyroads M0, hardware).
    eq += [
        ('OP_BR_VDE', op, 'branch: below the window -> STOP'),
        ('OP_BR_VDB', op + 8, 'branch: above the window -> STOP (falls through from BR_VDE)'),
        ('OP_OBJS', op + 16, 'first object, falls through from BR_VDB (32-byte cells)'),
        ('OP_STOP', op + 16 + 32 * len(OBJECTS), 'the final STOP'),
        ('OBJ_STRIDE', 32, None),
        ('OBJ_N', len(OBJECTS), None),
        ('STACK_TOP', regs['STACK'][0] + regs['STACK'][1], None),
    ]
    eq += [(k, v, 'switch') for k, v in SWITCHES.items()]
    # The C blob's entry table (csrc/jag/crt0.s): six bytes per entry, new ones at the END.
    eq += [('EMU_END_MAGIC', EMU_END_MAGIC, 'EMU_END: the run is over (tools/emu_log.py)')]
    eq += [('C_MAGIC', C_MAGIC, "c_init's answer"),
           ('C_ENTRY_INIT', regs['DBLOB'][0], 'C entry table: jmp c_init'),
           ('C_ENTRY_MAIN', regs['DBLOB'][0] + 6, 'C entry table: jmp c_main (never returns)'),
           ('C_ENTRY_AUDIO', regs['DBLOB'][0] + 12, 'C entry table: jmp c_audio (the VI, a frame)')]
    eq += [('AD_RING_WORDS', AD_RING_WORDS, 'the AdLib stream ring, words'),
           ('AD_LEAD', AD_LEAD, "the driver's ticks written ahead of the DSP")]
    for i, o in enumerate(OBJECTS):
        eq.append(('OBJ_' + o, i, 'index in the list'))
    # Pad bits (Keen Dreams' ReadPad, verified on hardware): PAD_STATE / PAD_KEYS.
    for i, n in enumerate(['UP', 'DOWN', 'LEFT', 'RIGHT', 'A', 'B', 'C', 'OPT', 'PAUSE']):
        eq.append(('PAD_B_' + n, i, 'bit of PAD_STATE'))
    for i in range(10):
        eq.append((f'KEY_B_{i}', i, 'bit of PAD_KEYS'))
    eq += [('KEY_B_STAR', 10, 'bit of PAD_KEYS'), ('KEY_B_HASH', 11, 'bit of PAD_KEYS')]
    # The room record (tools/mi2jag.py): field offsets the 68000 and the GPU read.
    eq += [('GQ_N', GQ_N, 'GPU queue: commands in the ring (a power of 2)'),
           ('GQ_CMD', GQ_CMD, 'GPU queue: bytes per command (8 longs)')]
    eq += [(k, v, 'GPU queue: command type (first long)') for k, v in GQ_TYPES.items()]
    # The Blitter's A1/A2_FLAGS for a 320-pitch buffer in phrase mode: PIXEL8 | WIDTH 320.
    eq += [('BLIT_FLAGS320', 0x18 | (blit_field(SCR_W) << 9), 'Blitter flags: 8 bpp, pitch 320'),
           ('SCR_RECTS', SCR_RECTS, "a screen list's entries"),
           ('CEL_LONGS', CEL_LONGS, "a cel's block, longs"), ('CEL_RING', CEL_RING, 'blocks in CELBUF')]
    eq += [('CEL_' + n, i, "a cel's block: long offset") for i, n in enumerate(CEL_FIELDS) if n]
    eq += [('SCR_W', SCR_W, None), ('SCR_H', SCR_H, None), ('FRAME_Y', FRAME_Y, None),
           ('START_ROOM', START_ROOM, None)]

    os.makedirs(os.path.join(REPO, 'include'), exist_ok=True)
    os.makedirs(os.path.join(REPO, 'build'), exist_ok=True)

    hdr = 'GENERATED BY tools/memory_map.py - DO NOT EDIT BY HAND.'
    with open(os.path.join(REPO, 'include', 'map.inc'), 'w', newline='\n') as f:
        f.write(f'; {hdr}\n\n')
        for n, v, c in eq:
            if c:
                f.write(f'; {c}\n')
            f.write(f'{n:<16} EQU     ${v:X}\n')

    with open(os.path.join(REPO, 'csrc', 'mem_map.h'), 'w', newline='\n') as f:
        f.write(f'/* {hdr} */\n#ifndef MEM_MAP_H\n#define MEM_MAP_H\n\n')
        for n, v, c in eq:
            f.write(f'#define {n:<16} 0x{v:X}UL\n')
        f.write('\n#endif\n')

    with open(os.path.join(REPO, 'include', 'jag_c.ld'), 'w', newline='\n') as f:
        f.write(f'''/* {hdr} */
OUTPUT_ARCH(m68k)
ENTRY(entry_table)

MEMORY
{{
    BLOB (rwx) : ORIGIN = 0x{regs['DBLOB'][0]:X}, LENGTH = 0x{regs['DBLOB'][1]:X}
    BSS  (rw)  : ORIGIN = 0x{regs['CBSS'][0]:X}, LENGTH = 0x{regs['CBSS'][1]:X}
}}

SECTIONS
{{
    .text : {{ *(.entry) *(.text*) *(.rodata*) . = ALIGN(4); }} > BLOB
    .data : {{ *(.data*) . = ALIGN(4); }} > BLOB
    .bss (NOLOAD) : {{ *(.bss*) *(COMMON) . = ALIGN(4); }} > BSS
    /DISCARD/ : {{ *(.comment) *(.note*) *(.eh_frame*) }}
}}
''')

    with open(os.path.join(REPO, 'build', 'layout.txt'), 'w') as f:
        f.write('DRAM\n')
        for n, a, s in dram:
            f.write(f'  {n:<14} ${a:06X} .. ${a + s:06X}  {s:>8,}\n')
        f.write(f'  (free from ${dram_end:06X}: {DRAM_SIZE - dram_end:,} bytes)\n\nHAL\n')
        for n, a, s in hal:
            f.write(f'  {n:<14} ${a:06X}  {s}\n')
        f.write('\nCARTRIDGE\n')
        for sym, addr, ofs, size, fn in cart:
            f.write(f'  {sym:<12} ${addr:06X}  {size:>9,}  {fn or ""}\n')
        f.write(f'  (end ${ROM_BASE + cart_end:06X}, cart {cart_size() >> 20} MB)\n')
    print(f"memory_map: DRAM used to ${dram_end:06X}, cartridge to ${ROM_BASE + cart_end:06X}"
          f" ({cart_size() >> 20} MB)")


if __name__ == '__main__':
    main()
