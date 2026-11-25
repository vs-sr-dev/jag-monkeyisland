; SPDX-License-Identifier: GPL-2.0-or-later
; Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
; developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING.
;===========================================================================
; src/gpu_room.s - the GPU serves the picture's command queue.
;
; M2b (session 6).  Started once at boot (src/main.s GpuStart), the GPU runs
; a loop: whenever GQ_HEAD (the commands the 68000 has written) is ahead of
; GQ_TAIL (the ones done), it executes the next command of the ring GQ_RING
; and moves GQ_TAIL on; when they are equal it waits in its own RAM and looks
; again.  The 68000 writes commands and never waits for each one (csrc/jag/
; jag_main.c gq_put); the video interrupt shows a picture only once GQ_TAIL
; has reached the command count it was handed over with (NEXT_FENCE).
; Both counters run modulo 65536 in long cells whose high word stays 0: the
; 68000 reads a long as two words, and a value that only moves in its low
; word cannot be read torn (JaguarDocs hw/06 sec.11).
;
; A command is GQ_CMD = 32 bytes, 8 longs, the type first:
;   GQ_STRIP   one strip of a Gdi::drawBitmap (plat_drawBitmap):
;                 +4 the stream's address in the game image (after the codec
;                 byte, any alignment), +8 the header (bits 0-7 codec % 10,
;                 bit 8 vertical: codecs 14-18, 34-38, bit 9 transparent:
;                 34-38, 44-48), +12 the strip's top-left pixel in the back
;                 buffer, +16 the back buffer's pitch, +20 the height, +24
;                 where the strip goes in the front buffer (0: nowhere), +28
;                 the back buffer's A2_FLAGS, bit 0 set: the lights are off
;                 (the front strip is cleared, not copied: clear8Col).  The
;                 walk comes from the direction, the pitch and the height.
;   GQ_BLIT    one Blitter copy or fill (csrc/jag/jag_gfx.c q_blit): +4 the
;              destination's address, +8 the source's (a copy), any byte:
;              the GPU splits each into a phrase-aligned base and the pixel
;              x it starts at; +12 A1_FLAGS, +16 A2_FLAGS (the pitch as the
;              WIDTH field, phrase or pixel mode), +20 B_COUNT (rows << 16 |
;              width), +24 B_CMD, +28 the fill colour in all four bytes
;              (B_PATD, both longs).  The step: next row, back by the width.
;   GQ_TABLES  +4 the address of the colour table (256 LONGS, GQ_COLTAB in
;              DRAM), copied into gr_tab: the colour each value is written as
;              (writeRoomColor: _roomPalette); 255 (_transparentColor) has
;              bit 8 set too, and a transparent strip does not write a value
;              whose long has it (the strip's mask, r4).  Longs: loadw/loadb
;              on local RAM perform a 32-bit read (TRM p.49; JaguarDocs hw/06
;              sec.3)
;   GQ_RECTS   rectangles copied 320-pitch to 320-pitch in phrase mode (the
;              screen update and the screen sync, csrc/jag/jag_gfx.c): +4 a
;              list in DRAM of entries of 3 longs (the source's address, the
;              destination's offset, B_COUNT), +8 how many, +12 the
;              destination's base, +16 0, or the source's base: then each
;              source is that base + the entry's offset (the sync, screen to
;              screen).  The 68000 writes each rectangle once, as 3 longs,
;              instead of a command of 8: on the 68000 every command costs.  +20 0, or
;              the text surface: each copy is followed by the text surface's same
;              rectangle composited over it (drawStripToScreen: where it is not
;              CHARSET_MASK_TRANSPARENCY), DCOMPEN + DSTEN in phrase mode with
;              B_PATD = $FD in every byte (JaguarDocs hw/05 rule 5, verified)
;   GQ_CHAR    one glyph (CharsetRendererClassic::drawBitsN): +4 its bits (DRAM),
;              +8 where its first row goes (pitch 320), +12 width | rows << 8 |
;              rows to skip << 16 | bpp << 24 (the 68000 has clipped the rows
;              against the surface: the skipped ones are read, not written),
;              +16..+28 the colour map, 16 bytes
;   GQ_MASK    one z-plane strip (decodeMask) into the mask buffer MASKBUF:
;              copied, ORed, or zeros (see gq_mask)
;   GQ_CEL     one costume cel into the front buffer (plat_drawCel): +4 its
;              block in DRAM, CEL_LONGS longs the 68000 filled (jag_gfx.c
;              plat_drawCel, the CEL_* offsets); see gq_cel
;   GQ_STOP    the GPU sets GPU_DONE and stops (before the GameDrive's
;              Reset(2): a GPU left serving would read whatever the next ROM
;              puts in DRAM)
;
; The strip decoder is ScummVM's drawStripBasicH / drawStripBasicV
; (gfx.cpp), in the form tools/m0_ref.py models and checks against ScummVM
; on every pixel of the 81 rooms.  The stream is read as aligned longs from
; the address rounded down, the bytes before it skipped, and every long
; byte-swapped as it enters the bit buffer (7 instructions per 32 bits): the
; bit stream is least significant bit first, as ScummVM consumes it.
;
; Per strip, the stream: 8 bits of first colour; per pixel, write the colour, then
;   0    same colour
;   10   new colour: the next `width` bits; inc = -1
;   110  colour += inc
;   111  inc = -inc; colour += inc
;
; The copy: the GPU programs the Blitter (JagGOT's gpu_cmd.s, verified on
; hardware: A1_CLIP 0, the pointers and B_COUNT every time, B_CMD last) and
; waits for it from its own RAM, before and after, with a ceiling (GPU_STALL
; counts the waits that ran out; a copy whose Blitter never went idle is
; skipped, never programmed over a running one: JaguarDocs hw/05 rule 4).
; Waiting after keeps the order: a strip decoded next may cover the copy.
; The 68000 splits long blits: one stops it for its whole length (hw/05
; rule 6b).  Phrase mode when both ends and the width are whole phrases,
; pixel mode otherwise (the 68000 chooses: the flags).  A fill writes
; B_PATD (rule 16: PATDSEL, JagLBA's $01810200).
;
; Registers (bank 0; r31, the interrupt stack, never touched):
;   r3 GQ_TAIL        r4 GQ_HEAD as read     r1 the command    r22 its type
;   the blit core (bl_core): r5 dst  r6 src  r7 A1_FLAGS  r15 A2_FLAGS
;                 r10 B_COUNT  r11 B_CMD  r16 B_PATD  r4 where it returns
;   the strip:
;   r4 the strip's transparency mask (0, or bit 8)
;   r5 strip's place  r6 stream    r7 bit buffer      r8 bits left
;   r9 colour         r10 inc      r11 header, then the swap mask $00FF00FF
;   r12-r15 the walk               r16 inner left     r17 dst
;   r18 bit / temp    r19 1        r20 the colour table
;   r21 -1            r23 bits to read                r0 temp
;   jump targets: r2 gr_outer  r24 gr_rc_loop  r25 gr_pix  r26 gq_next
;                 r27 the strip's gr_nc<width>  r28 gr_next
;                 r29 gr_readcol's return       r30 the stream read ahead
;
; A new colour (measured on hardware, M0: 64% of the decode time when read
; one bit at a time, 26.6 clk a bit) is read in one go by gr_nc<width>
; whenever the buffer holds enough bits: one variant per width, because
; shrq takes an immediate (the variable `sh` is the one JRISC instruction
; never checked on silicon: JaguarDocs hw/07 sec.13).  When the buffer runs
; short, the variant falls back to the bit loop gr_readcol.
;
; The stream is read one long AHEAD (r30): a refill takes the long already
; loaded and starts the next load at once (JaguarDocs hw/06 rule 14).
;
; Rules (JaguarDocs hw/06, and JagGOT's gpu_cmd.s, verified on hardware):
;   - external memory read only as aligned longs; bytes written with storeb
;     (measured on DRAM: hw/06 sec.12);
;   - a register loaded from external memory is read before it is written
;     again (TOM 13);
;   - long jumps through registers; a nop in every delay slot; no indexed
;     loads or stores;
;   - a barrier (a load, its value used) before GQ_TAIL moves: the pixels
;     are out before the 68000 can see the command done;
;   - JagLBA's epilogue for the stop: barrier, DONE, barrier, self-stop.
;===========================================================================

GQ_PAUSE        EQU     64              ; idle: ~450 clk between two looks at GQ_HEAD
BLT_WAIT_MAX    EQU     $100000         ; a Blitter wait's ceiling (JagGOT)
BL_COPY         EQU     $01800601       ; SRCEN|UPDA1|UPDA2|LFU source (JagLBA, KD, JagGOT)
BL_FILL         EQU     $01810200       ; PATDSEL|UPDA1|LFU replace (JagLBA B_CMD_FILL)
BL_TEXT         EQU     $09800609       ; the copy + DSTEN + DCOMPEN (JagGOT, phrase mode)

    .68000
; Once at boot, before GpuStart.  The count is rounded UP: the last
; instruction is the halt loop's delay-slot nop (JagLBA).
InitGpu:
    movem.l d0/a0-a1, -(sp)
    move.l  #0, G_CTRL
    lea     GpuRom, a0
    movea.l #G_RAM, a1
    move.w  #(((GpuRomEnd-GpuRom)+3)/4)-1, d0
.ig_copy:
    move.l  (a0)+, (a1)+
    dbra    d0, .ig_copy
    movem.l (sp)+, d0/a0-a1
    rts

GpuRom:
    .gpu
    .org    G_RAM

GpuServer:
    moveq   #1, r19
    movei   #$FFFFFFFF, r21
    movei   #gr_outer, r2
    movei   #gr_rc_loop, r24
    movei   #gr_pix, r25
    movei   #gq_next, r26
    movei   #gr_next, r28
    movei   #GQ_TAIL, r0
    load    (r0), r3                    ; where the queue stands (0 at boot)
    or      r3, r3                      ; read before anything writes it

; --- the queue -----------------------------------------------------------
gq_poll:
    movei   #GQ_HEAD, r0
    load    (r0), r4
    movei   #gq_cmd, r0
    cmp     r3, r4                      ; waits for the load
    jump    NE, (r0)
    nop
    movei   #GQ_PAUSE, r0               ; nothing to do: wait off the bus
gq_pause:
    subq    #1, r0
    jr      NE, gq_pause
    nop
    movei   #gq_poll, r0
    jump    T, (r0)
    nop

gq_cmd:
    move    r3, r1
    movei   #(GQ_N-1), r0
    and     r0, r1
    shlq    #5, r1                      ; x GQ_CMD (32: see the .assert below)
    movei   #GQ_RING, r0
    add     r0, r1
    load    (r1), r22                   ; the type
    addq    #4, r1
    movei   #gq_strip, r0
    cmpq    #GQ_STRIP, r22              ; waits for the load
    jump    EQ, (r0)
    nop
    movei   #gq_blit, r0
    cmpq    #GQ_BLIT, r22
    jump    EQ, (r0)
    nop
    movei   #gq_tables, r0
    cmpq    #GQ_TABLES, r22
    jump    EQ, (r0)
    nop
    movei   #gq_mask, r0
    cmpq    #GQ_MASK, r22
    jump    EQ, (r0)
    nop
    movei   #gq_rects, r0
    cmpq    #GQ_RECTS, r22
    jump    EQ, (r0)
    nop
    movei   #gq_cel, r0
    cmpq    #GQ_CEL, r22
    jump    EQ, (r0)
    nop
    movei   #gq_char, r0
    cmpq    #GQ_CHAR, r22
    jump    EQ, (r0)
    nop
    movei   #gq_stop, r0                ; GQ_STOP (or a stray type: stop)
    jump    T, (r0)
    nop

; A command done: the barrier, then GQ_TAIL + 1.
gq_next:
    movei   #GQ_TAIL, r0
    load    (r0), r18                   ; the barrier: every store before it is out
    or      r18, r18                    ; once its value is here
    addq    #1, r3
    movei   #$FFFF, r18
    and     r18, r3
    store   r3, (r0)
    movei   #gq_poll, r0
    jump    T, (r0)
    nop

; --- GQ_TABLES: 256 longs, DRAM -> gr_tab ----------------------------------
gq_tables:
    load    (r1), r5
    movei   #gr_tab, r17
    movei   #256, r16
gq_tcopy:
    load    (r5), r0                    ; waits for r5
    addq    #4, r5
    subq    #1, r16
    store   r0, (r17)                   ; waits for the load
    addq    #4, r17
    cmpq    #0, r16
    jr      NE, gq_tcopy
    nop
    jump    T, (r26)                    ; gq_next
    nop

;--- GQ_BLIT: one Blitter copy or fill ------------------------------------
gq_blit:
    load    (r1), r5                    ; the destination's address
    addq    #4, r1
    load    (r1), r6                    ; the source's
    addq    #4, r1
    load    (r1), r7                    ; A1_FLAGS
    addq    #4, r1
    load    (r1), r15                   ; A2_FLAGS
    addq    #4, r1
    load    (r1), r10                   ; B_COUNT
    addq    #4, r1
    load    (r1), r11                   ; B_CMD
    addq    #4, r1
    load    (r1), r16                   ; B_PATD's long
    move    r26, r4                     ; then gq_next
; bl_core: one blit from the registers above; back to r4
bl_core:
    moveq   #7, r8
    and     r5, r8                      ; A1_PIXEL: x in its phrase, y 0
    sub     r8, r5                      ; A1_BASE
    moveq   #7, r17
    and     r6, r17                     ; A2_PIXEL
    sub     r17, r6                     ; A2_BASE
    move    r10, r9                     ; the step: y + 1, x - the width
    shlq    #16, r9
    shrq    #16, r9                     ; the width
    neg     r9
    shlq    #16, r9
    shrq    #16, r9
    movei   #$10000, r0
    or      r0, r9
    or      r16, r16                    ; read before the Blitter's writes
    movei   #ISR_BUSY, r0               ; not while the video interrupt re-arms the
gq_isrw:                                ; list (a blit would stop it: lost OP rows)
    load    (r0), r14
    moveq   #31, r13
gq_isrp:
    subq    #1, r13                     ; a pause between two looks (off the bus)
    jr      NE, gq_isrp
    nop
    cmpq    #0, r14
    jr      NE, gq_isrw
    nop
    movei   #B_CMD, r12
    movei   #BLT_WAIT_MAX, r13
gq_cw0:
    load    (r12), r14
    btst    #0, r14                     ; waits for the load: idle?
    jr      NE, gq_cw0ok
    nop
    subq    #1, r13
    jr      NE, gq_cw0
    nop
    movei   #gq_stall, r0               ; never idle: count it, skip the copy
    jump    T, (r0)
    nop
gq_cw0ok:
    movei   #A1_CLIP, r0
    moveq   #0, r13
    store   r13, (r0)
    movei   #A1_BASE, r0
    store   r5, (r0)
    movei   #A1_FLAGS, r0
    store   r7, (r0)
    movei   #A1_PIXEL, r0
    store   r8, (r0)
    movei   #A1_STEP, r0
    store   r9, (r0)
    movei   #A2_BASE, r0
    store   r6, (r0)
    movei   #A2_FLAGS, r0
    store   r15, (r0)
    movei   #A2_PIXEL, r0
    store   r17, (r0)
    movei   #A2_STEP, r0
    store   r9, (r0)
    movei   #B_PATD, r0                 ; a data register: the Blitter is idle
    store   r16, (r0)
    addq    #4, r0
    store   r16, (r0)
    movei   #B_COUNT, r0
    store   r10, (r0)
    store   r11, (r12)                  ; B_CMD last: the blit starts
    movei   #BLT_WAIT_MAX, r13
gq_cw1:
    load    (r12), r14
    btst    #0, r14
    jr      NE, gq_cw1ok
    nop
    subq    #1, r13
    jr      NE, gq_cw1
    nop
    movei   #gq_stall, r0
    jump    T, (r0)
    nop
gq_cw1ok:
    jump    T, (r4)
    nop
gq_stall:
    movei   #GPU_STALL, r0
    load    (r0), r14
    or      r14, r14
    addq    #1, r14
    store   r14, (r0)
    jump    T, (r4)
    nop

; --- the strip into the front buffer: copy8Col (or clear8Col), 8 x height --
gs_front:
    load    (r1), r5                    ; +24 the front buffer's place, or 0
    move    r1, r0
    subq    #12, r0
    load    (r0), r6                    ; +12 the strip's place: the source
    addq    #4, r0
    addq    #4, r0
    load    (r0), r10                   ; +20 the height
    addq    #4, r1
    load    (r1), r15                   ; +28 A2_FLAGS | the lights off
    or      r6, r6                      ; each read (TOM 13)
    cmpq    #0, r5
    jump    EQ, (r26)                   ; nowhere: gq_next
    nop
    shlq    #16, r10                    ; B_COUNT: h rows of 8
    addq    #8, r10
    movei   #BLIT_FLAGS320, r7
    moveq   #1, r0
    and     r15, r0                     ; the lights off?
    sub     r0, r15                     ; A2_FLAGS
    moveq   #0, r16                     ; the clear's colour
    movei   #BL_COPY, r11
    cmpq    #0, r0
    jr      EQ, gs_fcopy
    nop
    movei   #BL_FILL, r11
gs_fcopy:
    move    r26, r4                     ; then gq_next
    movei   #bl_core, r0
    jump    T, (r0)
    nop

; --- GQ_RECTS: a list of copies, 320 to 320, phrase mode ---------------------
; r18 the entry   r20 entries left   r22 the destination's base
; r23 the source's base (0: each entry's own source)   r27 the text
; surface (0: none)   r29 the entry's destination offset
gq_rects:
    load    (r1), r18                   ; the list
    addq    #4, r1
    load    (r1), r20                   ; how many
    addq    #4, r1
    load    (r1), r22                   ; the destination's base
    addq    #4, r1
    load    (r1), r23                   ; the source's base, or 0
    addq    #4, r1
    load    (r1), r27                   ; the text surface, or 0
    or      r18, r18                    ; each read (TOM 13)
    or      r22, r22
    or      r23, r23
    or      r27, r27
    movei   #BLIT_FLAGS320, r7
    move    r7, r15
    cmpq    #0, r20                     ; waits for the count
    jump    EQ, (r26)                   ; none: gq_next
    nop
gq_rone:
    load    (r18), r6                   ; the source
    addq    #4, r18
    load    (r18), r5                   ; the destination's offset
    addq    #4, r18
    load    (r18), r10                  ; B_COUNT
    addq    #4, r18
    or      r6, r6                      ; read (TOM 13)
    move    r5, r29                     ; the offset, for the text surface
    cmpq    #0, r23
    jr      EQ, gq_rown
    nop
    move    r23, r6                     ; the sync: the source's base + offset
    add     r5, r6
gq_rown:
    add     r22, r5                     ; the destination
    or      r10, r10
    movei   #BL_COPY, r11
    moveq   #0, r16
    movei   #gq_rtext, r4
    movei   #bl_core, r0
    jump    T, (r0)
    nop
gq_rtext:
    movei   #gq_rnext, r4
    cmpq    #0, r27
    jump    EQ, (r4)                    ; no text surface
    nop
    move    r29, r5
    add     r22, r5                     ; the same destination
    move    r29, r6
    add     r27, r6                     ; the text surface's rectangle (screen coordinates)
    movei   #BL_TEXT, r11
    movei   #$FDFDFDFD, r16             ; CHARSET_MASK_TRANSPARENCY: not written
    movei   #bl_core, r0
    jump    T, (r0)
    nop
gq_rnext:
    movei   #gq_rone, r0
    subq    #1, r20
    jump    NE, (r0)
    nop
    jump    T, (r26)                    ; gq_next
    nop

; --- GQ_STRIP: one strip ------------------------------------------------
gq_strip:
    load    (r1), r6                    ; the stream, after its codec byte
    addq    #4, r1
    load    (r1), r11                   ; the header
    addq    #4, r1
    load    (r1), r5                    ; the strip's place in the buffer
    addq    #4, r1
    load    (r1), r14                   ; the back buffer's pitch
    addq    #4, r1
    load    (r1), r15                   ; the height
    addq    #4, r1                      ; r1: +24, read when the strip is done
    moveq   #3, r23
    and     r6, r23                     ; waits for r6: the bytes to skip
    sub     r23, r6                     ; the long that holds the first byte
    or      r30, r30                    ; the last strip's prefetch, read (TOM 13)
    load    (r6), r30                   ; prefetch the stream's first long
    addq    #4, r6
; the walk: horizontal strips row by row, vertical ones column by column
    btst    #8, r11                     ; waits for the header
    jr      NE, gs_vert
    nop
    moveq   #1, r12                     ; inner step 1, 8 pixels
    moveq   #8, r13
    subq    #8, r14                     ; outer step: pitch - 8; outer count: h
    jr      T, gs_walk
    nop
gs_vert:
    move    r14, r12                    ; inner step: the pitch, h pixels
    move    r15, r13
    move    r15, r0
    mult    r14, r0                     ; h x pitch (both under 65536)
    moveq   #1, r14
    sub     r0, r14                     ; outer step: 1 - h x pitch
    moveq   #8, r15                     ; 8 columns
gs_walk:
    movei   #gr_tab, r20                ; the colour table
    moveq   #0, r4                      ; every value written,
    btst    #9, r11
    jr      EQ, gr_tab1
    nop
    movei   #$100, r4                   ; or all but _transparentColor
gr_tab1:
    movei   #$FF, r0
    and     r0, r11                     ; colour-field width (4..8)
    move    r11, r27
    subq    #4, r27
    shlq    #2, r27
    movei   #gr_nctab, r0
    add     r0, r27
    load    (r27), r27                  ; local RAM: this width's variant
    move    r5, r17                     ; dst = the strip's place
    movei   #$00FF00FF, r11             ; the byte swap's mask, from here on
; the first long into the buffer, swapped, then the bytes before the strip
    move    r30, r7                     ; waits for the prefetch
    load    (r6), r30                   ; and the next one, in flight
    addq    #4, r6
    rorq    #16, r7                     ; b0b1b2b3 -> b2b3b0b1
    move    r7, r0
    and     r11, r7                     ; 00 b3 00 b1
    shlq    #8, r7                      ; b3 00 b1 00
    shrq    #8, r0                      ; 00 b2 b3 b0
    and     r11, r0                     ; 00 b2 00 b0
    or      r0, r7                      ; b3 b2 b1 b0: b0 in the low bits
    moveq   #31, r8                     ; 32 bits
    addq    #1, r8                     ; 32 bits
gr_skip:
    cmpq    #0, r23
    jr      EQ, gr_skipped
    nop
    shrq    #8, r7
    subq    #8, r8
    subq    #1, r23
    jr      T, gr_skip
    nop
gr_skipped:
    moveq   #8, r23                     ; the first colour: 8 bits
    movei   #gr_first, r29
    movei   #gr_readcol, r0
    jump    T, (r0)
    nop
gr_first:
    move    r21, r10                    ; inc = -1

; --- the walk -----------------------------------------------------------
gr_outer:
    move    r13, r16                    ; inner count
gr_pix:
    move    r9, r0                      ; writeRoomColor: the table's long for
    shlq    #24, r0                     ; the colour's low byte
    shrq    #22, r0
    add     r20, r0
    load    (r0), r0
    move    r0, r18                     ; waits for the load
    and     r4, r18                     ; not written in this strip?
    jr      NE, gr_nowr
    nop
    storeb  r0, (r17)
gr_nowr:
    add     r12, r17
; bit A: 0 = same colour
    cmpq    #0, r8
    jr      NE, gr_a
    nop
    move    r30, r7                     ; the long prefetched last time
    load    (r6), r30                   ; and the next one, in flight
    addq    #4, r6
    rorq    #16, r7                     ; byte swap (see gq_strip)
    move    r7, r0
    and     r11, r7
    shlq    #8, r7
    shrq    #8, r0
    and     r11, r0
    or      r0, r7
    moveq   #31, r8                     ; 32 bits
    addq    #1, r8
gr_a:
    move    r7, r18
    shrq    #1, r7
    subq    #1, r8
    and     r19, r18
    jump    EQ, (r28)
    nop
; bit B: 0 = a new colour
    cmpq    #0, r8
    jr      NE, gr_b
    nop
    move    r30, r7                     ; the long prefetched last time
    load    (r6), r30                   ; and the next one, in flight
    addq    #4, r6
    rorq    #16, r7                     ; byte swap (see gq_strip)
    move    r7, r0
    and     r11, r7
    shlq    #8, r7
    shrq    #8, r0
    and     r11, r0
    or      r0, r7
    moveq   #31, r8                     ; 32 bits
    addq    #1, r8
gr_b:
    move    r7, r18
    shrq    #1, r7
    subq    #1, r8
    and     r19, r18
    jr      NE, gr_c
    nop
    move    r21, r10                    ; inc = -1
    jump    T, (r27)                    ; gr_nc<width>, back to gr_next
    nop
; bit C: 0 = colour + inc, 1 = inc reversed first
gr_c:
    cmpq    #0, r8
    jr      NE, gr_c2
    nop
    move    r30, r7                     ; the long prefetched last time
    load    (r6), r30                   ; and the next one, in flight
    addq    #4, r6
    rorq    #16, r7                     ; byte swap (see gq_strip)
    move    r7, r0
    and     r11, r7
    shlq    #8, r7
    shrq    #8, r0
    and     r11, r0
    or      r0, r7
    moveq   #31, r8                     ; 32 bits
    addq    #1, r8
gr_c2:
    move    r7, r18
    shrq    #1, r7
    subq    #1, r8
    and     r19, r18
    jr      NE, gr_flip
    nop
    add     r10, r9
    jump    T, (r28)
    nop
gr_flip:
    neg     r10
    add     r10, r9
gr_next:
    subq    #1, r16
    jump    NE, (r25)                   ; gr_pix
    nop
    add     r14, r17
    subq    #1, r15
    jump    NE, (r2)                    ; gr_outer
    nop
    movei   #gs_front, r0               ; the strip is done
    jump    T, (r0)
    nop

; --- gr_readcol: r23 bits, least significant first, into r9; back to r29 --
gr_readcol:
    moveq   #0, r9
    moveq   #1, r22                     ; the bit's weight
gr_rc_loop:
    cmpq    #0, r8
    jr      NE, gr_rc_have
    nop
    move    r30, r7                     ; the long prefetched last time
    load    (r6), r30                   ; and the next one, in flight
    addq    #4, r6
    rorq    #16, r7                     ; byte swap (see gq_strip)
    move    r7, r0
    and     r11, r7
    shlq    #8, r7
    shrq    #8, r0
    and     r11, r0
    or      r0, r7
    moveq   #31, r8                     ; 32 bits
    addq    #1, r8
gr_rc_have:
    move    r7, r18
    shrq    #1, r7
    subq    #1, r8
    and     r19, r18                    ; 0 or 1
    neg     r18                         ; 0 or all ones
    and     r22, r18
    or      r18, r9
    add     r22, r22
    subq    #1, r23
    jump    NE, (r24)                   ; gr_rc_loop
    nop
    jump    T, (r29)
    nop

; --- gr_nc<k>: a new colour of k bits into r9, then gr_next.  Fast when the
; buffer holds k bits or more (r8 - k not negative); otherwise the bit loop.
gr_nc4:
    move    r8, r0
    subq    #4, r0
    jr      MI, gr_nc4s
    nop
    move    r7, r9
    movei   #$0F, r0
    and     r0, r9
    shrq    #4, r7
    subq    #4, r8
    jump    T, (r28)
    nop
gr_nc4s:
    moveq   #4, r23
    movei   #gr_ncslow, r0
    jump    T, (r0)
    nop

gr_nc5:
    move    r8, r0
    subq    #5, r0
    jr      MI, gr_nc5s
    nop
    move    r7, r9
    movei   #$1F, r0
    and     r0, r9
    shrq    #5, r7
    subq    #5, r8
    jump    T, (r28)
    nop
gr_nc5s:
    moveq   #5, r23
    movei   #gr_ncslow, r0
    jump    T, (r0)
    nop

gr_nc6:
    move    r8, r0
    subq    #6, r0
    jr      MI, gr_nc6s
    nop
    move    r7, r9
    movei   #$3F, r0
    and     r0, r9
    shrq    #6, r7
    subq    #6, r8
    jump    T, (r28)
    nop
gr_nc6s:
    moveq   #6, r23
    movei   #gr_ncslow, r0
    jump    T, (r0)
    nop

gr_nc7:
    move    r8, r0
    subq    #7, r0
    jr      MI, gr_nc7s
    nop
    move    r7, r9
    movei   #$7F, r0
    and     r0, r9
    shrq    #7, r7
    subq    #7, r8
    jump    T, (r28)
    nop
gr_nc7s:
    moveq   #7, r23
    movei   #gr_ncslow, r0
    jump    T, (r0)
    nop

gr_nc8:
    move    r8, r0
    subq    #8, r0
    jr      MI, gr_nc8s
    nop
    move    r7, r9
    movei   #$FF, r0
    and     r0, r9
    shrq    #8, r7
    subq    #8, r8
    jump    T, (r28)
    nop
gr_nc8s:
    moveq   #8, r23
gr_ncslow:
    movei   #gr_next, r29
    movei   #gr_readcol, r0
    jump    T, (r0)
    nop

; --- GQ_MASK: one z-plane strip into the mask buffer (decodeMask) ----------
; +4 the strip's RLE in the game image (any alignment; 0: an empty strip,
; zeros), +8 where its first row goes, +12 0 to copy, or, to OR over what is
; there, the byte's lane in its long (+8 is then that long: the GPU reads
; external memory only as aligned longs, and a row down is 40 bytes, so the
; lane stays), +16 the height.  The RLE (decompressMaskImg/Or): a control
; byte b, then, bit 7 set, one byte written b & $7F times, else b bytes
; written as they come; a count of 0 is 256 (ScummVM's uint8 do/while);
; the strip ends when the height is done, even inside a count.
; Registers: r6 the RLE   r7 its long   r8 bytes left in r7   r9 the count
;   r22 the byte   r17 dst   r15 lane / 0   r16 rows left   r12 40
;   r11 $FF   r10 $7F   r4 where a count goes on (gm_lit or gm_put)
;   r5 gm_loop   r13 gm_put   r14 gm_lit   r18 r20 r23 the OR's temps
gq_mask:
    load    (r1), r6                    ; the RLE, or 0
    addq    #4, r1
    load    (r1), r17                   ; dst
    addq    #4, r1
    load    (r1), r15                   ; the lane, or 0: copy
    addq    #4, r1
    load    (r1), r16                   ; the height
    movei   #40, r12                    ; NUM_STRIPS: one row down
    movei   #$FF, r11
    movei   #$7F, r10
    movei   #gm_loop, r5
    movei   #gm_put, r13
    movei   #gm_lit, r14
    cmpq    #0, r6                      ; waits for the load
    jr      NE, gm_rle
    nop
; an empty strip (copied: the 68000 sends none to OR): zeros
    moveq   #0, r22
gm_zero:
    storeb  r22, (r17)
    add     r12, r17
    subq    #1, r16
    jr      NE, gm_zero
    nop
    jump    T, (r26)                    ; gq_next
    nop
gm_rle:
    moveq   #3, r23
    and     r6, r23                     ; the bytes to skip
    sub     r23, r6                     ; the long that holds the first one
    load    (r6), r7
    addq    #4, r6
    moveq   #4, r8
gm_skip:
    cmpq    #0, r23
    jr      EQ, gm_loop
    nop
    rorq    #24, r7                     ; waits for the load; a byte skipped
    subq    #1, r8
    subq    #1, r23
    jr      T, gm_skip
    nop
; a control byte (the height is not done)
gm_loop:
    cmpq    #0, r8
    jr      NE, gm_b0
    nop
    load    (r6), r7
    addq    #4, r6
    moveq   #4, r8
gm_b0:
    rorq    #24, r7                     ; big-endian: the next byte comes to the bottom
    move    r7, r22
    and     r11, r22
    subq    #1, r8
    move    r22, r9
    and     r10, r9                     ; the count
    jr      NE, gm_n
    nop
    movei   #256, r9
gm_n:
    btst    #7, r22
    jr      NE, gm_run
    nop
    move    r14, r4                     ; literal bytes: a new one each row
    jump    T, (r14)                    ; gm_lit
    nop
gm_run:
    move    r13, r4                     ; a run: the same byte each row
    cmpq    #0, r8
    jr      NE, gm_b1
    nop
    load    (r6), r7
    addq    #4, r6
    moveq   #4, r8
gm_b1:
    rorq    #24, r7
    move    r7, r22
    and     r11, r22
    subq    #1, r8
    jump    T, (r13)                    ; gm_put
    nop
gm_lit:
    cmpq    #0, r8
    jr      NE, gm_b2
    nop
    load    (r6), r7
    addq    #4, r6
    moveq   #4, r8
gm_b2:
    rorq    #24, r7
    move    r7, r22
    and     r11, r22
    subq    #1, r8
; the byte in r22 to its row, then one row down
gm_put:
    cmpq    #0, r15
    jr      NE, gm_or
    nop
    storeb  r22, (r17)
    jr      T, gm_down
    nop
gm_or:
    load    (r17), r18                  ; the long that holds the byte
    move    r22, r20
    shlq    #8, r20
    or      r22, r20
    move    r20, r23
    shlq    #16, r23
    or      r23, r20                    ; the byte in all four lanes
    and     r15, r20                    ; in its own
    or      r20, r18                    ; waits for the load
    store   r18, (r17)
gm_down:
    add     r12, r17
    subq    #1, r16
    jump    EQ, (r26)                   ; the height done: gq_next
    nop
    subq    #1, r9
    jump    EQ, (r5)                    ; the count done: gm_loop
    nop
    jump    T, (r4)                     ; gm_lit or gm_put
    nop

; --- GQ_CEL: a costume cel (BaseCostumeRenderer::byleRLEDecode, ScummVM's
; unoptimised version as csrc/ref/costume_ref.c has it, v4's x scale step).
; The block is copied into gc_blk first (the 68000 reuses it), its palette
; widened into gc_pal (local RAM is read 32 bits at a time).  The RLE comes
; from the game image in the cartridge as aligned longs, a byte at a time
; (as gq_mask); the mask byte and the scale table are read with loadb from
; DRAM (right on hardware: JaguarDocs hw/06, T-GPU-11).
; A run: a byte, colour = byte >> shr, count = byte & mask, a count of 0 in
; the next byte (and a byte of 0 there: 256, ScummVM's uint8 do/while).
; Per pixel of a column: the row is drawn unless scaled away (scaleY: the
; table at the row's index, the index moving only when scaled); drawn means
; written if the colour is not 0 and y, x are inside the bounds and the
; z-plane's bit is clear; then dst and mask one row down.
; Registers: r6 the RLE  r7 its long  r8 bytes left  r9 the run's count
;   r10 its colour  r20 its palette value (or the byte read)  r11 $FF
;   r12 y  r13 x  r14 rows left  r15 columns left  r16 dst  r17 mask
;   r18 maskbit  r22 scale index y  r23 scaleY (-1: not scaled)  r27 the
;   bottom bound  r29 1 if x is inside the bounds  r30 the scale table
;   r1 gc_rd's return, r4 gc_byte's and gc_colset's, r0 r5 temps
gq_cel:
    load    (r1), r4                    ; the block
    or      r30, r30                    ; the last strip's prefetch, read (TOM 13)
    movei   #$FF, r11
    movei   #gc_blk, r5
    moveq   #CEL_LONGS, r0
gc_cp:
    load    (r4), r20
    addq    #4, r4
    store   r20, (r5)                   ; waits for the load
    addq    #4, r5
    subq    #1, r0
    jr      NE, gc_cp
    nop
; the palette: 8 longs of 4 bytes -> 32 longs
    movei   #(gc_blk+(CEL_PAL*4)), r4
    movei   #gc_pal, r5
    moveq   #8, r0
    movei   #gc_pe, r18
gc_pe:
    load    (r4), r20
    addq    #4, r4
    move    r20, r9
    shrq    #24, r9
    store   r9, (r5)
    addq    #4, r5
    move    r20, r9
    shrq    #16, r9
    and     r11, r9
    store   r9, (r5)
    addq    #4, r5
    move    r20, r9
    shrq    #8, r9
    and     r11, r9
    store   r9, (r5)
    addq    #4, r5
    and     r11, r20
    store   r20, (r5)
    addq    #4, r5
    subq    #1, r0
    jump    NE, (r18)
    nop
; the state
    movei   #gc_blk, r4
    load    (r4), r6                    ; CEL_SRC
    addq    #4, r4
    load    (r4), r9                    ; CEL_SKIPN: skipCelLines' num, or 0
    addq    #4, r4
    load    (r4), r16                   ; CEL_DEST: the first column's top
    movei   #(gc_blk+(CEL_X*4)), r4
    load    (r4), r13
    addq    #4, r4
    load    (r4), r12                   ; CEL_Y
    addq    #4, r4
    load    (r4), r14                   ; CEL_H
    addq    #4, r4
    load    (r4), r15                   ; CEL_SKIPW
    movei   #(gc_blk+(CEL_SCALEY*4)), r4
    load    (r4), r23
    movei   #(gc_blk+(CEL_SYI*4)), r4
    load    (r4), r22
    movei   #(gc_blk+(CEL_BB*4)), r4
    load    (r4), r27
    movei   #(gc_blk+(CEL_TABLE*4)), r4
    load    (r4), r30
    movei   #gc_st1, r4
    movei   #gc_colset, r0
    jump    T, (r0)
    nop
gc_st1:
; the RLE: the long that holds its first byte, the bytes before it skipped
    moveq   #3, r20
    and     r6, r20
    sub     r20, r6
    load    (r6), r7
    addq    #4, r6
    moveq   #4, r8
gc_sk:
    cmpq    #0, r20
    jr      EQ, gc_skd
    nop
    rorq    #24, r7                     ; waits for the load
    subq    #1, r8
    subq    #1, r20
    jr      T, gc_sk
    nop
gc_skd:
    movei   #gc_run, r0
    cmpq    #0, r9
    jump    EQ, (r0)                    ; no columns to skip: the first run
    nop
; skipCelLines: runs counted off until num is used up; the last run goes on
; with 1 - num pixels (then --len: StartPos)
gc_skip:
    movei   #gc_skr, r1
    movei   #gc_rd, r0
    jump    T, (r0)
    nop
gc_skr:
    movei   #gc_skend, r0
    sub     r20, r9                     ; num -= the count
    jump    EQ, (r0)
    nop
    jump    MI, (r0)
    nop
    movei   #gc_skip, r0
    jump    T, (r0)
    nop
gc_skend:
    moveq   #1, r20
    sub     r9, r20                     ; 1 - num
    move    r20, r9
    move    r10, r20                    ; the run's palette value
    shlq    #2, r20
    movei   #gc_pal, r0
    add     r0, r20
    load    (r20), r20
    movei   #gc_lenchk, r0
    jump    T, (r0)
    nop
; a run
gc_run:
    movei   #gc_run1, r1
    movei   #gc_rd, r0
    jump    T, (r0)
    nop
gc_run1:
    move    r20, r9
    cmpq    #0, r9
    jr      NE, gc_run2
    nop
    movei   #256, r9
gc_run2:
    move    r10, r20                    ; its palette value
    shlq    #2, r20
    movei   #gc_pal, r0
    add     r0, r20
    load    (r20), r20
; a pixel
gc_pix:
    movei   #gc_draw, r0
    cmpq    #0, r23
    jump    MI, (r0)                    ; not scaled: every row
    nop
    move    r22, r0
    addq    #1, r22                     ; scaleIndexY++
    shlq    #24, r0
    shrq    #24, r0                     ; & $FF
    add     r30, r0
    loadb   (r0), r0
    movei   #gc_rowskip, r5
    cmp     r23, r0                     ; the table - scaleY: a borrow when less
    jump    CC, (r5)                    ; not less: the row is scaled away
    nop
gc_draw:
    cmpq    #0, r10                     ; colour 0: not written
    jr      EQ, gc_adv
    nop
    cmpq    #0, r29                     ; x outside the bounds
    jr      EQ, gc_adv
    nop
    cmp     r27, r12                    ; y - the bottom: a borrow when 0 <= y < it
    jr      CC, gc_adv
    nop
    loadb   (r17), r0                   ; the z-plane's byte
    and     r18, r0                     ; waits for the load: masked?
    jr      NE, gc_adv
    nop
    storeb  r20, (r16)
gc_adv:
    movei   #320, r0
    add     r0, r16
    movei   #40, r0
    add     r0, r17
    addq    #1, r12
gc_rowskip:
    movei   #gc_col, r0
    subq    #1, r14
    jump    EQ, (r0)                    ; the column is done
    nop
gc_lenchk:
    movei   #gc_pix, r0
    subq    #1, r9
    jump    NE, (r0)
    nop
    movei   #gc_run, r0
    jump    T, (r0)
    nop
; a column done: the next one, or the end
gc_col:
    subq    #1, r15
    jump    EQ, (r26)                   ; skipWidth used up: gq_next
    nop
    movei   #(gc_blk+(CEL_H*4)), r0
    load    (r0), r14
    movei   #(gc_blk+(CEL_Y*4)), r0
    load    (r0), r12
    movei   #(gc_blk+(CEL_SYI*4)), r0
    load    (r0), r22
    movei   #(gc_blk+(CEL_SCALEX*4)), r4
    load    (r4), r5                    ; scaleX, -1: not scaled
    movei   #(gc_blk+(CEL_SXI*4)), r4
    load    (r4), r1                    ; scaleXIndex
    movei   #gc_xstep, r0
    cmpq    #0, r5
    jump    MI, (r0)
    nop
    move    r1, r0
    shlq    #24, r0
    shrq    #24, r0
    add     r30, r0
    loadb   (r0), r0
    movei   #gc_xidx, r4
    cmp     r5, r0                      ; the table - scaleX
    jump    CC, (r4)                    ; not less: x stays
    nop
gc_xstep:
    movei   #(gc_blk+(CEL_STEP*4)), r4
    load    (r4), r0                    ; +1 or -1
    add     r0, r13                     ; x += step
    movei   #(gc_blk+(CEL_DEST*4)), r4
    load    (r4), r5
    add     r0, r5                      ; destPtr += step
    store   r5, (r4)
    movei   #(gc_blk+(CEL_BR*4)), r4
    load    (r4), r0
    cmp     r0, r13                     ; x - the right bound: a borrow when inside
    jump    CC, (r26)                   ; outside: the end (gq_next)
    nop
gc_xidx:
    addq    #1, r1
    shlq    #24, r1
    shrq    #24, r1                     ; (scaleXIndex + 1) & $FF
    movei   #(gc_blk+(CEL_SXI*4)), r4
    store   r1, (r4)
    movei   #(gc_blk+(CEL_DEST*4)), r4
    load    (r4), r16                   ; dst = destPtr
    movei   #gc_lenchk, r4              ; then StartPos: --len
    movei   #gc_colset, r0
    jump    T, (r0)
    nop

; gc_colset: the column's mask pointer (the mask row + x / 8), maskbit
; (revBitMask(x & 7)) and r29 = x inside the bounds; back to r4
gc_colset:
    movei   #(gc_blk+(CEL_MROW*4)), r0
    load    (r0), r17
    move    r13, r0
    sharq   #3, r0
    add     r0, r17                     ; read only when x >= 0
    moveq   #7, r18
    and     r13, r18
    shlq    #2, r18
    movei   #gc_bits, r0
    add     r0, r18
    load    (r18), r18
    movei   #(gc_blk+(CEL_BR*4)), r0
    load    (r0), r0
    moveq   #0, r29
    cmp     r0, r13                     ; x - the right bound
    jump    CC, (r4)                    ; not inside (x < 0 is a large unsigned)
    nop
    moveq   #1, r29
    jump    T, (r4)
    nop

; gc_rd: a run's header into r10 (colour) and r20 (count); back to r1
gc_rd:
    movei   #gc_rd1, r4
    movei   #gc_byte, r0
    jump    T, (r0)
    nop
gc_rd1:
    move    r20, r10
    movei   #(gc_blk+(CEL_CSHR*4)), r0
    load    (r0), r0
    cmpq    #4, r0
    jr      EQ, gc_rd4
    nop
    shrq    #3, r10                     ; 32 colours
    jr      T, gc_rd5
    nop
gc_rd4:
    shrq    #4, r10                     ; 16 colours
gc_rd5:
    movei   #(gc_blk+(CEL_CMASK*4)), r0
    load    (r0), r0
    and     r0, r20
    jump    NE, (r1)
    nop
    move    r1, r4                      ; 0: the count is the next byte
    movei   #gc_byte, r0
    jump    T, (r0)
    nop

; gc_byte: the RLE's next byte into r20; back to r4
gc_byte:
    cmpq    #0, r8
    jr      NE, gc_by1
    nop
    load    (r6), r7
    addq    #4, r6
    moveq   #4, r8
gc_by1:
    rorq    #24, r7                     ; big-endian: the next byte to the bottom
    move    r7, r20
    and     r11, r20
    subq    #1, r8
    jump    T, (r4)
    nop

; --- GQ_CHAR: one glyph, drawBitsN ------------------------------------------
; The bits run on across the rows (one stream), a byte at a time from DRAM
; (loadb); bpp 1, 2, 4 or 8: colour = bits >> (8 - bpp), then bits <<= bpp
; (a byte), a new byte when 8 bits are used.  A colour is written, through
; the colour map (widened into gc_pal), unless it is 0 or its row is one
; of the skipped ones.
; Registers: r6 the bits  r7 the byte  r8 bits left in it  r9 the colour
;   r11 $FF  r12 columns left  r13 the width  r14 rows left  r15 rows to
;   skip  r16 dst  r17 the row step (320 - width)  r18 gc_pal  r27 this
;   bpp's extractor  r29 its return  r10 r20 r0 temps
gq_char:
    load    (r1), r6                    ; the bits
    addq    #4, r1
    load    (r1), r16                   ; the first row
    addq    #4, r1
    load    (r1), r10                   ; width | rows | skip | bpp
    addq    #4, r1
    movei   #$FF, r11
    movei   #gc_pal, r18
    moveq   #4, r12
    movei   #gch_cm, r27
gch_cm:
    load    (r1), r20                   ; 4 bytes of the colour map
    addq    #4, r1
    move    r20, r9
    shrq    #24, r9
    store   r9, (r18)
    addq    #4, r18
    move    r20, r9
    shrq    #16, r9
    and     r11, r9
    store   r9, (r18)
    addq    #4, r18
    move    r20, r9
    shrq    #8, r9
    and     r11, r9
    store   r9, (r18)
    addq    #4, r18
    and     r11, r20
    store   r20, (r18)
    addq    #4, r18
    subq    #1, r12
    jump    NE, (r27)
    nop
    movei   #gc_pal, r18
    or      r6, r6                      ; each read (TOM 13)
    or      r16, r16
    move    r10, r13
    and     r11, r13                    ; the width
    move    r10, r14
    shrq    #8, r14
    and     r11, r14                    ; the rows
    move    r10, r15
    shrq    #16, r15
    and     r11, r15                    ; the rows to skip
    shrq    #24, r10                    ; bpp
    movei   #gch_vok, r0
    movei   #gch_v1, r27
    cmpq    #1, r10
    jump    EQ, (r0)
    nop
    movei   #gch_v2, r27
    cmpq    #2, r10
    jump    EQ, (r0)
    nop
    movei   #gch_v4, r27
    cmpq    #4, r10
    jump    EQ, (r0)
    nop
    movei   #gch_v8, r27
gch_vok:
    movei   #320, r17
    sub     r13, r17                    ; the row step
    cmpq    #0, r14
    jump    EQ, (r26)                   ; no rows: gq_next
    nop
    loadb   (r6), r7                    ; bits = *src++
    addq    #1, r6
    moveq   #8, r8
gch_row:
    move    r13, r12
gch_px:
    movei   #gch_back, r29
    jump    T, (r27)                    ; r9 = the colour
    nop
gch_back:
    cmpq    #0, r9
    jr      EQ, gch_nw
    nop
    cmpq    #0, r15                     ; a skipped row
    jr      NE, gch_nw
    nop
    shlq    #2, r9
    add     r18, r9
    load    (r9), r9                    ; the colour map
    storeb  r9, (r16)
gch_nw:
    addq    #1, r16
    cmpq    #0, r8
    jr      NE, gch_hb
    nop
    loadb   (r6), r7                    ; a new byte
    addq    #1, r6
    moveq   #8, r8
gch_hb:
    movei   #gch_px, r0
    subq    #1, r12
    jump    NE, (r0)
    nop
    add     r17, r16
    cmpq    #0, r15
    jr      EQ, gch_ns
    nop
    subq    #1, r15
gch_ns:
    movei   #gch_row, r0
    subq    #1, r14
    jump    NE, (r0)
    nop
    jump    T, (r26)                    ; gq_next
    nop
; the extractors: r9 = the colour, the byte shifted on (kept 8 bits)
gch_v1:
    move    r7, r9                      ; waits for a pending loadb
    shrq    #7, r9
    shlq    #1, r7
    and     r11, r7
    subq    #1, r8
    jump    T, (r29)
    nop
gch_v2:
    move    r7, r9
    shrq    #6, r9
    shlq    #2, r7
    and     r11, r7
    subq    #2, r8
    jump    T, (r29)
    nop
gch_v4:
    move    r7, r9
    shrq    #4, r9
    shlq    #4, r7
    and     r11, r7
    subq    #4, r8
    jump    T, (r29)
    nop
gch_v8:
    move    r7, r9
    moveq   #0, r7
    subq    #8, r8
    jump    T, (r29)
    nop

; --- GQ_STOP: the command done, then JagLBA's epilogue (GpuCommon_Done),
; verified on hardware.  The barrier's register (r30) is never reused for
; the value: the load lands late.
gq_stop:
    movei   #GQ_TAIL, r0
    addq    #1, r3
    movei   #$FFFF, r18
    and     r18, r3
    store   r3, (r0)
    or      r30, r30                    ; the last prefetch lands before r30 is reused
    movei   #GPU_DONE, r0
    load    (r0), r30
    nop
    nop
    nop
    nop
    nop
    nop
    moveq   #1, r19
    store   r19, (r0)
    load    (r0), r30
    nop
    nop
    nop
    nop
    nop
    nop
    movei   #G_CTRL, r0
    moveq   #0, r19
    store   r19, (r0)
    nop
    nop
    nop
    nop
    nop
    nop
gr_halt:
    movei   #gr_halt, r0
    jump    (r0)
    nop

; The variants by width - 4 (local RAM, read with a long load).
    .long
gr_nctab:
    dc.l    gr_nc4, gr_nc5, gr_nc6, gr_nc7, gr_nc8

; The costume renderer's: revBitMask, the cel's block, its palette.
    .long
gc_bits:
    dc.l    $80, $40, $20, $10, $08, $04, $02, $01
gc_blk:
    dcb.l   CEL_LONGS, 0
gc_pal:
    dcb.l   32, 0

; The colour table (a GQ_TABLES command copies it here from GQ_COLTAB).
    .long
gr_tab:
    dcb.l   256, 0
GpuServerEnd:

    .assert GQ_CMD = 32
    .assert GpuServerEnd <= (G_RAM+$1000)

    .68000
GpuRomEnd:
