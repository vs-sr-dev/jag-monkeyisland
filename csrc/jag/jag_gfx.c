/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * jag_gfx.c - ScummVM's screens on the Jaguar: the picture half of the platform.
 *
 * The model is ScummVM's own (gfx.cpp, and csrc/ref/gfx_ref.c, the PC's transcription that
 * equals ScummVM's screen pixel for pixel): virtual screens (the main one with a back buffer,
 * the text and the verb screens), each with its dirty strips; drawDirtyScreenParts copies the
 * dirty parts into the system screen, which is what the player sees.  The bookkeeping (the
 * dirty strips, their merging, the clipping, the effects' order) is gfx_ref.c's, line for
 * line, so that the same rectangles move at the same moments; the pixels move differently:
 *
 *   - the strips: the GPU decodes them into BACKBUF (GQ_STRIP), the z-planes into MASKBUF
 *     (GQ_MASK);
 *   - every copy and fill between the screens (copy8Col, resetBackground, restoreBackground,
 *     drawBox, the screen update) is a Blitter blit the GPU programs (GQ_BLIT), queued in
 *     order with the strips: the 68000 never touches a pixel and never waits for one;
 *   - the system screen is double (SCREEN0/1): the OP shows one, the copies go into the other;
 *     the hand-over (plat_drawDirtyScreenParts) gives it to the video interrupt with the CLUT,
 *     and before the next copies the rectangles it showed are copied into the other one
 *     (screen to screen), so both hold what ScummVM's single screen holds.
 *
 * The OP shows the system screen, not the main virtual screen: the actors are restored and
 * redrawn in the front buffer during a round, and nothing half done may be on screen.  The
 * costumes (GQ_CEL) and the glyphs (GQ_CHAR) are the GPU's too; the text surface (the glyphs
 * over the room, CHARSET_MASK_TRANSPARENCY where there are none) is composited over every
 * rectangle copied into the system screen.  The cursor is an OP sprite over the screen.  The checks (the trace phase, CHECKS bits, tools/pal_crc.py): the palette, the back
 * buffer's window, the z-planes, the front buffer's window, the system screen.
 */
#include <string.h>
#include "jag.h"

#define NUM_STRIPS 40                       /* Gdi::_numStrips = screen width / 8 (v < 7) */
#define CHARSET_MASK_TRANSPARENCY 0xFD      /* the text surface: no text here */
enum { kMainVirtScreen = 0, kTextVirtScreen = 1, kVerbVirtScreen = 2, kBannerVirtScreen = 3 };

/* the Blitter: commands and flags taken verbatim from code verified on hardware (JaguarDocs
 * hw/05 rule 9): the copy is JagLBA's/KD's/JagGOT's, the fill JagLBA's B_CMD_FILL */
#define B_COPY    0x01800601UL              /* SRCEN | UPDA1 | UPDA2 | LFU source */
#define B_FILL    0x01810200UL              /* PATDSEL | UPDA1 | LFU replace */
#define F_PIXEL8  0x00000018UL
#define F_XADDPIX 0x00010000UL              /* pixel mode; 0 is phrase mode */
#define B_TEXT    0x09800609UL              /* the copy + DSTEN + DCOMPEN: B_PATD not written
                                               (JagGOT, phrase mode; KD, pixel mode) */

static inline uint32_t mul16(uint32_t a, uint32_t b)     /* a 68000 mulu.w, no libgcc call */
{
    return (uint32_t)(uint16_t)a * (uint16_t)b;
}

/* ---- the virtual screens ------------------------------------------------------------- */
typedef struct {
    int number, topline, w, h, pitch, hasTwoBuffers, scrollable;
    uint32_t pixels;                         /* its buffer in DRAM */
    int tdirty[NUM_STRIPS], bdirty[NUM_STRIPS];
} VirtScreen;

static VirtScreen virtscr[4];

/* the back buffer: the room's pitch, unwrapped (ScummVM's is the 320-pitch wrapped buffer;
 * the same pixel of the room either way) */
static uint32_t roomW, roomH, roomP, roomFlags;
static uint32_t maskItem, maskUsed;         /* the z-plane buffer: one plane, all in use */
static uint32_t flags320;                   /* WIDTH 320, from blit_pitch (gfx_start) */
static uint32_t pitch_flags(uint32_t pitch);
static volatile uint32_t *slot(void);       /* gq_slot, the screen list flushed first */

/* The narrowest width >= w the Blitter's WIDTH field describes, (4 + m) << (e - 2), and the
 * field (e << 2 | m) (TRM p.66, JaguarDocs hw/05; tools/memory_map.py blit_width, which sizes
 * the buffers by the same rule). */
static uint32_t blit_pitch(uint32_t w, uint32_t *field)
{
    uint32_t e, m, v;
    for (e = 2; e < 12; e++)
        for (m = 0; m < 4; m++) {
            v = (4 + m) << (e - 2);
            if (v >= w) {
                *field = (e << 2) | m;
                return v;
            }
        }
    return 0;
}

static uint32_t pitch_flags(uint32_t pitch)
{
    uint32_t field = 0;
    if (blit_pitch(pitch, &field) != pitch)
        plat_error("blit: a pitch the Blitter cannot describe");
    return F_PIXEL8 | (field << 9);
}

/* a / b, the quotient under 65536: one divu.w (gcc's own would call libgcc's __udivsi3) */
static inline uint32_t div16(uint32_t a, uint32_t b)
{
    __asm__("divu.w %1,%0" : "+d"(a) : "d"((uint16_t)b) : "cc");
    return a & 0xFFFF;
}

/* the blits of the time phase's report: fills, background copies, screen updates, syncs */
static uint32_t blitStat[5][2], nReset, nStrip;

void gfx_report(uint32_t n)
{
    jag_logf("blits/round: fill %u (%u B), bg %u (%u B), update %u (%u B), sync %u (%u B), pixel mode %u",
             (unsigned)(blitStat[0][0] / n), (unsigned)(blitStat[0][1] / n),
             (unsigned)(blitStat[1][0] / n), (unsigned)(blitStat[1][1] / n),
             (unsigned)(blitStat[2][0] / n), (unsigned)(blitStat[2][1] / n),
             (unsigned)(blitStat[3][0] / n), (unsigned)(blitStat[3][1] / n),
             (unsigned)(blitStat[4][0] / n));
    jag_logf("x0.01/round: drawBitmap strips %u, resetBackground %u",
             (unsigned)(nStrip * 100 / n), (unsigned)(nReset * 100 / n));
    nStrip = nReset = 0;
    memset(blitStat, 0, sizeof(blitStat));
}

/* One rectangle copied (cmd B_COPY) or filled (B_FILL, colour), w x h pixels, each end at any
 * byte, between buffers of pitch 320 or the room's.  Phrase mode when both ends and the width
 * are whole phrases, pixel mode otherwise.  A long blit stops the 68000 for its whole length,
 * interrupts included (hw/05 rule 6b): split into bands of a fraction of a millisecond.  The
 * 68000 is slow here (every instruction counts: bus shared with the GPU and the Blitter), so no
 * libgcc call, no function call but the queue's. */
static void q_blit(uint32_t dst, uint32_t dpitch, uint32_t src, uint32_t spitch,
                   int w, int h, uint32_t cmd, int colour)
{
    uint32_t dflags, sflags, pat, limit, area;
    int rows;
    if (w <= 0 || h <= 0)
        return;
    dflags = dpitch == 320 ? flags320 : dpitch == roomP ? roomFlags : pitch_flags(dpitch);
    sflags = cmd == B_FILL ? dflags :
             spitch == 320 ? flags320 : spitch == roomP ? roomFlags : pitch_flags(spitch);
    if ((dst & 7) || (w & 7) || (cmd != B_FILL && (src & 7))) {
        dflags |= F_XADDPIX;
        sflags |= F_XADDPIX;
        limit = 1024;
        blitStat[4][0]++;
    } else
        limit = cmd == B_FILL ? 16384 : 4096;
    area = mul16((uint32_t)w, (uint32_t)h);
    rows = area <= limit ? h : (int)div16(limit, (uint32_t)w);
    if (rows < 1)
        rows = 1;
    pat = (uint8_t)colour;
    pat |= pat << 8;
    pat |= pat << 16;
    {
        const int k = cmd == B_FILL ? 0 : dst < SCREEN0 ? 1 : src < SCREEN0 ? 2 : 3;
        blitStat[k][0]++;
        blitStat[k][1] += area;
    }
    while (h > 0) {
        const int n = h < rows ? h : rows;
        volatile uint32_t *c = slot();
        c[0] = GQ_BLIT;
        c[1] = dst;
        c[2] = src;
        c[3] = dflags;
        c[4] = sflags;
        c[5] = ((uint32_t)n << 16) | (uint32_t)w;
        c[6] = cmd;
        c[7] = pat;
        gq_commit();
        dst += mul16((uint32_t)n, dpitch);
        src += mul16((uint32_t)n, spitch);
        h -= n;
    }
}

static int vs_xstart(const VirtScreen *vs)
{
    return vs->number == kMainVirtScreen ? g.mainXStart : 0;
}

static uint32_t getPixels(const VirtScreen *vs, int x, int y)
{
    return vs->pixels + mul16((uint32_t)y, (uint32_t)vs->pitch) + (uint32_t)(vs_xstart(vs) + x);
}

/* the main screen's back buffer at the same place (only the main screen has one) */
static uint32_t getBackPixels(const VirtScreen *vs, int x, int y)
{
    return BACKBUF + mul16((uint32_t)y, roomP) + (uint32_t)(vs_xstart(vs) + x);
}

static uint32_t getBasePtr(const VirtScreen *vs, int x, int y)
{
    return vs->pixels + mul16((uint32_t)y, (uint32_t)vs->pitch) + (uint32_t)x;
}

static void setDirtyRange(VirtScreen *vs, int top, int bottom)
{
    int i;
    for (i = 0; i < NUM_STRIPS; i++) {
        vs->tdirty[i] = top;
        vs->bdirty[i] = bottom;
    }
}

static int isLightOn(void)
{
    return (VAR(VAR_CURRENT_LIGHTS) & 2) != 0;     /* LIGHTMODE_room_lights_on */
}

static void initVirtScreen(int slot, int top, int width, int height, int twobufs, int scrollable)
{
    static const uint32_t bufs[4] = { FRONTBUF, TEXTVS, VERBVS, BANNERVS };
    static const uint32_t sizes[4] = { FRONTBUF_SIZE, TEXTVS_SIZE, VERBVS_SIZE, BANNERVS_SIZE };
    VirtScreen *vs = &virtscr[slot];
    uint32_t size;

    vs->number = slot;
    vs->w = width;
    vs->topline = top;
    vs->h = height;
    vs->hasTwoBuffers = twobufs;
    vs->scrollable = scrollable;
    vs->pitch = width;
    size = mul16((uint32_t)vs->pitch, (uint32_t)vs->h);
    if (scrollable)
        size += mul16((uint32_t)vs->pitch, 4);
    if (width != 320 || size > sizes[slot])
        plat_error("initVirtScreen: too big");
    vs->pixels = bufs[slot];
    q_blit(vs->pixels, 320, 0, 0, 320, (int)(size / 320), B_FILL, 0);   /* reset background */
    if (twobufs)                             /* a new resource: zeroed */
        q_blit(BACKBUF, 1024, 0, 0, 1024, (int)(BACKBUF_SIZE / 1024), B_FILL, 0);
    if (slot != 3)
        setDirtyRange(vs, 0, height);
}

void plat_initScreens(int b, int h)
{
    initVirtScreen(kBannerVirtScreen, 80, 320, 12, 0, 0);
    initVirtScreen(kMainVirtScreen, b, 320, h - b, 1, 1);
    initVirtScreen(kTextVirtScreen, 0, 320, b, 0, 0);
    initVirtScreen(kVerbVirtScreen, h, 320, 200 - h, 0, 0);
}

static VirtScreen *findVirtScreen(int y)
{
    int i;
    for (i = 0; i < 3; i++)
        if (y >= virtscr[i].topline && y < virtscr[i].topline + virtscr[i].h)
            return &virtscr[i];
    return 0;
}

/* markRectAsDirty, the virtual screen's part (the logic has clipped top and bottom) */
void plat_markRectAsDirty(int virt, int left, int right, int top, int bottom)
{
    VirtScreen *vs = &virtscr[virt];
    int lp = left / 8, rp = right / 8;
    if (lp >= NUM_STRIPS || rp < 0)
        return;
    if (lp < 0)
        lp = 0;
    if (rp >= NUM_STRIPS)
        rp = NUM_STRIPS - 1;
    while (lp <= rp) {
        if (top < vs->tdirty[lp])
            vs->tdirty[lp] = top;
        if (bottom > vs->bdirty[lp])
            vs->bdirty[lp] = bottom;
        lp++;
    }
}

/* ---- the system screen ---------------------------------------------------------------- */

/* The screen the copies go into (scrHidden) and the one the last hand-over gave the OP
 * (scrShown, 0 before the first).  The copies into scrHidden are entries of a list in DRAM
 * (GQ_RECTS: the source's address, the destination's offset, B_COUNT), the GPU walks them:
 * on the 68000 every command costs (measured, session 8: ~8 half-lines a command in the bar),
 * an entry is 3 longs.  The list of the last hand-over (due) is replayed screen to screen
 * into scrHidden before anything else goes there.  A full list: the whole screen. */
static uint32_t curList = SCR_LIST0, dueList = SCR_LIST1;
static int curN, dueN, curAll, dueAll;
static int pendFrom;                        /* curList's entries not queued yet: from here */
static uint32_t scrHidden, scrShown;
static int scrSynced;                       /* scrHidden holds all the last hand-over showed */

static int handover_taken(void)
{
    return HW32(NEXT_SEQ) == HW32(VIEW_SEQ);
}

static int textClean;                       /* the text surface is all 0xFD */

static void q_rects(uint32_t list, int n, uint32_t dstBase, uint32_t srcBase)
{
    volatile uint32_t *c = gq_slot();
    c[0] = GQ_RECTS;
    c[1] = list;
    c[2] = (uint32_t)n;
    c[3] = dstBase;
    c[4] = srcBase;
    c[5] = srcBase || textClean ? 0 : TEXTSURF;   /* the update composites the text */
    gq_commit();
}

/* the text surface all CHARSET_MASK_TRANSPARENCY (ScummVM's clearTextSurface) */
static void text_clear(void)
{
    q_blit(TEXTSURF, 320, 0, 0, 320, 200, B_FILL, CHARSET_MASK_TRANSPARENCY);
    textClean = 1;
}

static uint32_t rd32be(const uint8_t *p)    /* 4 bytes at any address */
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* the entries written since the last flush, to the GPU: before any other command (the
 * queue's order is the picture's) and at the hand-over */
static void rects_flush(void)
{
    if (curN > pendFrom)
        q_rects(curList + (uint32_t)pendFrom * 12, curN - pendFrom, scrHidden, 0);
    pendFrom = curN;
}

static volatile uint32_t *slot(void)
{
    if (curN > pendFrom)
        rects_flush();
    return gq_slot();
}

/* Before the first copy into the hidden screen after a hand-over: that hand-over taken (until
 * then the hidden screen is still on screen), then what it showed copied across. */
static void scr_begin(void)
{
    if (scrSynced)
        return;
    scrSynced = 1;
    if (!scrShown)
        return;
    jag_wait_until(handover_taken, "hand-over: never taken");
    if (dueAll)
        q_blit(scrHidden, 320, scrShown, 320, 320, 200, B_COPY, 0);
    else if (dueN)
        q_rects(dueList, dueN, scrHidden, scrShown);
    dueN = dueAll = 0;
}

/* drawStripToScreen (v < 7, DOS VGA: no render mode post-processing): the rectangle of the
 * virtual screen into the system screen: entries of the list, at most 4 KB each (a long blit
 * stops the 68000, interrupts included).  The text surface's composite over it is the next
 * step (it holds nothing yet: plat_drawChar draws nothing). */
static void drawStripToScreen(VirtScreen *vs, int x, int width, int top, int bottom)
{
    int y, height, rows;
    uint32_t src, dst;

    if (bottom <= top || top >= vs->h)
        return;
    if (width > vs->w - x)
        width = vs->w - x;
    if (top < 0)
        top = 0;                                 /* _screenTop is 0 before v7 */
    if (bottom > 200)
        bottom = 200;
    y = vs->topline + top;
    height = bottom - top;
    if (width <= 0 || height <= 0)
        return;
    if ((x | width) & 7)
        plat_error("drawStripToScreen: not whole strips");

    scr_begin();
    src = getPixels(vs, x, top);
    dst = mul16((uint32_t)y, 320) + (uint32_t)x;
    rows = mul16((uint32_t)width, (uint32_t)height) <= 4096 ? height : (int)div16(4096, (uint32_t)width);
    while (height > 0) {
        const int n = height < rows ? height : rows;
        volatile uint32_t *e;
        if (curN == SCR_RECTS) {                 /* full: queued, the GPU done with it, and */
            rects_flush();                       /* the next sync copies the whole screen */
            gq_drain();
            curN = pendFrom = 0;
            curAll = 1;
        }
        e = (volatile uint32_t *)(curList + (uint32_t)curN * 12);
        e[0] = src;
        e[1] = dst;
        e[2] = ((uint32_t)n << 16) | (uint32_t)width;
        curN++;
        blitStat[2][0]++;
        blitStat[2][1] += mul16((uint32_t)width, (uint32_t)n);
        src += mul16((uint32_t)n, 320);
        dst += mul16((uint32_t)n, 320);
        height -= n;
    }
}

static void updateDirtyScreen(int slot)
{
    VirtScreen *vs = &virtscr[slot];
    int i, w = 8, start = 0;

    if (vs->h == 0)
        return;
    for (i = 0; i < NUM_STRIPS; i++) {
        if (vs->bdirty[i]) {
            const int top = vs->tdirty[i];
            const int bottom = vs->bdirty[i];
            vs->tdirty[i] = vs->h;
            vs->bdirty[i] = 0;
            if (i != (NUM_STRIPS - 1) && vs->bdirty[i + 1] == bottom && vs->tdirty[i + 1] == top) {
                w += 8;
                continue;
            }
            drawStripToScreen(vs, start * 8, w, top, bottom);
            w = 8;
        }
        start = i + 1;
    }
}

/* ---- the palette ---------------------------------------------------------------------- */
static uint16_t clutStage[256];             /* the CLUT as updatePalette left it, RGB16 */
static uint8_t palRGB[768];                 /* the same in 8-bit RGB: its CRC is logged */
static int palDirty;

/* the Jaguar's RGB16: R 5 bits, B 5, G 6 */
static uint16_t rgb16(const uint8_t *c)
{
    return (uint16_t)(((c[0] >> 3) << 11) | ((c[2] >> 3) << 6) | (c[1] >> 2));
}

void plat_setPalette(int first, int num, const uint8_t *rgb)
{
    int i;
    for (i = 0; i < num; i++) {
        clutStage[first + i] = rgb16(rgb + 3 * i);
        palRGB[3 * (first + i) + 0] = rgb[3 * i + 0];
        palRGB[3 * (first + i) + 1] = rgb[3 * i + 1];
        palRGB[3 * (first + i) + 2] = rgb[3 * i + 2];
    }
    palDirty = 1;
}

/* ---- the hand-over: what ScummVM's screen shows after drawDirtyScreenParts, the Jaguar hands
 * to the video interrupt there, in one piece: the system screen the copies went into (if any
 * did) and the CLUT updatePalette changed (if it did), with the queue's count at that moment
 * (NEXT_FENCE).  The interrupt takes it once the GPU has done that many commands: loads the
 * CLUT and shows the screen in the same vertical blank (src/main.s SetView), so a new room
 * never shows a frame in the old room's colours, and a frame never shows a copy the GPU has
 * not finished.  The 68000 does not wait for the GPU here. ---- */
static int traceChecks;                     /* the trace phase: CHECKS */
static uint32_t presentRound;               /* drawDirtyScreenParts calls: the loop round */
static uint32_t lastBb, lastZ, lastFb, lastSc;

static int cursor_hand(int force);

static void hand_over(void)
{
    const int swap = curN || curAll;
    const int cur = cursor_hand(0);         /* the cursor changed (image, shown, place)? */
    if (!swap && !palDirty && !cur)
        return;
    rects_flush();
    jag_wait_until(handover_taken, "hand-over: never taken");
    cursor_hand(1);
    HW32(NEXT_BUF) = swap ? scrHidden : scrShown;
    if (palDirty) {                         /* the interrupt loads ROOM_CLUT at every take */
        volatile uint32_t *clut = (volatile uint32_t *)ROOM_CLUT;
        const uint32_t *from = (const uint32_t *)clutStage;
        int i;
        for (i = 0; i < 128; i++)
            clut[i] = from[i];
    }
    HW32(NEXT_CLUT) = ROOM_CLUT;
    HW32(NEXT_FENCE) = gq_head();
    HW32(NEXT_SEQ) = HW32(NEXT_SEQ) + 1;
    if (palDirty && (traceChecks & 1))
        jag_logf("pal %u crc %x", (unsigned)presentRound, (unsigned)jag_crc(palRGB, 768));
    palDirty = 0;
    if (swap) {
        const uint32_t t = dueList;
        dueList = curList;
        curList = t;
        dueN = curN;
        dueAll = curAll;
        curN = curAll = pendFrom = 0;
        scrShown = scrHidden;
        scrHidden = scrHidden == SCREEN0 ? SCREEN1 : SCREEN0;
        scrSynced = 0;
    }
}

/* The trace phase's checks, as scumm_pc prints them after the round (tools/pal_crc.py): each
 * buffer's CRC32 when it changed, once the GPU has done everything queued. */
static void checks(void)
{
    uint32_t c, y;
    const int rows = scumm_mainScreenHeight();
    if (!(traceChecks & 30))
        return;
    gq_drain();
    if ((traceChecks & 2) && g.currentRoom && g.roomImage) {
        /* the back buffer's window: 320 x the main screen, at the camera (scumm_pc --bbcrc) */
        c = 0xFFFFFFFFUL;
        for (y = 0; y < (uint32_t)rows; y++)
            c = jag_crc_more(c, (const uint8_t *)BACKBUF + mul16(y, roomP) + g.mainXStart, 320);
        c = ~c;
        if (c != lastBb)
            jag_logf("bb %u crc %x", (unsigned)presentRound, (unsigned)c);
        lastBb = c;
    }
    if ((traceChecks & 4) && g.currentRoom && g.roomImage && maskUsed) {
        /* the z-plane buffer, all of it initBGBuffers sized (scumm_pc --zcrc) */
        c = jag_crc((const uint8_t *)MASKBUF, maskUsed);
        if (c != lastZ)
            jag_logf("z %u crc %x", (unsigned)presentRound, (unsigned)c);
        lastZ = c;
    }
    if (traceChecks & 8) {
        /* the front buffer's window: the main virtual screen's rows from getPixels(0, 0),
         * 320 wide (scumm_pc --fbcrc) */
        const VirtScreen *vs = &virtscr[kMainVirtScreen];
        c = jag_crc((const uint8_t *)getPixels(vs, 0, 0), mul16((uint32_t)vs->h, 320));
        if (c != lastFb)
            jag_logf("fb %u crc %x", (unsigned)presentRound, (unsigned)c);
        lastFb = c;
    }
    if (traceChecks & 16) {
        /* the system screen the last hand-over showed (scumm_pc --sccrc) */
        c = jag_crc((const uint8_t *)(scrShown ? scrShown : scrHidden), 320 * 200);
        if (c != lastSc)
            jag_logf("sc %u crc %x", (unsigned)presentRound, (unsigned)c);
        lastSc = c;
    }
}

void plat_drawDirtyScreenPartsOnly(void)
{
    updateDirtyScreen(kVerbVirtScreen);
    updateDirtyScreen(kTextVirtScreen);
    if (g.camera.last_x != g.camera.cur_x) {
        VirtScreen *vs = &virtscr[kMainVirtScreen];
        drawStripToScreen(vs, 0, vs->w, 0, vs->h);
        setDirtyRange(vs, vs->h, 0);
    } else {
        updateDirtyScreen(kMainVirtScreen);
    }
}

void plat_drawDirtyScreenParts(void)
{
    plat_drawDirtyScreenPartsOnly();
    hand_over();
    checks();
    presentRound++;
}

/* a banner's waitForTimer: what drawDirtyScreenParts and updateDirtyScreen put in the system
 * screen, and the cursor, shown now (not a round: no checks) */
void gfx_present(void)
{
    hand_over();
    if (traceChecks & 16) {                 /* the screen shown (scumm_pc --sccrc: "banner") */
        gq_drain();
        jag_logf("banner %u crc %x", (unsigned)presentRound,
                 (unsigned)jag_crc((const uint8_t *)(scrShown ? scrShown : scrHidden), 320 * 200));
    }
}

/* ---- the message banner (gfx_gui.cpp saveSurfacesPreGUI / restoreSurfacesPostGUI, gfx.cpp
 * drawPixel), Blitter copies in the queue's order like every other.  The engine is paused
 * while a banner is up: drawPixel's few hundred pixels are the 68000's own, written once the
 * GPU has done everything queued before them (gq_drain). ---- */
static int guiTextClean;                    /* textClean when the surfaces were kept */

void plat_saveSurfacesPreGUI(void)
{
    VirtScreen *m = &virtscr[kMainVirtScreen], *v = &virtscr[kVerbVirtScreen];
    /* the main screen's rows from getBasePtr(xstart, y), 320 each: one run of the 320-pitch
     * buffer from xstart (a wide room wraps into the next row there, as ScummVM's does) */
    q_blit(GUI_MAIN, 320, getPixels(m, 0, 0), 320, m->w, m->h, B_COPY, 0);
    q_blit(GUI_VERB, 320, getBasePtr(v, 0, 0), 320, v->pitch, v->h, B_COPY, 0);
    q_blit(GUI_TEXT, 320, TEXTSURF, 320, 320, 200, B_COPY, 0);
    guiTextClean = textClean;
    /* the text surface stamped on the main screen, where it is not CHARSET_MASK_TRANSPARENCY:
     * its row y onto the main screen's row y (x < pitch, y < h), at xstart + x wrapped */
    if (!textClean)
        q_blit(getPixels(m, 0, 0), 320, TEXTSURF, 320, 320, m->h, B_TEXT,
               CHARSET_MASK_TRANSPARENCY);
}

void plat_restoreSurfacesPostGUI(void)
{
    VirtScreen *m = &virtscr[kMainVirtScreen], *v = &virtscr[kVerbVirtScreen];
    q_blit(TEXTSURF, 320, GUI_TEXT, 320, 320, 200, B_COPY, 0);
    textClean = guiTextClean;
    q_blit(getPixels(m, 0, 0), 320, GUI_MAIN, 320, m->w, m->h, B_COPY, 0);
    setDirtyRange(m, 0, m->h);
    q_blit(getBasePtr(v, 0, 0), 320, GUI_VERB, 320, v->pitch, v->h, B_COPY, 0);
    setDirtyRange(v, 0, v->h);
}

void plat_fillVirtScreen(int virt, int color)
{
    VirtScreen *vs = &virtscr[virt];
    q_blit(getBasePtr(vs, 0, 0), 320, 0, 0, vs->w, vs->h, B_FILL, color);
}

void plat_drawPixel(int virt, int x, int y, int color)
{
    gq_drain();
    HW8(getPixels(&virtscr[virt], x, y)) = (uint8_t)color;
}

void plat_setDirtyRange(int virt, int top, int bottom)
{
    setDirtyRange(&virtscr[virt], top, bottom);
}

void plat_updateDirtyScreen(int virt)
{
    updateDirtyScreen(virt);
}

/* ---- the room and object images (Gdi) -------------------------------------------------- */
static uint32_t tabGen;                     /* the roomPaletteGen the GPU's tables hold */
static int tabValid;

/* initBGBuffers: a new room.  Its back buffer at the room's pitch; the z-plane buffer cleared
 * as ScummVM's initBGBuffers does, once the GPU has done the old room's commands (the strips
 * and GQ_MASKs written with the old pitch). */
void plat_initBGBuffers(int height)
{
    uint32_t field = 0, rows;
    (void)height;
    roomW = (uint32_t)g.roomWidth;
    roomH = (uint32_t)g.roomHeight;
    roomP = blit_pitch(roomW, &field);
    rows = roomH > 200 ? roomH : 200;
    if (!roomP || roomP * rows > BACKBUF_SIZE || (roomW & 7))
        plat_error("initBGBuffers: the room does not fit the back buffer");
    roomFlags = F_PIXEL8 | (field << 9);
    maskItem = (roomH + 4) * 40;
    maskUsed = (uint32_t)g.numZBuffer * maskItem;
    if (g.numZBuffer < 1 || maskUsed > MASKBUF_SIZE)
        plat_error("initBGBuffers: the z-planes do not fit MASKBUF");
    gq_drain();
    {
        volatile uint32_t *m = (volatile uint32_t *)MASKBUF;
        uint32_t n = (maskUsed + 3) / 4;
        while (n--)
            *m++ = 0;
    }
}

/* Gdi::drawBitmap on the main virtual screen: the same strips ScummVM draws (its clipping to
 * the visible ones), each decoded into the back buffer (GQ_STRIP), copied into the front
 * buffer (or cleared there, the lights off), its z-planes into the mask buffer (GQ_MASK) */
void plat_drawBitmap(const uint8_t *ptr, int x, int y, int width, int height,
                     int stripnr, int numstrip, int flag)
{
    VirtScreen *vs = &virtscr[kMainVirtScreen];
    int sx, limit, k, i;
    const int xstart = g.mainXStart;
    const int numzbuf = g.numZBuffer;
    const int lightsOn = isLightOn();
    const uint8_t *zplane[4];
    const uint8_t *zoffs[4];                    /* each plane's LE16 offset of this strip */
    uint32_t zrow[4];                           /* each plane's row y in the mask buffer */
    uint32_t backRow, frontRow;                 /* row y of the back and front buffers */
    int smapLen;
    const uint32_t backFlags = roomFlags | (lightsOn ? 0 : 1);   /* GQ_STRIP +28 */
    (void)width;

    if (!ptr || height <= 0)
        plat_error("drawBitmap: no image");
    if (flag & DB_DRAW_MASK_ON_ALL)
        plat_error("decodeMask: dbDrawMaskOnAll is not a v4 flag");
    /* getZPlanes (GF_SMALL_HEADER): plane 1 at the LE32 after the image, the next ones
     * chained by their LE16 sizes; world.c counts at most 4 */
    if (numzbuf < 1 || numzbuf > 4)
        plat_error("drawBitmap: numZBuffer");
    zplane[0] = ptr;
    if (numzbuf > 1) {
        zplane[1] = ptr + rd32(ptr);
        for (i = 2; i < numzbuf; i++)
            zplane[i] = zplane[i - 1] + rd16(zplane[i - 1]);
    }
    if ((uint32_t)(y + height) > ((roomH > 200 ? roomH : 200)) || y + height > vs->h + 4)
        plat_error("drawBitmap: below the buffer");
    sx = x - xstart / 8;
    if (sx < 0) {
        numstrip -= -sx;
        x += -sx;
        stripnr += -sx;
        sx = 0;
    }
    limit = ((int)roomW > vs->w ? (int)roomW : vs->w) / 8 - x;
    if (limit > numstrip)
        limit = numstrip;
    if (limit > NUM_STRIPS - sx)
        limit = NUM_STRIPS - sx;
    if (limit <= 0)
        return;

    /* writeRoomColor's _roomPalette: the GPU's tables, rewritten (once the GPU is done with
     * them) when it changed (g.roomPaletteGen: a 256-byte memcmp per call cost the 68000 ~18
     * half-lines) */
    if (!tabValid || tabGen != g.roomPaletteGen) {
        volatile uint32_t *tab = (volatile uint32_t *)GQ_COLTAB;   /* longs: the GPU reads its local RAM 32 bits at a time */
        volatile uint32_t *c;
        gq_drain();
        for (i = 0; i < 256; i++)
            tab[i] = g.roomPalette[i];
        tab[255] |= 0x100;                      /* _transparentColor: not written by a
                                                   transparent strip */
        tabGen = g.roomPaletteGen;
        tabValid = 1;
        c = slot();
        c[0] = GQ_TABLES;
        c[1] = GQ_COLTAB;
        gq_commit();
    }
    for (i = 1; i < numzbuf; i++) {
        zoffs[i] = zplane[i] + 2 + stripnr * 2;
        zrow[i] = MASKBUF + (uint32_t)i * maskItem + (uint32_t)y * 40;
    }


    smapLen = (int)rd32(ptr);
    backRow = BACKBUF + mul16((uint32_t)y, roomP);
    frontRow = getBasePtr(vs, 0, y);
    for (k = 0; k < limit; ++k, ++stripnr, ++sx, ++x) {
        int offset = -1, codec, hdr = 0;
        const uint8_t *st;
        volatile uint32_t *c;
        nStrip++;
        if (y < vs->tdirty[sx])
            vs->tdirty[sx] = y;
        if (y + height > vs->bdirty[sx])
            vs->bdirty[sx] = y + height;
        if (stripnr * 4 + 4 < smapLen)
            offset = (int)rd32(ptr + stripnr * 4 + 4);
        if (offset < 0 || offset > smapLen - 1) {
            jag_logf("drawStrip: room %d (%u wide), %s %d, state %d, strip %d of %d (smapLen %d), x %d y %d "
                     "h %d, numstrip %d, camera %d, xstart %d",
                     g.currentRoom, (unsigned)roomW, (flag & DB_OBJECT_MODE) ? "object" : "the room",
                     g.drawingObject, g.drawingObject ? getState(g.drawingObject) : 0,
                     stripnr, (int)((rd32(ptr + 4) - 4) / 4), smapLen, x, y, height, numstrip,
                     g.camera.cur_x, xstart);
            plat_error("drawStrip: screen strip out of range");
        }
        st = ptr + offset;
        codec = st[0];
        /* bits 0-7 codec % 10 (no division: libgcc's would cost the 68000 a call) */
        if (codec >= 14 && codec <= 18)
            hdr = (codec - 10) | 0x100;
        else if (codec >= 24 && codec <= 28)
            hdr = codec - 20;
        else if (codec >= 34 && codec <= 38)
            hdr = (codec - 30) | 0x100 | 0x200;
        else if (codec >= 44 && codec <= 48)
            hdr = (codec - 40) | 0x200;
        else
            plat_error("drawBitmap: a codec the GPU does not decode yet (RAW256?)");
        /* the strip into the back buffer, then (the GPU, once decoded) into the front
         * buffer at getBasePtr(x * 8, y): copy8Col, or clear8Col with the lights off */
        c = slot();
        c[0] = GQ_STRIP;
        c[1] = (uint32_t)(st + 1);
        c[2] = (uint32_t)hdr;
        c[3] = backRow + (uint32_t)x * 8;
        c[4] = roomP;
        c[5] = (uint32_t)height;
        c[6] = frontRow + (uint32_t)x * 8;
        c[7] = backFlags;
        gq_commit();
        blitStat[1][0]++;
        blitStat[1][1] += (uint32_t)height * 8;
        /* decodeMask: each z-plane's strip into the mask buffer (the GPU keeps the order).
         * A transparent strip of an image drawn with DB_ALLOW_MASK_OR is ORed over what is
         * there, and an empty one leaves it; otherwise copied, an empty strip as zeros. */
        for (i = 1; i < numzbuf; i++) {
            const int orMode = (hdr & 0x200) && (flag & DB_ALLOW_MASK_OR);
            const unsigned offs = zoffs[i][0] | (zoffs[i][1] << 8);
            const uint32_t dst = zrow[i] + (uint32_t)x;
            zoffs[i] += 2;
            if (!offs && orMode)
                continue;
            c = slot();
            c[0] = GQ_MASK;
            c[1] = offs ? (uint32_t)(zplane[i] + offs) : 0;
            if (orMode) {                       /* the long that holds the byte, its lane */
                c[2] = dst & ~3UL;
                c[3] = 0xFF000000UL >> ((dst & 3) * 8);
            } else {
                c[2] = dst;
                c[3] = 0;
            }
            c[4] = (uint32_t)height;
            gq_commit();
        }
    }
}

/* Gdi::resetBackground: an actor's strip back from the background */
void plat_resetBackground(int top, int bottom, int strip)
{
    VirtScreen *vs = &virtscr[kMainVirtScreen];
    uint32_t x;
    int numLinesToProcess;

    nReset++;
    if (top < 0)
        top = 0;
    if (bottom > vs->h)
        bottom = vs->h;
    if (top >= bottom)
        return;
    if (strip < 0 || strip >= NUM_STRIPS)
        plat_error("resetBackground: strip");
    if (top < vs->tdirty[strip])
        vs->tdirty[strip] = top;
    if (bottom > vs->bdirty[strip])
        vs->bdirty[strip] = bottom;

    x = (uint32_t)(strip + vs_xstart(vs) / 8) * 8;
    numLinesToProcess = bottom - top;
    if (isLightOn())
        q_blit(getBasePtr(vs, (int)x, top), 320, BACKBUF + mul16((uint32_t)top, roomP) + x, roomP,
               8, numLinesToProcess, B_COPY, 0);
    else
        q_blit(getBasePtr(vs, (int)x, top), 320, 0, 0, 8, numLinesToProcess, B_FILL, 0);
}

/* restoreBackground, the pixels: the logic has found the virtual screen, clipped the
 * rectangle to it (its own coordinates, right and bottom exclusive) and marked it.  The text
 * surface's part (cleared where the main screen is restored) comes with the text. */
void plat_restoreBackground(int virt, int left, int top, int right, int bottom, int backColor)
{
    VirtScreen *vs = &virtscr[virt];
    const int width = right - left, height = bottom - top;

    if (width <= 0 || height <= 0)
        return;
    if (vs->hasTwoBuffers && g.currentRoom != 0 && isLightOn()) {
        q_blit(getPixels(vs, left, top), (uint32_t)vs->pitch, getBackPixels(vs, left, top), roomP,
               width, height, B_COPY, 0);
        if (vs->number == kMainVirtScreen && text_hasMask())   /* (the screen's own y: ScummVM's) */
            q_blit(TEXTSURF + mul16((uint32_t)top, 320) + (uint32_t)left, 320, 0, 0,
                   width, height, B_FILL, CHARSET_MASK_TRANSPARENCY);
    } else
        q_blit(getPixels(vs, left, top), (uint32_t)vs->pitch, 0, 0, width, height, B_FILL, backColor);
}

/* restoreCharsetBg, the pixels: over the room nothing (the screen is redrawn from the buffers
 * the logic marked); a screen without a background is cleared; the text surface cleared. */
void plat_restoreCharsetBg(int virt)
{
    VirtScreen *vs = &virtscr[virt];
    if (vs->hasTwoBuffers && g.currentRoom != 0 && isLightOn()) {
        if (vs->number != kMainVirtScreen)
            plat_error("restoreCharsetBg: two buffers on a screen other than the main one");
    } else {
        q_blit(getPixels(vs, 0, 0), (uint32_t)vs->pitch, 0, 0, vs->pitch, vs->h, B_FILL, 0);
    }
    if (vs->hasTwoBuffers)
        text_clear();
}

/* drawBox: the fill (o5_drawBox) */
void plat_drawBox(int x, int y, int x2, int y2, int color)
{
    VirtScreen *vs;
    int width, height, t;

    if ((vs = findVirtScreen(y)) == 0)
        return;
    if (x > x2) { t = x; x = x2; x2 = t; }
    if (y > y2) { t = y; y = y2; y2 = t; }
    x2++;
    y2++;
    y -= vs->topline;
    y2 -= vs->topline;
    if (x < 0)
        x = 0;
    else if (x >= vs->w)
        return;
    if (x2 < 0)
        return;
    else if (x2 > vs->w)
        x2 = vs->w;
    if (y < 0)
        y = 0;
    else if (y > vs->h)
        return;
    if (y2 < 0)
        return;
    else if (y2 > vs->h)
        y2 = vs->h;
    width = x2 - x;
    height = y2 - y;
    if (width <= 0 || height <= 0)
        return;

    markRectAsDirty(vs->number, x, x2, y, y2, 0);
    if (color == -1) {
        if (vs->number != kMainVirtScreen)
            plat_error("can only copy bg to main window");
        q_blit(getPixels(vs, x, y), (uint32_t)vs->pitch, getBackPixels(vs, x, y), roomP,
               width, height, B_COPY, 0);
        if (text_hasMask())
            q_blit(TEXTSURF + mul16((uint32_t)y, 320) + (uint32_t)x, 320, 0, 0,
                   width, height, B_FILL, CHARSET_MASK_TRANSPARENCY);
    } else {
        q_blit(getPixels(vs, x, y), (uint32_t)vs->pitch, 0, 0, width, height, B_FILL, color);
    }
}

/* ---- effects --------------------------------------------------------------------------- */
typedef struct {
    uint8_t numOfIterations;
    int8_t deltaTable[16];
    uint8_t stripTable[16];
} TransitionEffect;

static const TransitionEffect transitionEffects[6] = {
    { 13, { 1, 1, -1, 1,  -1, 1, -1, -1,  1, -1, -1, -1,  1, 1, 1, -1 },
          { 0, 0, 39, 0,  39, 0, 39, 24,  0, 24, 39, 24,  0, 0, 0, 24 } },
    { 25, { 0, 1, 2, 1,  2, 0, 2, 1,  2, 0, 2, 1,  0, 0, 0, 0 },
          { 0, 0, 0, 0,  0, 0, 0, 0,  1, 0, 1, 0,  255, 0, 0, 0 } },
    { 25, { -2, -1, 0, -1,  -2, -1, -2, 0,  -2, -1, -2, 0,  0, 0, 0, 0 },
          { 39, 24, 39, 24,  39, 24, 39, 24,  38, 24, 38, 24,  255, 0, 0, 0 } },
    { 25, { 0, -1, -2, -1,  -2, 0, -2, -1,  -2, 0, -2, -1,  0, 0, 0, 0 },
          { 0, 24, 39, 24,  39, 0, 39, 24,  38, 0, 38, 24,  255, 0, 0, 0 } },
    { 9,  { -1, -1, 1, -1,  -1, 1, 1, 1,  -1, -1, -1, 1,  1, -1, 1, 1 },
          { 7, 7, 32, 7,  7, 8, 32, 8,  7, 8, 7, 8,  32, 7, 32, 8 } },
    { 16, { 2, 0, 2, 0,  2, 0, 2, 0,  0, 0, 0, 0,  0, 0, 0, 0 },
          { 0, 0, 0, 15,  1, 0, 1, 15,  255, 0, 0, 0,  255, 0, 0, 0 } },
};

/* transitionEffect: 8x8 blocks of the main screen copied in the effect's order.  ScummVM
 * shows the screen at the end of each iteration (waitForTimer): not yet here (the effects'
 * step), so the effect's last picture is shown at the round's hand-over. */
static void transitionEffect(int a)
{
    int delta[16], tab_2[16];
    int i, j, bottom, l, t, r, b;
    const int height = virtscr[kMainVirtScreen].h < 200 ? virtscr[kMainVirtScreen].h : 200;
    const int numOfIterations = transitionEffects[a].numOfIterations;
    VirtScreen *vs = &virtscr[kMainVirtScreen];

    for (i = 0; i < 16; i++) {
        delta[i] = transitionEffects[a].deltaTable[i];
        j = transitionEffects[a].stripTable[i];
        if (j == 24)
            j = height / 8 - 1;
        tab_2[i] = j;
    }
    bottom = height / 8;
    for (j = 0; j < numOfIterations; j++) {
        for (i = 0; i < 4; i++) {
            l = tab_2[i * 4];
            t = tab_2[i * 4 + 1];
            r = tab_2[i * 4 + 2];
            b = tab_2[i * 4 + 3];
            if (t == b) {
                while (l <= r) {
                    if (l >= 0 && l < NUM_STRIPS && t < bottom) {
                        vs->tdirty[l] = t * 8;
                        vs->bdirty[l] = (b + 1) * 8;
                    }
                    l++;
                }
            } else {
                if (l < 0 || l >= NUM_STRIPS || b <= t)
                    continue;
                if (b > bottom)
                    b = bottom;
                if (t < 0)
                    t = 0;
                vs->tdirty[l] = t * 8;
                vs->bdirty[l] = (b + 1) * 8;
            }
            updateDirtyScreen(kMainVirtScreen);
        }
        for (i = 0; i < 16; i++)
            tab_2[i] += delta[i];
    }
}

void plat_fadeIn(int effect)
{
    switch (effect) {
    case 0:
    case 129:
        break;
    case 1: case 2: case 3: case 4: case 5: case 6:
        setDirtyRange(&virtscr[kMainVirtScreen], 0, 0);
        transitionEffect(effect - 1);
        break;
    case 128: case 134: case 135:            /* dissolveEffect: the logic's order, plat_dissolve */
        break;
    default:                                     /* 130-133: scrollEffect is v5/v6 */
        plat_error("fadeIn: effect not done yet");
    }
}

void plat_fadeOut(int effect, int run)
{
    VirtScreen *vs = &virtscr[kMainVirtScreen];
    setDirtyRange(vs, 0, 0);
    if (!run)
        return;
    q_blit(getPixels(vs, 0, 0), 320, 0, 0, 320, vs->h, B_FILL, 0);
    switch (effect) {
    case 1: case 2: case 3: case 4: case 5: case 6:
        transitionEffect(effect - 1);
        break;
    case 129:
        setDirtyRange(vs, 0, vs->h);
        updateDirtyScreen(kMainVirtScreen);
        break;
    case 128: case 134: case 135:            /* dissolveEffect: plat_dissolve */
        break;
    default:
        plat_error("fadeOut: effect not done yet");
    }
}

/* ---- dissolveEffect: the logic draws the order from the game's random source (DISSOLVE, in
 * DRAM: up to 320 x 200 entries); here, as the other effects so far, its last picture only:
 * every block copied once, so the whole main screen through drawStripToScreen (a width that
 * is a multiple of 4: the text composited), or straight to the screen (copyRectToScreen: no
 * text) for 1 x 1 and 1 x h.  Showing it block by block is the effects' step. ---- */
uint16_t *plat_dissolveBuffer(int n)
{
    if ((uint32_t)n * 2 > DISSOLVE_SIZE)
        plat_error("dissolveEffect: too many blocks");
    return (uint16_t *)DISSOLVE;
}

void plat_dissolve(int width, int height, const uint16_t *offsets, int n)
{
    VirtScreen *vs = &virtscr[kMainVirtScreen];
    const int rows = vs->topline + vs->h > 200 ? 200 - vs->topline : vs->h;
    (void)offsets;
    (void)n;
    (void)height;
    if ((width & 3) == 0) {
        drawStripToScreen(vs, 0, vs->w, 0, vs->h);
        return;
    }
    scr_begin();
    q_blit(scrHidden + mul16((uint32_t)vs->topline, 320), 320, getPixels(vs, 0, 0), 320,
           320, rows, B_COPY, 0);
    curAll = 1;                              /* the next sync: the whole screen */
}

/* ---- the costumes ---------------------------------------------------------------------
 * A cel (skipCelLines + byleRLEDecode) is the GPU's (src/gpu_room.s gq_cel): the 68000 fills a
 * block of CELBUF with what the renderer needs, the addresses worked out (compData.destPtr:
 * _out.getBasePtr(x, y) of the main screen; getMaskBuffer(0, y, zbuf)), and queues GQ_CEL.  A
 * block is reused once the GPU has done the command that read it (celMark). */
static uint32_t celNext, celMark[CEL_RING];       /* gq_head() after the block's GQ_CEL */

static inline int32_t ms16(int32_t a, int32_t b)          /* a 68000 muls.w */
{
    return (int32_t)(int16_t)a * (int16_t)b;
}

static uint32_t cel_block;
static int cel_block_free(void)
{
    return gq_passed(celMark[cel_block]);
}

void plat_drawCel(const CelDraw *c)
{
    volatile uint32_t *b, *cmd;
    const uint32_t *pal = (const uint32_t *)c->palette;
    const int xstart = g.mainXStart;
    const int z = c->zbuf < g.numZBuffer ? c->zbuf : g.numZBuffer - 1;   /* imgBufOffs */
    int i;

    cel_block = celNext;
    jag_wait_until(cel_block_free, "costume: the GPU does not take the cel blocks");
    b = (volatile uint32_t *)(CELBUF + cel_block * (CEL_LONGS * 4));
    b[CEL_SRC] = (uint32_t)c->src;
    b[CEL_SKIPN] = c->skipLines ? mul16((uint32_t)c->skipLines, (uint32_t)c->height) : 0;
    b[CEL_DEST] = (uint32_t)((int32_t)FRONTBUF + xstart + ms16(c->y, 320) + c->x);
    b[CEL_MROW] = (uint32_t)((int32_t)MASKBUF + (int32_t)mul16((uint32_t)z, maskItem) +
                             xstart / 8 + ms16(c->y, 40));
    b[CEL_X] = (uint32_t)c->x;
    b[CEL_Y] = (uint32_t)c->y;
    b[CEL_H] = (uint32_t)c->height;
    b[CEL_SKIPW] = (uint32_t)c->skipWidth;
    b[CEL_SCALEX] = c->scaleX == 255 ? 0xFFFFFFFFUL : (uint32_t)c->scaleX;
    b[CEL_SCALEY] = c->scaleY == 255 ? 0xFFFFFFFFUL : (uint32_t)c->scaleY;
    b[CEL_SXI] = (uint32_t)c->scaleXIndex;
    b[CEL_SYI] = (uint32_t)c->scaleYIndex;
    b[CEL_STEP] = (uint32_t)c->scaleXStep;
    b[CEL_CMASK] = (uint32_t)c->colorMask;
    b[CEL_CSHR] = (uint32_t)c->colorShr;
    b[CEL_BR] = (uint32_t)c->boundsRight;
    b[CEL_BB] = (uint32_t)c->boundsBottom;
    for (i = 0; i < 8; i++)
        b[CEL_PAL + i] = pal[i];
    b[CEL_TABLE] = (uint32_t)smallCostumeScaleTable;
    cmd = slot();
    cmd[0] = GQ_CEL;
    cmd[1] = (uint32_t)b;
    gq_commit();
    celMark[cel_block] = gq_head();
    celNext = (celNext + 1) & (CEL_RING - 1);
}


/* printCharIntern + CharsetRendererClassic::drawBitsN: one glyph, the GPU's (GQ_CHAR).  No
 * clipping in x (printChar has refused a glyph past either side); in y the rows are clipped
 * here as drawBitsN does: rows while yy + drawTop < the surface's height, written from
 * yy + drawTop >= 0.  The colour map travels in the command (it changes between strings). */
void plat_drawChar(int virt, int toText, int x, int y, int drawTop, const uint8_t *glyph,
                   int bpp, int width, int height, const uint8_t *cmap)
{
    VirtScreen *vs = &virtscr[virt];
    volatile uint32_t *c;
    uint32_t dst;
    int sh, rows, skip;

    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8)
        plat_error("drawBitsN: bpp");
    if (width <= 0 || width > 255 || height <= 0)
        return;
    if (toText) {
        dst = TEXTSURF + (uint32_t)(ms16(y, 320) + x);
        sh = 200;
    } else {
        dst = getPixels(vs, x, 0) + (uint32_t)ms16(y, 320);
        sh = vs->h;
    }
    rows = sh - drawTop < height ? sh - drawTop : height;
    if (rows <= 0)
        return;
    skip = drawTop < 0 ? -drawTop : 0;
    if (skip > rows)
        skip = rows;
    c = slot();
    c[0] = GQ_CHAR;
    c[1] = (uint32_t)glyph;
    c[2] = dst;
    c[3] = (uint32_t)width | ((uint32_t)rows << 8) | ((uint32_t)skip << 16) | ((uint32_t)bpp << 24);
    c[4] = rd32be(cmap);
    c[5] = rd32be(cmap + 4);
    c[6] = rd32be(cmap + 8);
    c[7] = rd32be(cmap + 12);
    gq_commit();
    if (toText)
        textClean = 0;
}

/* ---- the cursor: an OP sprite over the screen (src/main.s SetView), as ScummVM's backend
 * draws it over the system screen: the image plat_setCursor gave (0xFF transparent), when
 * plat_showMouse says so, where the mouse is (MOUSE_X/Y, every frame: the ISR places it at
 * the mouse minus the hotspot handed over with the image).  At the hand-over the image goes
 * into a CURSORn buffer the OP does not show, as the OP wants it (0 transparent: the cursor's
 * colours are 15, 7 and 8, never 0, cursor.c), by the Blitter: a fill with 0, then the image
 * copied over it with DCOMPEN + DSTEN against 0xFF (as the text surface).  Right of the screen
 * the EDGE object covers it.  The image is read from the logic's buffer by the GPU: it is
 * rewritten (setBuiltinCursor) only after the next hand-over has waited for this one. */
static const uint8_t *curSrc;               /* the image plat_setCursor was given */
static int curW, curH, curHotX, curHotY, curShown;
static int curNew;                          /* a new image (or shown) since the hand-over */
static uint32_t curCrc;                     /* the trace phase: image, size, hotspot */
static uint32_t curBuf;                     /* the CURSORn the last hand-over gave */

/* force 0: whether the cursor must be handed over; 1: hand it over (NEXT_CUR*) */
static int cursor_hand(int force)
{
    if (!force)
        return curNew;
    if (!curShown || !curSrc)
        HW32(NEXT_CUR) = 0;
    else {
        const uint32_t view = HW32(VIEW_CUR);
        uint32_t b = CURSOR0;
        if (b == view || b == curBuf)
            b = CURSOR1;
        if (b == view || b == curBuf)
            b = CURSOR2;
        q_blit(b, 16, 0, 0, 16, 16, B_FILL, 0);
        q_blit(b, 16, (uint32_t)curSrc, (uint32_t)curW, curW, curH, B_TEXT, 0xFF);
        curBuf = b;
        HW32(NEXT_CUR) = b;
    }
    HW32(NEXT_CURHX) = (uint32_t)curHotX;
    HW32(NEXT_CURHY) = (uint32_t)curHotY;
    curNew = 0;
    return 1;
}

void plat_setCursor(const uint8_t *img, int w, int h, int hotX, int hotY)
{
    if (w <= 0 || h <= 0 || w > 16 || h > 16)
        plat_error("setCursor: not 16 x 16 at most");
    curSrc = img;
    curW = w;
    curH = h;
    curHotX = hotX;
    curHotY = hotY;
    curNew = 1;
    if (traceChecks & 32) {
        uint8_t t[4];
        t[0] = (uint8_t)w; t[1] = (uint8_t)h; t[2] = (uint8_t)hotX; t[3] = (uint8_t)hotY;
        curCrc = jag_crc_more(jag_crc_more(0xFFFFFFFFUL, img, mul16((uint32_t)w, (uint32_t)h)), t, 4);
    }
}

/* CursorMan.showMouse, the last thing of a loop round: the trace phase logs "cur ROUND crc"
 * (the image, its size and hotspot, shown, the mouse) when it changed, as scumm_pc --curcrc */
void plat_showMouse(int show)
{
    static uint32_t last;
    if (show != curShown)
        curNew = 1;
    curShown = show;
    if (traceChecks & 32) {
        uint8_t t[5];
        int mx, my;
        uint32_t c;
        scumm_mousePos(&mx, &my);
        t[0] = (uint8_t)(show != 0);
        t[1] = (uint8_t)(mx >> 8); t[2] = (uint8_t)mx;
        t[3] = (uint8_t)(my >> 8); t[4] = (uint8_t)my;
        c = ~jag_crc_more(curCrc, t, 5);
        if (c != last)
            jag_logf("cur %u crc %x", (unsigned)(presentRound - 1), (unsigned)c);
        last = c;
    }
}

void plat_room(void)
{
}

/* ---- a start: the trace phase, then the time phase, each from scumm_init ------------------ */
void gfx_start(int trace)
{
    int i;
    traceChecks = trace ? (int)CHECKS : 0;
    flags320 = pitch_flags(320);             /* $21 << 9: (4 + 1) << (8 - 2) */
    presentRound = 0;
    palDirty = 0;
    tabValid = 0;                            /* scumm_init starts roomPaletteGen again */
    lastBb = lastZ = lastFb = lastSc = 0;
    roomW = roomH = roomP = roomFlags = maskUsed = 0;
    roomP = 1;                               /* no room: matches no pitch */
    /* the palette and both system screens black, as ScummVM's start; nothing shown yet */
    memset(clutStage, 0, sizeof(clutStage));
    memset(palRGB, 0, sizeof(palRGB));
    gq_drain();
    jag_wait_until(handover_taken, "hand-over: never taken");
    {
        volatile uint16_t *clut = (volatile uint16_t *)ROOM_CLUT;
        for (i = 0; i < 256; i++)
            clut[i] = 0;
    }
    HW32(NEXT_CLUT) = ROOM_CLUT;
    HW32(NEXT_BUF) = 0;
    HW32(NEXT_CUR) = 0;
    HW32(NEXT_FENCE) = gq_head();
    HW32(NEXT_SEQ) = HW32(NEXT_SEQ) + 1;
    jag_wait_until(handover_taken, "hand-over: never taken");
    curN = dueN = curAll = dueAll = pendFrom = 0;
    q_blit(SCREEN0, 320, 0, 0, 320, 200, B_FILL, 0);
    q_blit(SCREEN1, 320, 0, 0, 320, 200, B_FILL, 0);
    scrHidden = SCREEN0;
    scrShown = 0;
    scrSynced = 1;
    text_clear();
    curSrc = 0;
    curShown = curNew = 0;
    curBuf = 0;
    curCrc = 0xFFFFFFFFUL;
    gq_drain();
    for (i = 0; i < (int)CEL_RING; i++)      /* every cel block free */
        celMark[i] = gq_head();
    celNext = 0;
    memset(blitStat, 0, sizeof(blitStat));
}

/* clearTextSurface (a load) */
void plat_clearTextSurface(void)
{
    text_clear();
}
