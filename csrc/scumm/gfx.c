/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * gfx.c - the drawing bookkeeping the game logic can see: which strips of the screen each
 * actor covers and which strips were redrawn this frame (the gfx usage bits), and the redraw
 * flags they set.
 *
 * ScummVM usage_bits.cpp (set/clear/test), gfx.cpp markRectAsDirty (the bits), redrawBGAreas
 * and redrawBGStrip (v4: which strips; the pixels are the platform's), object.cpp
 * drawRoomObjects, drawRoomObject, drawObject (the strip loop), processDrawQue,
 * addObjectToDrawQue, clearDrawObjectQueue, markObjectRectAsDirty; actor.cpp
 * setActorRedrawFlags, resetActorBgs, processActors (the sort and the draw order).
 *
 * Why the logic needs it: an actor that shares a strip with another actor or with a redrawn
 * strip is redrawn, a redraw recomputes its scale (and so its speed) from its box, and
 * o5_actorFromPos finds actors by the strips they were drawn on.
 *
 * The pixels go to the platform at the points where ScummVM touches them, with ScummVM's own
 * arguments: Gdi::drawBitmap (redrawBGStrip, drawObject), Gdi::resetBackground
 * (resetActorBgs), markRectAsDirty's virtual-screen part, fadeIn and fadeOut.  On the PC the
 * reference renderer (csrc/ref/) is ScummVM's; on the Jaguar the GPU, Blitter and OP.
 */
#include <string.h>
#include "scumm.h"

#define NUM_STRIP_BITS 410                  /* gfxUsageBits[410 * 3] */

static uint32_t gfxUsageBits[NUM_STRIP_BITS * 3];
/* the actors' bits all sit in a strip's first word (setActorRedrawFlags, resetActorBgs) */
typedef char actor_bits_fit_one_word[(NUM_ACTORS - 1 <= 32) ? 1 : -1];
static uint8_t drawObjectQue[200];
static int drawObjectQueNr;
static int bgNeedsRedraw;

void gfx_clearUsageBits(void)
{
    memset(gfxUsageBits, 0, sizeof(gfxUsageBits));
}

void gfx_reset(void)
{
    memset(gfxUsageBits, 0, sizeof(gfxUsageBits));
    drawObjectQueNr = 0;
    bgNeedsRedraw = 0;
}

static void setGfxUsageBit(int strip, int bit)
{
    if (strip < 0 || strip >= NUM_STRIP_BITS || bit < 1 || bit > 96)
        plat_error("setGfxUsageBit: out of range");
    bit--;
    gfxUsageBits[3 * strip + bit / 32] |= (1u << (bit % 32));
}

int gfx_testUsageBit(int strip, int bit)
{
    if (strip < 0 || strip >= NUM_STRIP_BITS || bit < 1 || bit > 96)
        plat_error("testGfxUsageBit: out of range");
    bit--;
    return (gfxUsageBits[3 * strip + bit / 32] & (1u << (bit % 32))) != 0;
}

/* testGfxAnyUsageBits: any actor bit (DIRTY and RESTORED excluded) */
int gfx_testAnyUsageBits(int strip)
{
    static const uint32_t bitmask[3] = { 0xFFFFFFFF, 0xFFFFFFFF, 0x3FFFFFFF };
    int i;
    if (strip < 0 || strip >= NUM_STRIP_BITS)
        plat_error("testGfxAnyUsageBits: out of range");
    for (i = 0; i < 3; i++)
        if (gfxUsageBits[3 * strip + i] & bitmask[i])
            return 1;
    return 0;
}

/* markRectAsDirty: the usage bits (main screen, an actor's bit), then the virtual screen's
 * dirty strips, which are the platform's (the part ScummVM once called setVirtscreenDirty) */
void markRectAsDirty(int virt, int left, int right, int top, int bottom, int dirtybit)
{
    int lp, rp, h = scumm_virtscrHeight(virt);

    if (left > right || top > bottom)
        return;
    if (top > h || bottom < 0)
        return;
    if (top < 0)
        top = 0;
    if (bottom > h)
        bottom = h;
    if (virt == 0 && dirtybit) {
        lp = left / 8 + g.screenStartStrip;
        if (lp < 0)
            lp = 0;
        rp = (right + g.mainXStart) / 8;
        if (rp >= 200)
            rp = 200;
        for (; lp <= rp; lp++)
            setGfxUsageBit(lp, dirtybit);
    }
    plat_markRectAsDirty(virt, left, right, top, bottom);
}

/* restoreBackground: the rectangle (screen coordinates, right and bottom exclusive) gets
 * the background back, or the colour where there is none.  The virtual screen is found and
 * the rectangle clipped and marked here (USAGE_BIT_RESTORED on the main screen's strips);
 * the pixels are the platform's. */
void restoreBackground(int left, int top, int right, int bottom, int backColor)
{
    int virt, vtop;

    if (top < 0)
        top = 0;
    if (left >= right || top >= bottom)
        return;
    if ((virt = scumm_findVirtScreen(top)) < 0)
        return;
    if (left > g.screenWidth)
        return;
    vtop = scumm_virtscrTop(virt);
    top -= vtop;
    bottom -= vtop;
    /* Common::Rect::clip(vs->w, vs->h) */
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > g.screenWidth) right = g.screenWidth;
    if (bottom > scumm_virtscrHeight(virt)) bottom = scumm_virtscrHeight(virt);
    if (right < left) right = left;
    if (bottom < top) bottom = top;

    markRectAsDirty(virt, left, right, top, bottom, USAGE_BIT_RESTORED);
    if (bottom - top == 0)
        return;
    plat_restoreBackground(virt, left, top, right, bottom, backColor);
}

/* drawPixel (gfx.cpp): wScale 1 (no text surface multiplier), not the back buffer */
static void drawPixel(int virt, int x, int y, int color)
{
    if (x >= 0 && y >= 0 && g.screenWidth + 8 > x && g.screenHeight > y) {
        const int vy = y - scumm_virtscrTop(virt);         /* + _screenTop, 0 before v7 */
        plat_drawPixel(virt, x, vy, color);
        markRectAsDirty(virt, x, x + 1, vy, vy + 1, 0);
    }
}

/* drawLine (gfx.cpp), DOS: a colour given (the banner's); -1 alternates white and black, from
 * the current palette (getPaletteColorFromRGB: not used by MI1, so not transcribed) */
void drawLine(int x1, int y1, int x2, int y2, int color)
{
    int effX1, effY1, width, height, widthAccumulator, heightAccumulator, horizontalStrips;
    int originalHeight, canDrawPixel, virt;

    if ((virt = scumm_findVirtScreen(y1)) < 0)
        return;
    if (color == -1) {
        plat_error("drawLine: no colour (getPaletteColorFromRGB)");
        return;
    }
    effX1 = x1;
    effY1 = y1;
    width = x2 - x1 < 0 ? x1 - x2 : x2 - x1;
    height = y2 - y1 < 0 ? y1 - y2 : y2 - y1;
    originalHeight = height;
    if (height <= width)
        height = width;
    widthAccumulator = 0;
    heightAccumulator = 0;
    drawPixel(virt, x1, y1, color);
    if (height >= 0) {
        horizontalStrips = height + 1;
        do {
            widthAccumulator += width;
            canDrawPixel = 0;
            heightAccumulator += originalHeight;
            if (widthAccumulator > height) {
                canDrawPixel = 1;
                widthAccumulator -= height;
                effX1 += x2 - x1 < 0 ? -1 : 1;
            }
            if (heightAccumulator > height) {
                canDrawPixel = 1;
                heightAccumulator -= height;
                effY1 += y2 - y1 < 0 ? -1 : 1;
            }
            if (canDrawPixel)
                drawPixel(virt, effX1, effY1, color);
            horizontalStrips--;
        } while (horizontalStrips);
    }
}

void gfx_markActorRect(int left, int right, int top, int bottom, int dirtybit)
{
    markRectAsDirty(0, left, right, top, bottom, dirtybit);
}

/* redrawBGStrip: the strips are marked, and the room image drawn into them */
static void redrawBGStrip(int start, int num)
{
    int i, s = g.screenStartStrip + start;
    for (i = 0; i < num; i++)
        setGfxUsageBit(s + i, USAGE_BIT_DIRTY);
    if (g.roomImage)                         /* rooms 88 and 94 have none (world.c) */
        plat_drawBitmap(g.roomImage, s, 0, g.roomWidth, scumm_mainScreenHeight(), s, num, 0);
}

/* drawObject: the strips it touches, then its image into them.  ScummVM masks the height
 * (v < 7) on every draw. */
static void drawObject(int obj, int scrollType)
{
    ObjectData *od = &g.objs[obj];
    int xpos, ypos, width, height, a, tmp, x, numstrip;

    if (bgNeedsRedraw)
        scrollType = 0;
    if (od->obj_nr == 0)
        return;

    xpos = od->x_pos / 8;
    ypos = od->y_pos;
    od->height &= 0xFFF8;
    width = od->width / 8;
    height = od->height;
    if (width == 0 || xpos > g.screenEndStrip || xpos + width < g.screenStartStrip)
        return;
    /* getObjectImage(getOBIMFromObjectData(od), state): GF_SMALL_HEADER, the OBIM + 8, one
     * image whatever the state (flobjects: not yet) */
    if (!od->OBIMoffset)
        plat_error("drawObject: an object with width and no image");
    /* An OI block of 8 bytes holds no image (MI1: 333 objects, exits and hotspots, drawn
     * only in a state other than 0; the church's exits 819 / 820 among them).  ScummVM points 8 bytes in, at the next block, and its drawStrip's
     * assertRange("screen strip") stops there; the original drew nothing, as the port does.
     * Reached in part 4: High Street's script 200 lets a ghost through the church door
     * (scripts 25 / 26 open and close 447 and its twin 819); the player who walks in while
     * it is open enters the church with 819 in state 1 (scumm_pc --bootparam 8989
     * --state-of 819:1 shows it).  Found in the first play-through on hardware.  ScummVM
     * knows the crash as bug #1191 and puts 819 back to 0 after loading a save only
     * (saveload.cpp: "for unknown reasons ... in old save games"); the cause is the race
     * above, in play. */
    if (rd32(g.roomPtr + od->OBIMoffset) <= 8)
        return;

    x = 0xFFFF;
    for (a = numstrip = 0; a < width; a++) {
        tmp = xpos + a;
        if (tmp < g.screenStartStrip || g.screenEndStrip < tmp)
            continue;
        if (scrollType > 0 && g.screenStartStrip + scrollType <= tmp)
            continue;
        if (scrollType < 0 && tmp <= g.screenEndStrip + scrollType)
            continue;
        setGfxUsageBit(tmp, USAGE_BIT_DIRTY);
        if (tmp < x)
            x = tmp;
        numstrip++;
    }
    if (numstrip != 0) {
        g.drawingObject = od->obj_nr;        /* for a FATAL's message only */
        plat_drawBitmap(g.roomPtr + od->OBIMoffset + 8, x, ypos, width * 8, height, x - xpos,
                        numstrip, od->flags | DB_OBJECT_MODE);
        g.drawingObject = 0;
    }
}

/* drawRoomObject: an object is drawn when its parents' states allow it */
static void drawRoomObject(int i, int arg)
{
    ObjectData *od = &g.objs[i];
    uint8_t a;

    if (i < 1 || od->obj_nr < 1 || !od->state)
        return;
    do {
        a = od->parentstate;
        if (!od->parent) {
            drawObject(i, arg);
            break;
        }
        od = &g.objs[od->parent];
    } while ((od->state & 0xF) == a);
}

static void drawRoomObjects(int arg)
{
    int i;
    for (i = NUM_LOCAL_OBJECTS - 1; i > 0; i--)
        if (g.objs[i].obj_nr > 0 && (g.objs[i].state & 0xF))
            drawRoomObject(i, arg);
}

/* redrawBGAreas (v4).  stopTalk on a scroll stays in loop.c, where it was. */
static void redrawBGAreas(void)
{
    int i, diff, val = 0;

    if (!g.fullRedraw && bgNeedsRedraw) {
        for (i = 0; i != 40; i++)
            if (gfx_testUsageBit(g.screenStartStrip + i, USAGE_BIT_DIRTY))
                redrawBGStrip(i, 1);
    }

    diff = g.camera.cur_x - g.camera.last_x;
    if (!g.fullRedraw && diff == 8) {
        val = -1;
        redrawBGStrip(40 - 1, 1);            /* scrollLeft */
    } else if (!g.fullRedraw && diff == -8) {
        val = +1;
        redrawBGStrip(0, 1);                 /* scrollRight */
    } else if (g.fullRedraw || diff != 0) {
        /* clearFlashlight: the flashlight is not used by this game */
        bgNeedsRedraw = 0;
        redrawBGStrip(0, 40);
    }

    drawRoomObjects(val);
    bgNeedsRedraw = 0;
}

/* scummLoop_handleDrawing */
void gfx_handleDrawing(void)
{
    int i;
    if (g.camera.cur_x != g.camera.last_x || bgNeedsRedraw || g.fullRedraw)
        redrawBGAreas();
    for (i = 0; i < drawObjectQueNr; i++)    /* processDrawQue */
        if (drawObjectQue[i])
            drawObject(drawObjectQue[i], 0);
    drawObjectQueNr = 0;
}

void addObjectToDrawQue(int object)
{
    if (drawObjectQueNr >= (int)sizeof(drawObjectQue))
        plat_error("Draw Object Que overflow");
    drawObjectQue[drawObjectQueNr++] = (uint8_t)object;
}

void clearDrawObjectQueue(void)
{
    drawObjectQueNr = 0;
}

int gfx_bgNeedsRedraw(void)
{
    return bgNeedsRedraw;
}

/* markObjectRectAsDirty: an object removed or changed: its strips are redrawn */
void markObjectRectAsDirty(int obj)
{
    int i, strip;
    for (i = 1; i < NUM_LOCAL_OBJECTS; i++) {
        if (g.objs[i].obj_nr == (uint16_t)obj) {
            if (g.objs[i].width != 0) {
                int minStrip = g.objs[i].x_pos / 8, maxStrip = g.objs[i].x_pos / 8 + g.objs[i].width / 8;
                if (minStrip < g.screenStartStrip)
                    minStrip = g.screenStartStrip;
                if (maxStrip > g.screenEndStrip + 1)
                    maxStrip = g.screenEndStrip + 1;
                for (strip = minStrip; strip < maxStrip; strip++)
                    setGfxUsageBit(strip, USAGE_BIT_DIRTY);
            }
            bgNeedsRedraw = 1;
            return;
        }
    }
}

/* --- the actors' side ------------------------------------------------------------------- */

/* setActorRedrawFlags: everyone on a full redraw, else an actor sharing a strip */
static void setActorRedrawFlags(void)
{
    int i, j;
    if (g.fullRedraw) {
        for (j = 1; j < NUM_ACTORS; j++)
            g.actors[j].needRedraw = 1;
    } else {
        /* Word by word: the actors' bits (1..12) are bits 0..11 of a strip's first word,
         * DIRTY and RESTORED the top two of its third.  For each actor bit set: is any
         * other bit of the strip set?  (testGfxAnyUsageBits, testGfxUsageBit,
         * testGfxOtherUsageBits, folded; the 68000 pays dearly for the calls.) */
        if (g.screenStartStrip < 0 || g.screenStartStrip + 40 > NUM_STRIP_BITS)
            plat_error("setActorRedrawFlags: strips out of range");
        for (i = 0; i < 40; i++) {
            const uint32_t *w = &gfxUsageBits[3 * (g.screenStartStrip + i)];
            uint32_t w0 = w[0], rest = w[1] | w[2], bits;
            if (!(w0 | w[1] | (w[2] & 0x3FFFFFFF)))
                continue;
            for (bits = w0, j = 1; bits && j < NUM_ACTORS; bits >>= 1, j++)
                if ((bits & 1) && ((w0 & ~(1u << (j - 1))) | rest))
                    g.actors[j].needRedraw = 1;
        }
    }
}

/* resetActorBgs: the strips of an actor about to be redrawn (or hidden) are freed */
static void resetActorBgs(void)
{
    int i, j;
    uint32_t freed = 0;
    /* Whether an actor's strips are freed does not depend on the strip: one mask, then two
     * ANDs per strip (DIRTY = bit 31, RESTORED = bit 30 of the third word). */
    for (j = 1; j < NUM_ACTORS; j++) {
        const Actor *a = &g.actors[j];
        if ((a->top != 0x7FFFFFFF && a->needRedraw) || a->needBgReset)
            freed |= 1u << (j - 1);
    }
    if (g.screenStartStrip < 0 || g.screenStartStrip + 40 > NUM_STRIP_BITS)
        plat_error("resetActorBgs: strips out of range");
    for (i = 0; i < 40; i++) {
        uint32_t *w = &gfxUsageBits[3 * (g.screenStartStrip + i)];
        uint32_t reset = w[0] & freed;
        w[2] &= ~0xC0000000u;                /* DIRTY, RESTORED */
        w[0] &= ~freed;
        for (j = 1; reset; j++, reset >>= 1) {
            const Actor *a = &g.actors[j];
            if ((reset & 1) && a->bottom - a->top >= 0)
                plat_resetBackground(a->top, a->bottom, i);
        }
    }
    for (i = 1; i < NUM_ACTORS; i++)
        g.actors[i].needBgReset = 0;
}

/* processActors: sorted by y (a bubble of swaps, as ScummVM), drawn, animated */
static void processActors(void)
{
    Actor *sorted[NUM_ACTORS];
    int n = 0, i, j;

    for (i = 1; i < NUM_ACTORS; i++)
        if (actor_inCurrentRoom(&g.actors[i]))
            sorted[n++] = &g.actors[i];
    if (!n)
        return;

    for (j = 0; j < n; ++j) {
        for (i = 0; i < n; ++i) {
            int sc1 = sorted[j]->y, sc2 = sorted[i]->y;     /* _layer is 0 before v7 */
            if (sc1 < sc2) {
                Actor *t = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = t;
            }
        }
    }
    for (i = 0; i < n; i++) {
        Actor *a = sorted[i];
        if (a->costume) {
            PROF(14);
            actor_drawCostume(a);
            PROF(15);
            actor_animateCostume(a);
            PROF(13);
        }
    }
}

/* ScummEngine_v5::scummLoop_handleActors (the flashlight is not used by this game) */
void gfx_handleActors(void)
{
    setActorRedrawFlags();
    PROF(12);
    resetActorBgs();
    PROF(13);
    processActors();
}

/* --- save and load ---------------------------------------------------------------------- */
void gfx_saveLoad(Ser *s)
{
    SER_ARR(s, gfxUsageBits);
    SER(s, bgNeedsRedraw);
}

void gfx_afterLoad(void)
{
    drawObjectQueNr = 0;
}
