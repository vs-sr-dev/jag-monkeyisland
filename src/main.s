; SPDX-License-Identifier: MIT
; Copyright (c) 2026 Samuele Voltan.  See LICENSE.
;===========================================================================
; JaguarMI - M0: boot, 320x240 progressive video, a Monkey Island room
; decoded by the GPU from the game's own compressed strips, shown by the OP.
;
; A port of The Secret of Monkey Island (Lucasfilm Games, 1990), DOS VGA
; floppy release.  The game's data come from the user's own copy, converted
; by tools/mi2jag.py (BYOA).
;
; What M0 proves, and nothing else: that the boot reaches the main loop on
; real hardware, that the room pack sits where tools/memory_map.py says, that
; the GPU decodes SCUMM's strips exactly as ScummVM does (tools/m0_ref.py
; checks the algorithm on all 81 rooms; the screen checks the GPU), and that
; the OP shows a room wider than the screen with the camera as a DATA offset:
; one SCUMM strip = 8 pixels at 8 bpp = one phrase.
;
; On screen: room 38 (the lookout), letterboxed in SCUMM's 200-line frame
; (rows 20..219).  D-pad left/right: the camera, one strip per frame while
; held.  D-pad up/down: the previous/next room with an image.  Every build is
; logged on COM8 with its room, size and cost.  Expected frames:
; docs/m0_expected_<room>.png.
;
; Every hardware sequence here is COPIED from code verified on a real Jaguar
; (JagGOT's M0 main.s, itself JagSkyroads', Prowl's and Keen Dreams'), not
; derived.
;
; ONLY 7-BIT ASCII in this file.  RMAC has NO operator precedence: brackets
; around every mixed expression (tools/check_expr.py checks it in the build).
;
; BG colour, wherever no object covers the line (the margins, and the rows
; outside the room):
;   dark green   boot did not reach the main loop
;   blue         main loop reached, the video interrupt never fired
;   slow beat    running (two dim colours, BG_PROBE=1)
;   magenta beat the GPU missed a DONE (GPU_FAIL)
;   yellow       for 2 s at boot: LockVi moved VI to the other parity
;   black        the video is off: the boot died before VideoOnMinimal
;   red          Panic (an exception, or the C blob missing or returning)
;   dark cyan    M2a: the interpreter (the C blob) runs
;
; M2a: the boot hands over to the interpreter, compiled C in
; the blob ROM_CBLOB -> DBLOB (csrc/jag/jag_main.c), which logs on COM8.  M0's
; room pack does not fit beside the game image, so M0's room code is left
; out until the GPU decodes the rooms from the game image itself.
;===========================================================================

    .include "jaguar.inc"
    .include "map.inc"

;---------------------------------------------------------------------------
; Object Processor fields (Keen Dreams, verified against TOM's netlist).
;---------------------------------------------------------------------------
LINK_HI_STOP    EQU     ((OP_STOP>>11)&$7FF)
LINK_LO_STOP    EQU     ((OP_STOP>>3)&$FF)

OBJ_TYPE_BMP    EQU     0
OBJ_TYPE_BRANCH EQU     3
OBJ_TYPE_STOP   EQU     4

; Branch conditions, from YPOS' point of view: 1 = YPOS > VC, 2 = YPOS < VC.
CC_YPOS_GT      EQU     1
CC_YPOS_LT      EQU     2

FB_DEPTH_8BPP   EQU     3
FB_INDEX        EQU     1
FB_RELEASE      EQU     1

FIELD_IWIDTH    EQU     (SCR_W/8)

;---------------------------------------------------------------------------
; Video timing: Keen Dreams' values, verified on the user's NTSC Jaguar.
; VP odd = progressive.  VDE = VDB + 2*SCR_H (the counter counts half-lines).
;---------------------------------------------------------------------------
N_HP            EQU     844
N_HBB           EQU     1697
N_HBE           EQU     118
N_HS            EQU     1743
N_HVS           EQU     595
N_HEQ           EQU     780
N_VP            EQU     523
N_VEB           EQU     512
N_VS            EQU     518
N_VEE           EQU     5
N_VBE           EQU     30
N_VBB           EQU     512
N_VDB           EQU     30

P_HP            EQU     850
P_HBB           EQU     1709
P_HBE           EQU     153
P_HS            EQU     1749
P_HVS           EQU     600
P_HEQ           EQU     787
P_VP            EQU     623
P_VEB           EQU     614
P_VS            EQU     619
P_VEE           EQU     4
P_VBE           EQU     40
P_VBB           EQU     614
P_VDB           EQU     40

VD_SPAN         EQU     (SCR_H*2)

; Half-lines per frame, for BUILD_HL: VC runs 0..VP.
NTSC_HL         EQU     (N_VP+1)
PAL_HL          EQU     (P_VP+1)

; Jaguar RGB16: r5<<11 | b5<<6 | g6 (green at the BOTTOM, blue in the middle).
COL_BOOT        EQU     $0018                   ; dark green
COL_LOOP        EQU     $07C0                   ; blue
COL_PANIC       EQU     $F800                   ; red
COL_BLACK       EQU     $0000
COL_BEAT_A      EQU     $4010                   ; dim red-green
COL_BEAT_B      EQU     $0410                   ; dim blue-green
COL_GPU_A       EQU     $FFC0                   ; magenta
COL_GPU_B       EQU     $7BC0                   ; dark magenta
COL_VIFLIP      EQU     $F83F                   ; yellow
COL_CRUN        EQU     $0208                   ; dark cyan

; BG_PROBE: the BG beats while running, so that BLACK can only mean "video
; off".  Off once M0 is closed on hardware.
BG_PROBE        EQU     1

; INT1: every latch cleared and every source off (bits 8-12).
C_ALLCLR        EQU     $1F00
INT1_ENA        EQU     C_VIDENA

; Camera: one SCUMM strip per frame while the D-pad is held.
CAM_STEP        EQU     8

; Boot stages for BOOT_MARK.
BM_START        EQU     1
BM_VIDEO        EQU     2
BM_ASSETS       EQU     3
BM_OPLIST       EQU     4
BM_LOOP         EQU     5
BM_C_BAD        EQU     6                       ; c_init did not answer C_MAGIC
BM_C_RET        EQU     7                       ; c_main returned

;===========================================================================
; THE COLD BOOT RUNS FROM THE CARTRIDGE; EVERYTHING AFTER `jmp Start` FROM DRAM.
;
; The image is assembled at DCODE but sits at ROM_CODE, where the $F6 loader
; jumps.  These instructions are position independent - absolute constants,
; relative dbra, an absolute final jmp - so they work from the wrong address.
;
; 1. quiet: on the GameDrive the previous ROM's GPU, DSP, interrupts and video
;    survive a reset (Prowl, 2026-11-19/20 on hardware);
; 2. video OFF before anything touches the counters (Prowl, hardware);
; 3. the whole DRAM cleared: on hardware it holds what the BIOS left, and a
;    cell read before it is written breaks only there (Keen Dreams, twice);
; 4. the program copied to DCODE: code running from the cartridge holds the
;    bus the OP needs (Keen Dreams, measured: 98% of the frame).
;===========================================================================
    .org    DCODE

ColdBoot:
    move.w  #$2700, sr
    move.l  #0, G_CTRL
    move.l  #0, D_CTRL
    move.w  #0, INT2
    move.w  #C_ALLCLR, INT1
    move.w  #0, VMODE

    moveq   #0, d0
    moveq   #0, d1
    moveq   #0, d2
    moveq   #0, d3
    moveq   #0, d4
    moveq   #0, d5
    moveq   #0, d6
    suba.l  a1, a1
    movea.l #$200000, a0
    move.w  #$FFFF, d7                      ; 65536 x 32 bytes = 2 MB
.cb_clr:
    movem.l d0-d6/a1, -(a0)
    dbra    d7, .cb_clr

    movea.l #ROM_CODE, a0
    movea.l #DCODE, a1
    move.w  #((DCODE_SIZE/32)-1), d7
.cb_copy:
    movem.l (a0)+, d0-d6/a2
    movem.l d0-d6/a2, (a1)
    lea     32(a1), a1
    dbra    d7, .cb_copy

; The C blob too: code, rodata and data in one image.  Its .bss is in CBSS,
; which the clear above has already zeroed (JagGOT).
    movea.l #ROM_CBLOB, a0
    movea.l #DBLOB, a1
    move.w  #((ROM_CBLOB_SIZE/32)-1), d7
.cb_blob:
    movem.l (a0)+, d0-d6/a2
    movem.l d0-d6/a2, (a1)
    lea     32(a1), a1
    dbra    d7, .cb_blob

    jmp     Start

;===========================================================================
Start:
    move.w  #$2700, sr
    move.l  #STACK_TOP, a7
    move.l  #BM_START, BOOT_MARK

    bsr     InitVectors
    bsr     GdInstall                       ; COM8 log, or nothing (src/gd.s)
    bsr     InitVideoTiming
    bsr     VideoOnMinimal
    bsr     LockVi
    move.l  #BM_VIDEO, BOOT_MARK

    bsr     InitGpu
    move.l  #G_RAM, d0                      ; the GPU serves the picture's queue
    bsr     GpuStart                        ; from now on (src/gpu_room.s)
    move.l  #BM_ASSETS, BOOT_MARK

; The C blob answers with a signature: an empty region, a stale blob or an
; entry added in the middle of the table become Panic instead of a mystery
; (JagSkyroads).
    jsr     C_ENTRY_INIT
    cmpi.l  #C_MAGIC, d0
    beq.s   .st_cok
    move.l  #BM_C_BAD, BOOT_MARK
    bra     Panic
.st_cok:

; No picture yet: VIEW_BUF = 0 keeps the screen object at HEIGHT 0 (the DRAM clear
; left every cell at 0).  The first room is built from the main loop, with
; the video interrupt served.
    move.w  #START_ROOM, CUR_ROOM
    bsr     InitOpList
    move.l  #BM_OPLIST, BOOT_MARK

; Jerry before the video interrupt, as Keen Dreams, JagSkyroads and JagGOT
; (verified on hardware): the DSP waits on the AdLib driver's empty ring,
; which the video interrupt fills from the first frame (c_audio).
    bsr     InitDspAudio

    lea     MsgBoot, a0
    bsr     LogStr

; The mask is $2000, NOT $2100, and INT2 is written before INT1: the sequence
; Jaguar Chess invented ran on BigPEmu and died on hardware.
    move.w  #0, INT2
    move.w  #INT1_ENA|C_VIDCLR, INT1
    move.w  #$2000, sr

    move.l  #BM_LOOP, BOOT_MARK
    move.w  #COL_LOOP, BG

; M2a: the interpreter takes the 68000 for good (its rounds wait on IRQ_TICK).
    move.w  #COL_CRUN, BG
    jsr     C_ENTRY_MAIN
    move.l  #BM_C_RET, BOOT_MARK
    bra     Panic

;---------------------------------------------------------------------------
; GpuStart: d0 = entry.  JagLBA's Gpu_Run without the wait (as JagSkyroads'
; gpu_run), verified on hardware: DONE cleared and READ BACK, the GPU stopped
; and checked stopped before G_PC is written (a G_PC written while it runs is
; lost).  Once at boot: the GPU then serves the queue until a GQ_STOP.
;---------------------------------------------------------------------------
GpuStart:
    move.l  #0, GPU_DONE
    tst.l   GPU_DONE
    move.l  #0, G_CTRL
    tst.l   G_CTRL
    move.l  #0, G_FLAGS
    move.l  #$00070007, G_END
    move.l  d0, G_PC
    tst.l   G_PC
    move.l  #1, G_CTRL
    rts

;---------------------------------------------------------------------------
; TimeNow: d0 = half-lines since boot.  IRQ_TICK counts video interrupts,
; which fire at VC = VI, not at VC = 0: the phase is therefore VC - VI
; (wrapped into the frame), so that it restarts from 0 exactly when
; IRQ_TICK moves.  IRQ_TICK read on both sides of VC: if it moved, again.
;---------------------------------------------------------------------------
TimeNow:
    movem.l d1-d3, -(sp)
    move.w  #NTSC_HL, d1
    tst.w   VID_NTSC
    bne.s   .tn_again
    move.w  #PAL_HL, d1
.tn_again:
    move.l  IRQ_TICK, d0
    move.w  VC, d2
    move.l  IRQ_TICK, d3
    cmp.l   d0, d3
    bne.s   .tn_again
    andi.w  #$7FF, d2
    sub.w   VI_VAL, d2
    bpl.s   .tn_pos
    add.w   d1, d2
.tn_pos:
    mulu.w  d1, d0
    andi.l  #$FFFF, d2
    add.l   d2, d0
    movem.l (sp)+, d1-d3
    rts

;---------------------------------------------------------------------------
; BgProbe: the BG beats every 32 frames.  Magenta if the GPU ever missed a
; DONE; yellow for 2 s if LockVi moved VI.
;---------------------------------------------------------------------------
BgProbe:
    movem.l d0-d2, -(sp)
    tst.w   VI_FLIP
    beq.s   .bp_run
    cmpi.l  #120, IRQ_TICK
    bhs.s   .bp_run
    move.w  #COL_VIFLIP, BG
    bra.s   .bp_out
.bp_run:
    move.w  #COL_BEAT_A, d0
    move.w  #COL_BEAT_B, d1
    move.l  GPU_FAIL, d2
    or.l    GPU_STALL, d2
    beq.s   .bp_pick
    move.w  #COL_GPU_A, d0
    move.w  #COL_GPU_B, d1
.bp_pick:
    move.l  IRQ_TICK, d2
    btst    #5, d2
    beq.s   .bp_set
    move.w  d1, d0
.bp_set:
    move.w  d0, BG
.bp_out:
    movem.l (sp)+, d0-d2
    rts

;---------------------------------------------------------------------------
; ReadPad (Keen Dreams' main.s, verified on hardware): the controller is a
; matrix of four strobed rows.  PAD_STATE gets the game buttons (PAD_B_*),
; PAD_KEYS the keypad (KEY_B_*).
;---------------------------------------------------------------------------
JOY_ROW0        EQU     $F1FE
JOY_ROW1        EQU     $81FD
JOY_ROW2        EQU     $81FB
JOY_ROW3        EQU     $81F7

ReadPad:
    movem.l d0-d4, -(sp)
    moveq   #0, d3
    moveq   #0, d4

    move.w  #JOY_ROW0, d1
    bsr     .rp_row
    move.l  d0, d2
    andi.l  #$0F, d2                        ; U D L R, already in order
    or.l    d2, d3
    btst    #5, d0
    beq.s   .rp_noa
    bset    #PAD_B_A, d3
.rp_noa:
    btst    #4, d0
    beq.s   .rp_nopause
    bset    #PAD_B_PAUSE, d3
.rp_nopause:

    move.w  #JOY_ROW1, d1
    bsr     .rp_row
    btst    #5, d0
    beq.s   .rp_nob
    bset    #PAD_B_B, d3
.rp_nob:
    moveq   #KEY_B_1, d2
    bsr     .rp_k3
    moveq   #KEY_B_4, d2
    bsr     .rp_k2
    moveq   #KEY_B_7, d2
    bsr     .rp_k1
    moveq   #KEY_B_STAR, d2
    bsr     .rp_k0

    move.w  #JOY_ROW2, d1
    bsr     .rp_row
    btst    #5, d0
    beq.s   .rp_noc
    bset    #PAD_B_C, d3
.rp_noc:
    moveq   #KEY_B_2, d2
    bsr     .rp_k3
    moveq   #KEY_B_5, d2
    bsr     .rp_k2
    moveq   #KEY_B_8, d2
    bsr     .rp_k1
    moveq   #KEY_B_0, d2
    bsr     .rp_k0

    move.w  #JOY_ROW3, d1
    bsr     .rp_row
    btst    #5, d0
    beq.s   .rp_noopt
    bset    #PAD_B_OPT, d3
.rp_noopt:
    moveq   #KEY_B_3, d2
    bsr     .rp_k3
    moveq   #KEY_B_6, d2
    bsr     .rp_k2
    moveq   #KEY_B_9, d2
    bsr     .rp_k1
    moveq   #KEY_B_HASH, d2
    bsr     .rp_k0

    move.w  d3, PAD_STATE
    move.w  d4, PAD_KEYS
    movem.l (sp)+, d0-d4
    rts

.rp_k3:
    btst    #3, d0
    beq.s   .rp_kdone
    bset    d2, d4
    rts
.rp_k2:
    btst    #2, d0
    beq.s   .rp_kdone
    bset    d2, d4
    rts
.rp_k1:
    btst    #1, d0
    beq.s   .rp_kdone
    bset    d2, d4
    rts
.rp_k0:
    btst    #0, d0
    beq.s   .rp_kdone
    bset    d2, d4
.rp_kdone:
    rts

; d1 = strobe -> d0 = bits 0-3 the row's four keys, bits 4-5 JOYBUTS' two
; buttons, active HIGH.  One LONG read takes JOYSTICK and JOYBUTS at the same
; instant (JagLBA, hardware).
.rp_row:
    move.w  d1, JOYSTICK
    moveq   #7, d2
.rp_settle:
    dbra    d2, .rp_settle
    move.l  JOYSTICK, d0
    not.l   d0
    move.l  d0, d2
    andi.l  #3, d2
    lsl.l   #4, d2
    lsr.l   #8, d0
    lsr.l   #8, d0
    lsr.l   #8, d0
    andi.l  #$0F, d0
    or.l    d2, d0
    rts

;---------------------------------------------------------------------------
; InitVectors: all 256 to Panic, then $100 (the Jaguar's only interrupt
; vector) to the video handler.
;---------------------------------------------------------------------------
InitVectors:
    movea.l #VECTORS, a0
    move.w  #255, d0
    move.l  #Panic, d1
.iv_loop:
    move.l  d1, (a0)+
    dbra    d0, .iv_loop
    move.l  #ViHandler, $100
    rts

Panic:
    move.w  #$2700, sr
.pa_loop:
    move.w  #COL_PANIC, BG
    bra.s   .pa_loop

;---------------------------------------------------------------------------
; InitVideoTiming: NTSC/PAL detection and 240p timing (Keen Dreams).
; VI must be ODD in progressive (TOM vid.v:3412, vintd = vieq & hdeeq, and
; hc[10] == VC&1 once HC and VC start together from zero).
;---------------------------------------------------------------------------
InitVideoTiming:
    move.w  #0, VMODE
    move.w  #0, HC
    move.w  #0, VC

    move.w  BASE+$14002, d0
    and.w   #$10, d0
    beq     .ivt_pal
    move.w  #1, VID_NTSC
    move.w  #823, d2
    move.w  #1409, d0
    move.w  #N_HP, HP
    move.w  #N_HBB, HBB
    move.w  #N_HBE, HBE
    move.w  #N_HS, HS
    move.w  #N_HVS, HVS
    move.w  #N_HEQ, HEQ
    move.w  #N_VP, VP
    move.w  #N_VEB, VEB
    move.w  #N_VS, VS
    move.w  #N_VEE, VEE
    move.w  #N_VBE, VBE
    move.w  #N_VBB, VBB
    move.w  #N_VDB, d5
    bra     .ivt_common
.ivt_pal:
    move.w  #0, VID_NTSC
    move.w  #843, d2
    move.w  #1381, d0
    move.w  #P_HP, HP
    move.w  #P_HBB, HBB
    move.w  #P_HBE, HBE
    move.w  #P_HS, HS
    move.w  #P_HVS, HVS
    move.w  #P_HEQ, HEQ
    move.w  #P_VP, VP
    move.w  #P_VEB, VEB
    move.w  #P_VS, VS
    move.w  #P_VEE, VEE
    move.w  #P_VBE, VBE
    move.w  #P_VBB, VBB
    move.w  #P_VDB, d5
.ivt_common:
    move.w  d0, d1
    asr     #1, d1
    sub.w   d1, d2
    add.w   #4, d2
    sub.w   #1, d1
    or.w    #$400, d1
    move.w  d1, HDE
    move.w  d2, HDB1
    move.w  d2, HDB2

    move.w  d5, VDB
    move.w  d5, VDB_VAL
    move.w  d5, d6
    add.w   #VD_SPAN, d6
    move.w  d6, VDE
    move.w  d6, VDE_VAL

    move.w  d6, d0
    addq.w  #1, d0
    ori.w   #1, d0
    move.w  d0, VI
    move.w  d0, VI_VAL
    move.l  #0, BORD1
    rts

;---------------------------------------------------------------------------
; VideoOnMinimal: a list of one STOP, video on: the screen shows BG only.
; PWIDTH4 = 320 columns; RGB16 because the CLUT entries are RGB16.
;---------------------------------------------------------------------------
VideoOnMinimal:
    move.l  #0, OP_STOP
    move.l  #OBJ_TYPE_STOP, OP_STOP+4
    move.l  #OP_STOP, d0
    swap    d0
    move.l  d0, OLP
; One write to OBF: an OP left waiting on a GPU object by the previous ROM
; stays stopped through the GameDrive's reboot, and only this frees it
; (JaguarDocs hw/04 rule 13, hardware).
    move.w  #0, OBF
    move.w  #COL_BOOT, BG
    move.w  #VIDEN|BGEN|PWIDTH4|RGB16|CSYNC, VMODE
    rts

;---------------------------------------------------------------------------
; LockVi: VI on the parity the counters ACTUALLY came up with (JagSkyroads,
; hardware: HC/VC loads can be missed depending on where the code sits, and
; the video interrupt then never fires).  With the video running: read VC,
; HC, VC (equal VCs = a coherent pair), and if hc[10] != VC & 1, put VI on
; the EVEN half-line after VDE instead, and say so in VI_FLIP.
;---------------------------------------------------------------------------
LockVi:
    movem.l d0-d2, -(sp)
.lv_read:
    move.w  VC, d0
    move.w  HC, d1
    move.w  VC, d2
    cmp.w   d0, d2
    bne.s   .lv_read
    lsr.w   #8, d1
    lsr.w   #2, d1                          ; hc[10]
    eor.w   d2, d1
    andi.w  #1, d1                          ; 1: hc[10] != VC & 1
    move.w  d1, VI_FLIP
    beq.s   .lv_done
    move.w  VDE_VAL, d0
    addq.w  #1, d0
    ori.w   #1, d0                          ; the odd VI InitVideoTiming chose
    addq.w  #1, d0                          ; the next, even, half-line
    move.w  d0, VI
    move.w  d0, VI_VAL
.lv_done:
    movem.l (sp)+, d0-d2
    rts

;---------------------------------------------------------------------------
; InitOpList:
;   OP_BR_VDE  if YPOS < VC -> STOP  (below the window)
;   OP_BR_VDB  if YPOS > VC -> STOP  (above)
;   the objects, in OBJ_ order, each linking to the next; the last to STOP.
; The truth lives in OBJ_SHADOW; ArmObjects copies it into the list, and the
; video handler does it again every frame because the OP consumes HEIGHT
; and DATA as it draws.
;---------------------------------------------------------------------------
InitOpList:
    movea.l #OP_LIST, a0
    move.w  #((OP_LIST_SIZE/8)-1), d0
.iol_stop:
    move.l  #0, (a0)+
    move.l  #OBJ_TYPE_STOP, (a0)+
    dbra    d0, .iol_stop

    move.l  #LINK_HI_STOP, OP_BR_VDE
    moveq   #0, d0
    move.w  VDE_VAL, d0
    lsl.l   #3, d0
    or.l    #((LINK_LO_STOP<<24)|(CC_YPOS_LT<<14)|OBJ_TYPE_BRANCH), d0
    move.l  d0, OP_BR_VDE+4

    move.l  #LINK_HI_STOP, OP_BR_VDB
    moveq   #0, d0
    move.w  VDB_VAL, d0
    lsl.l   #3, d0
    or.l    #((LINK_LO_STOP<<24)|(CC_YPOS_GT<<14)|OBJ_TYPE_BRANCH), d0
    move.l  d0, OP_BR_VDB+4

    bsr     SetView
    bsr     ArmObjects
    move.l  #OP_BR_VDE, d0
    swap    d0
    move.l  d0, OLP
    rts

;---------------------------------------------------------------------------
; SetView: called once at boot, then by ViHandler every frame.
; 1. A picture the main loop has handed over (NEXT_SEQ moved), once the GPU
;    has done every command written before it (GQ_TAIL has reached
;    NEXT_FENCE; both 16-bit counts, compared in their low words): its CLUT
;    first, then VIEW_BUF (JaguarDocs hw/04 rule 11), all in this vertical
;    blank.  Until then the picture before it stays.  VIEW_BUF and VIEW_SEQ
;    are this routine's own cells.
; 2. The screen object from VIEW_BUF: a system screen (SCREEN0 or SCREEN1,
;    csrc/jag/jag_gfx.c), 320 x 200, pitch 320.  None yet (VIEW_BUF = 0):
;    HEIGHT 0.  3. The cursor (below).
;---------------------------------------------------------------------------
SetView:
    movem.l d0-d7/a0-a1, -(sp)
    move.l  NEXT_SEQ, d6
    cmp.l   VIEW_SEQ, d6
    beq.s   .sv_obj
    move.w  GQ_TAIL+2, d0                   ; the GPU's done count (low word)
    sub.w   NEXT_FENCE+2, d0
    bmi.s   .sv_obj                         ; not drawn yet: the next frame
    movea.l NEXT_CLUT, a0
    movea.l #CLUT, a1
    move.w  #((256/2)-1), d0
.sv_clut:
    move.l  (a0)+, (a1)+
    dbra    d0, .sv_clut
    move.l  NEXT_BUF, VIEW_BUF
    move.l  NEXT_CUR, VIEW_CUR
    move.l  NEXT_CURHX, VIEW_CURHX
    move.l  NEXT_CURHY, VIEW_CURHY
    move.l  d6, VIEW_SEQ
.sv_obj:
    lea     OBJ_SHADOW+(OBJ_ROOM*OBJ_STRIDE), a0
    move.l  VIEW_BUF, d0
    move.l  #200, d2
    tst.l   d0
    bne.s   .sv_have
    moveq   #0, d2
    bra.s   .sv_bg
.sv_have:
    move.w  #COL_BLACK, BG                  ; a picture: the border black, as the game's
.sv_bg:
    moveq   #0, d1
    move.w  VDB_VAL, d1
    add.l   #(FRAME_Y*2), d1
    moveq   #0, d3                          ; XPOS
    move.l  #FIELD_IWIDTH, d4
    move.l  #(SCR_W/8), d5                  ; DWIDTH: 320 pixels, in phrases
    moveq   #0, d7
    bsr     MakeObj
; 3. The cursor over it (ScummVM's backend draws it over the screen where the
;    mouse is, at once, clipped to the screen): VIEW_CUR, 16 x 16 at 8 bpp,
;    0 transparent (TRANS), at the mouse minus the hotspot.  The OP does not
;    clip at the top (JaguarDocs hw/04 rule 3): rows above the screen are
;    skipped by moving DATA on; rows below it are cut from HEIGHT; left the
;    OP clips, right EDGE covers it.  None: HEIGHT 0.
    lea     OBJ_SHADOW+(OBJ_CURSOR*OBJ_STRIDE), a0
    move.l  VIEW_CUR, d0
    move.l  MOUSE_Y, d6
    sub.l   VIEW_CURHY, d6                  ; the top row on the screen
    moveq   #0, d2
    tst.l   d0
    beq.s   .sv_cput                        ; not shown
    moveq   #16, d2
    tst.l   d6
    bpl.s   .sv_cbot
    add.l   d6, d2                          ; above the screen: 16 + y rows left
    move.l  d6, d1
    neg.l   d1
    lsl.l   #4, d1
    add.l   d1, d0                          ; DATA past the rows cut (16 bytes each)
    moveq   #0, d6
.sv_cbot:
    move.l  #200, d1
    sub.l   d6, d1                          ; the screen's rows from y down
    cmp.l   d1, d2
    ble.s   .sv_cpos
    move.l  d1, d2
.sv_cpos:
    tst.l   d2
    bpl.s   .sv_cput
    moveq   #0, d2
.sv_cput:
    moveq   #0, d1
    move.w  VDB_VAL, d1
    add.l   #(FRAME_Y*2), d1
    add.l   d6, d1
    add.l   d6, d1                          ; YPOS: two half-lines a row
    move.l  MOUSE_X, d3
    sub.l   VIEW_CURHX, d3
    moveq   #2, d4                          ; IWIDTH 16 pixels
    moveq   #2, d5                          ; DWIDTH
    move.l  #$8000, d7                      ; TRANS (bit 47)
    bsr     MakeObj
; 4. EDGE: black right of the screen, over the cursor (the NTSC display is 352
;    pixels wide, ScummVM clips the cursor at 320): 32 x 200 at 16 bpp, $0000
;    opaque (no TRANS), with the screen.
    lea     OBJ_SHADOW+(OBJ_EDGE*OBJ_STRIDE), a0
    move.l  #EDGE_BUF, d0
    move.l  #200, d2
    tst.l   VIEW_BUF
    bne.s   .sv_edge
    moveq   #0, d2
.sv_edge:
    moveq   #0, d1
    move.w  VDB_VAL, d1
    add.l   #(FRAME_Y*2), d1
    move.l  #SCR_W, d3                      ; XPOS 320
    moveq   #8, d4                          ; IWIDTH 32 pixels at 16 bpp
    moveq   #8, d5
    moveq   #0, d7
    bsr     MakeObj
    andi.l  #$FFFF8FFF, 12(a0)              ; DEPTH: 16 bpp (4), not MakeObj's 8
    ori.l   #$00004000, 12(a0)
    movem.l (sp)+, d0-d7/a0-a1
    rts

;---------------------------------------------------------------------------
; Spies: before the list is re-armed.  EDGE is the list's last object and as
; tall as the screen: the HEIGHT the OP left in it is the rows on which the
; OP did not reach the end of the list (JaguarDocs, Keen Dreams' spy).  And
; how late this interrupt started after VI, in half-lines.
;---------------------------------------------------------------------------
Spies:
    movem.l d0-d1, -(sp)
    move.w  VC, d0
    andi.w  #$7FF, d0
    sub.w   VI_VAL, d0
    bpl.s   .sp_vc
    add.w   #NTSC_HL, d0                    ; past the frame's end: VC wrapped
    tst.w   VID_NTSC
    bne.s   .sp_vc
    add.w   #(PAL_HL-NTSC_HL), d0
.sp_vc:
    cmp.w   ISR_LATE, d0
    bls.s   .sp_op
    move.w  d0, ISR_LATE
.sp_op:
    move.l  OP_OBJS+(OBJ_EDGE*OBJ_STRIDE)+4, d0
    lsr.l   #8, d0
    lsr.l   #6, d0
    andi.l  #$3FF, d0                       ; HEIGHT: bits 14-23 of phrase 0
    add.l   d0, OP_MISS
    cmp.w   OP_MISSMAX, d0
    bls.s   .sp_out
    move.w  d0, OP_MISSMAX
.sp_out:
    movem.l (sp)+, d0-d1
    rts

; SpyEnd: how late after VI the list was re-armed (ISR_END, the most).
SpyEnd:
    move.l  d0, -(sp)
    move.w  VC, d0
    andi.w  #$7FF, d0
    sub.w   VI_VAL, d0
    bpl.s   .se_vc
    add.w   #NTSC_HL, d0                    ; past the frame's end: VC wrapped
    tst.w   VID_NTSC
    bne.s   .se_vc
    add.w   #(PAL_HL-NTSC_HL), d0
.se_vc:
    cmp.w   ISR_END, d0
    bls.s   .se_out
    move.w  d0, ISR_END
.se_out:
    move.l  (sp)+, d0
    rts

;---------------------------------------------------------------------------
; MovePad (PLAY): the pad moves the mouse, as a mouse would move ScummVM's.
; Every frame: ReadPad, the D-pad moves MOUSE_X/Y (1 pixel a frame, faster
; the longer it is held: 1 + frames / 8, at most 4), clamped to the screen;
; A, B, C and OPTION pressed (an edge) count into PAD_ADOWN .. PAD_ODOWN, so
; that the main loop sees a press even if it was released before the round;
; each keypad key pressed goes into the queue KEYQ (KEYQ_HEAD counts them).
;---------------------------------------------------------------------------
MovePad:
    movem.l d0-d4/a0, -(sp)
    move.w  PAD_STATE, d3                   ; the frame before
    move.w  PAD_KEYS, d4                    ; the keypad, the frame before
    bsr     ReadPad
    move.w  PAD_STATE, d0
    move.w  d0, d1
    andi.w  #$000F, d1                      ; U D L R
    beq     .mp_still
    moveq   #0, d2
    move.w  PAD_HELD, d2
    addq.w  #1, PAD_HELD
    lsr.w   #3, d2
    addq.w  #1, d2
    cmpi.w  #4, d2
    bls.s   .mp_speed
    moveq   #4, d2
.mp_speed:
    btst    #PAD_B_UP, d0
    beq.s   .mp_nu
    sub.l   d2, MOUSE_Y
.mp_nu:
    btst    #PAD_B_DOWN, d0
    beq.s   .mp_nd
    add.l   d2, MOUSE_Y
.mp_nd:
    btst    #PAD_B_LEFT, d0
    beq.s   .mp_nl
    sub.l   d2, MOUSE_X
.mp_nl:
    btst    #PAD_B_RIGHT, d0
    beq.s   .mp_clamp
    add.l   d2, MOUSE_X
.mp_clamp:
    tst.l   MOUSE_X
    bpl.s   .mp_x0
    clr.l   MOUSE_X
.mp_x0:
    cmpi.l  #(SCR_W-1), MOUSE_X
    ble.s   .mp_x1
    move.l  #(SCR_W-1), MOUSE_X
.mp_x1:
    tst.l   MOUSE_Y
    bpl.s   .mp_y0
    clr.l   MOUSE_Y
.mp_y0:
    cmpi.l  #199, MOUSE_Y
    ble.s   .mp_buttons
    move.l  #199, MOUSE_Y
    bra.s   .mp_buttons
.mp_still:
    clr.w   PAD_HELD
.mp_buttons:
    btst    #PAD_B_A, d0                    ; A pressed now, not before: a press
    beq.s   .mp_na
    btst    #PAD_B_A, d3
    bne.s   .mp_na
    addq.l  #1, PAD_ADOWN
.mp_na:
    btst    #PAD_B_B, d0
    beq.s   .mp_nb
    btst    #PAD_B_B, d3
    bne.s   .mp_nb
    addq.l  #1, PAD_BDOWN
.mp_nb:
    btst    #PAD_B_C, d0
    beq.s   .mp_nc
    btst    #PAD_B_C, d3
    bne.s   .mp_nc
    addq.l  #1, PAD_CDOWN
.mp_nc:
    btst    #PAD_B_OPT, d0
    beq.s   .mp_no
    btst    #PAD_B_OPT, d3
    bne.s   .mp_no
    addq.l  #1, PAD_ODOWN
.mp_no:
    move.w  PAD_KEYS, d0
    not.w   d4
    and.w   d0, d4                          ; the keys down now and not before
    beq.s   .mp_nokey
    lea     KEYQ, a0
    moveq   #0, d0                          ; KEY_B_0 .. KEY_B_HASH
.mp_key:
    btst    d0, d4
    beq.s   .mp_knext
    move.l  KEYQ_HEAD, d1
    andi.w  #15, d1
    move.b  d0, 0(a0,d1.w)
    addq.l  #1, KEYQ_HEAD
.mp_knext:
    addq.w  #1, d0
    cmpi.w  #12, d0
    blo.s   .mp_key
.mp_nokey:
    movem.l (sp)+, d0-d4/a0
    rts

;---------------------------------------------------------------------------
; ArmObjects: OBJ_SHADOW -> OP_OBJS, 32 bytes per object, no arithmetic.
;---------------------------------------------------------------------------
ArmObjects:
    movem.l d0-d7/a0-a2, -(sp)
    lea     OBJ_SHADOW, a0
    lea     OP_OBJS, a1
    move.w  #(OBJ_N-1), d0
.ao_loop:
    movem.l (a0)+, d1-d7/a2
    movem.l d1-d7/a2, (a1)
    lea     OBJ_STRIDE(a1), a1
    dbra    d0, .ao_loop
    movem.l (sp)+, d0-d7/a0-a2
    rts

;---------------------------------------------------------------------------
; MakeObj (Keen Dreams): a bitmap object into the shadow cell at a0.
;   d0 data   d1 YPOS (half-lines)   d2 HEIGHT   d3 XPOS
;   d4 IWIDTH (phrases)   d5 DWIDTH (phrases)   d7 extra high-phrase-1 bits
; The LINK is the next cell, always: the last object's next cell is OP_STOP.
;---------------------------------------------------------------------------
MakeObj:
    movem.l d0-d7, -(sp)
    move.l  d7, 8(a0)
    move.l  a0, d7
    sub.l   #OBJ_SHADOW, d7
    add.l   #(OP_OBJS+OBJ_STRIDE), d7
    lsr.l   #3, d7

    lsr.l   #3, d0
    lsl.l   #8, d0
    lsl.l   #3, d0
    move.l  d7, d6
    lsr.l   #8, d6
    andi.l  #$000007FF, d6
    or.l    d6, d0
    move.l  d0, (a0)

    move.l  d7, d0
    andi.l  #$000000FF, d0
    swap    d0
    lsl.l   #8, d0
    move.l  d2, d6
    lsl.l   #8, d6
    lsl.l   #6, d6
    or.l    d6, d0
    move.l  d1, d6
    lsl.l   #3, d6
    or.l    d6, d0
    or.l    #OBJ_TYPE_BMP, d0
    move.l  d0, 4(a0)

    move.l  d4, d0
    lsr.l   #4, d0
    or.l    #((FB_INDEX<<6)|(FB_RELEASE<<16)), d0
    or.l    8(a0), d0
    move.l  d0, 8(a0)

    move.l  d3, d0
    andi.l  #$00000FFF, d0
    or.l    #((FB_DEPTH_8BPP<<12)|(1<<15)), d0
    move.l  d5, d6
    lsl.l   #8, d6
    lsl.l   #8, d6
    lsl.l   #2, d6
    or.l    d6, d0
    move.l  d4, d6
    andi.l  #$0000000F, d6
    swap    d6
    lsl.l   #8, d6
    lsl.l   #4, d6
    or.l    d6, d0
    move.l  d0, 12(a0)
    movem.l (sp)+, d0-d7
    rts

;---------------------------------------------------------------------------
; ViHandler: acknowledge (C_VIDCLR, then INT2), count, re-arm the list.
; The acknowledge comes FIRST: while an interrupt is applied and INT2 is not
; written the 68000 sits above the GPU and the Blitter on the bus (JaguarDocs
; hw/01 rule 7, hw/06 rule 15).  SetView runs every frame (the camera), and
; updates the shadow BEFORE it is copied, so a change takes effect on this
; very field.
;---------------------------------------------------------------------------
ViHandler:
    move.w  #INT1_ENA|C_VIDCLR, INT1
    move.w  #0, INT2
    move.l  #1, ISR_BUSY                    ; the GPU starts no blit until the list is armed
    addq.l  #1, IRQ_TICK
    bsr     Spies                           ; EDGE's HEIGHT: before the list is re-armed
    bsr     SetView
    bsr     ArmObjects
    move.l  #0, ISR_BUSY
    bsr     SpyEnd
    .if PLAY
    bsr     MovePad                         ; the mouse for the next frame's cursor
    .endif
    bsr     CAudio                          ; the AdLib driver, AD_LEAD ticks ahead
    rte

;---------------------------------------------------------------------------
; CAudio: the AdLib driver's frame (csrc/jag/jag_audio.c c_audio), after the
; list is armed.  gcc's C may change d0-d1/a0-a1; the rest it keeps.
;---------------------------------------------------------------------------
CAudio:
    movem.l d0-d1/a0-a1, -(sp)
    jsr     C_ENTRY_AUDIO
    movem.l (sp)+, d0-d1/a0-a1
    rts

;---------------------------------------------------------------------------
; The COM8 log (src/gd.s GdSendC; nothing is sent without a GameDrive).
; LogStr: a0 = a NUL-terminated line.  PutStr: a0 -> a1, a1 left on the NUL.
; PutDec: d0 unsigned -> decimal at a1.
;---------------------------------------------------------------------------
LogStr:
    movem.l d0-d1/a0-a1, -(sp)
    move.l  GD_SEND_FN, d0
    beq.s   .ls_none
    movea.l d0, a1
    move.l  a0, -(sp)
    jsr     (a1)
    addq.l  #4, sp
.ls_none:
    movem.l (sp)+, d0-d1/a0-a1
    rts

PutStr:
    move.b  (a0)+, (a1)+
    bne.s   PutStr
    subq.l  #1, a1
    rts

; A 32-bit value divided by 10 in two DIVUs (each 32/16 -> 16): the high
; word first, then its remainder with the low word, which cannot overflow.
PutDec:
    movem.l d0-d3/a0, -(sp)
    lea     -12(sp), sp
    movea.l sp, a0                          ; the digits, least significant first
    moveq   #0, d3
.pd_div:
    move.l  d0, d1
    clr.w   d1
    swap    d1                              ; d1 = high word
    divu.w  #10, d1                         ; remH << 16 | qH
    move.w  d1, d2
    swap    d2                              ; d2 = qH << 16
    move.w  d0, d1                          ; d1 = remH << 16 | low word
    divu.w  #10, d1                         ; rem << 16 | qL
    move.w  d1, d2                          ; d2 = the quotient
    swap    d1
    addi.b  #$30, d1                        ; '0'
    move.b  d1, (a0)+
    addq.w  #1, d3
    move.l  d2, d0
    bne.s   .pd_div
.pd_out:
    move.b  -(a0), (a1)+
    subq.w  #1, d3
    bne.s   .pd_out
    lea     12(sp), sp
    movem.l (sp)+, d0-d3/a0
    rts

MsgBoot:
    dc.b    'JaguarMI M0: boot ok',0
MsgRoom1:
    dc.b    'M0 room ',0
MsgRoom2:
    dc.b    ' decoded in ',0
MsgRoom3:
    dc.b    ' half-lines, gpu fails ',0
    .even

    .include "src/gd.s"

; The GPU's code: its own section and .org, so it goes LAST.
    .include "src/gpu_room.s"
    .include "src/dsp_audio.s"

CodeEnd:
    .end
