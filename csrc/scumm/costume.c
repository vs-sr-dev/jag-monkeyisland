/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * costume.c - the classic costumes (formats 0x58/0x59): what an actor shows, as far as the
 * game logic can see it.
 *
 * ScummVM costume.cpp + base-costume.cpp: ClassicCostumeLoader (loadCostume,
 * costumeDecodeData, increaseAnims, increaseAnim), Actor::animateCostume,
 * Actor::drawActorCostume and prepareDrawActorCostume, BaseCostumeRenderer::drawCostume,
 * ClassicCostumeRenderer::drawLimb, paintCelByleRLE and paintCelByleRLECommon (the geometry),
 * markAsDirty; actor.cpp getActorFromPos and playActorSounds.
 *
 * Drawing reaches into the logic in three ways, and this file keeps exactly those:
 *   - the drawn rectangle: the actor's _top/_bottom and the strips it covers (gfx usage bits),
 *     which getActorFromPos (o5_actorFromPos) and the redraw flags read;
 *   - the draw result: an actor cut by the screen edge is redrawn next frame, and a redraw
 *     recomputes its scale from its box (setupActorScale), so the scale and speed follow it;
 *   - the animation commands: limbs advance (animateCostume), count (getAnimCounter) and ask
 *     for sounds (0x78 -> playActorSounds).
 * The pixels themselves (skipCelLines, byleRLEDecode) are the platform's: everything the
 * decoder needs (ScummVM's ByleRLEData and the renderer's palette, scale and z-plane) is
 * worked out here and handed over in a CelDraw (plat_drawCel).  On the PC the reference
 * renderer, on the Jaguar the GPU.
 */
#include <string.h>
#include "scumm.h"

/* costume.cpp: the scale table, a bit-reversed count, shared by width and height */
const uint8_t smallCostumeScaleTable[256] = {
    0xFF, 0xFD, 0x7D, 0xBD, 0x3D, 0xDD, 0x5D, 0x9D,
    0x1D, 0xED, 0x6D, 0xAD, 0x2D, 0xCD, 0x4D, 0x8D,
    0x0D, 0xF5, 0x75, 0xB5, 0x35, 0xD5, 0x55, 0x95,
    0x15, 0xE5, 0x65, 0xA5, 0x25, 0xC5, 0x45, 0x85,
    0x05, 0xF9, 0x79, 0xB9, 0x39, 0xD9, 0x59, 0x99,
    0x19, 0xE9, 0x69, 0xA9, 0x29, 0xC9, 0x49, 0x89,
    0x09, 0xF1, 0x71, 0xB1, 0x31, 0xD1, 0x51, 0x91,
    0x11, 0xE1, 0x61, 0xA1, 0x21, 0xC1, 0x41, 0x81,
    0x01, 0xFB, 0x7B, 0xBB, 0x3B, 0xDB, 0x5B, 0x9B,
    0x1B, 0xEB, 0x6B, 0xAB, 0x2B, 0xCB, 0x4B, 0x8B,
    0x0B, 0xF3, 0x73, 0xB3, 0x33, 0xD3, 0x53, 0x93,
    0x13, 0xE3, 0x63, 0xA3, 0x23, 0xC3, 0x43, 0x83,
    0x03, 0xF7, 0x77, 0xB7, 0x37, 0xD7, 0x57, 0x97,
    0x17, 0xE7, 0x67, 0xA7, 0x27, 0xC7, 0x47, 0x87,
    0x07, 0xEF, 0x6F, 0xAF, 0x2F, 0xCF, 0x4F, 0x8F,
    0x0F, 0xDF, 0x5F, 0x9F, 0x1F, 0xBF, 0x3F, 0x7F,
    0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0,
    0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
    0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8,
    0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
    0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4,
    0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
    0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC,
    0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
    0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2,
    0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
    0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA,
    0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
    0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6,
    0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
    0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE,
    0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE
};

/* --- the loader ----------------------------------------------------------------------- */

typedef struct {
    int id;
    const uint8_t *baseptr, *animCmds, *frameOffsets, *dataOffsets, *palette;
    int numAnim, format, mirror, numColors;
} CostumeLoader;

/* ClassicCostumeLoader::loadCostume, GF_SMALL_HEADER: the block itself (header included) */
static void loadCostume(CostumeLoader *L, int id)
{
    const uint8_t *ptr = res_costume(id);
    if (!ptr)
        plat_error("loadCostume: no such costume");
    L->id = id;
    L->baseptr = ptr;
    L->numAnim = ptr[6];
    L->format = ptr[7] & 0x7F;
    L->mirror = (ptr[7] & 0x80) != 0;
    L->palette = ptr + 8;
    switch (L->format) {
    case 0x58: L->numColors = 16; break;
    case 0x59: L->numColors = 32; break;
    default:
        plat_error("Costume with an invalid format");
    }
    ptr += 8 + L->numColors;
    L->frameOffsets = ptr + 2;
    L->dataOffsets = ptr + 34;
    L->animCmds = L->baseptr + rd16(ptr);
}

static CostumeLoader loader;

void costume_reset(CostumeData *c)
{
    c->animCounter = 0;
    c->soundCounter = 0;
    c->stopped = 0;
    memset(c->curpos, 0xFF, sizeof(c->curpos));
    memset(c->start, 0xFF, sizeof(c->start));
    memset(c->end, 0xFF, sizeof(c->end));
    memset(c->frame, 0xFF, sizeof(c->frame));
}

/* ClassicCostumeLoader::costumeDecodeData: start the limbs of animation 'frame' (in the
 * actor's direction) that usemask selects. */
void costumeDecodeData(Actor *a, int frame, unsigned usemask)
{
    const uint8_t *r;
    unsigned mask, j;
    int i, anim;
    uint8_t extra, cmd;

    loadCostume(&loader, a->costume);

    anim = newDirToOldDir(a->facing) + frame * 4;
    if (anim > loader.numAnim)
        return;

    r = loader.baseptr + rd16(loader.dataOffsets + anim * 2);
    if (r == loader.baseptr)
        return;

    mask = rd16(r);
    r += 2;
    i = 0;
    do {
        if (mask & 0x8000) {
            j = rd16(r);                     /* v4: 16-bit limb starts */
            r += 2;
            if (usemask & 0x8000) {
                if (j == 0xFFFF) {
                    a->cost.curpos[i] = 0xFFFF;
                    a->cost.start[i] = 0;
                    a->cost.frame[i] = (uint16_t)anim;
                } else {
                    extra = *r++;
                    cmd = loader.animCmds[j];
                    if (cmd == 0x7A) {
                        a->cost.stopped &= (uint16_t)~(1 << i);
                    } else if (cmd == 0x79) {
                        a->cost.stopped |= (uint16_t)(1 << i);
                    } else {
                        a->cost.curpos[i] = a->cost.start[i] = (uint16_t)j;
                        a->cost.end[i] = (uint16_t)(j + (extra & 0x7F));
                        if (extra & 0x80)
                            a->cost.curpos[i] |= 0x8000;
                        a->cost.frame[i] = (uint16_t)anim;
                    }
                }
            } else {
                if (j != 0xFFFF)
                    r++;
            }
        }
        i++;
        usemask <<= 1;
        mask <<= 1;
    } while (mask & 0xFFFF);
}

/* ClassicCostumeLoader::increaseAnim: one step of a limb; 1 when its picture changed */
static int increaseAnim(Actor *a, int slot)
{
    int highflag, i, end;
    uint8_t code, nc;

    if (a->cost.curpos[slot] == 0xFFFF)
        return 0;

    highflag = a->cost.curpos[slot] & 0x8000;
    i = a->cost.curpos[slot] & 0x7FFF;
    end = a->cost.end[slot];
    code = loader.animCmds[i] & 0x7F;

    for (;;) {
        if (!highflag) {
            if (i++ >= end)
                i = a->cost.start[slot];
        } else {
            if (i != end)
                i++;
        }
        nc = loader.animCmds[i];

        if (nc == 0x7C) {
            a->cost.animCounter++;
            if (a->cost.start[slot] != end)
                continue;
        } else if (nc == 0x78) {
            a->cost.soundCounter++;
            if (a->cost.start[slot] != end)
                continue;
        }
        a->cost.curpos[slot] = (uint16_t)(i | highflag);
        return (loader.animCmds[i] & 0x7F) != code;
    }
}

/* Actor::animateCostume: every animSpeed+1 frames the limbs advance */
void actor_animateCostume(Actor *a)
{
    int i, r = 0;
    if (a->costume == 0)
        return;
    a->animProgress++;
    if (a->animProgress >= a->animSpeed) {
        a->animProgress = 0;
        loadCostume(&loader, a->costume);
        for (i = 0; i != 16; i++)            /* increaseAnims */
            if (a->cost.curpos[i] != 0xFFFF)
                r |= increaseAnim(a, i);
        if (r)
            a->needRedraw = 1;
    }
}

/* --- the renderer, its geometry ---------------------------------------------------------- */

static struct {
    int actorID, actorX, actorY;
    int scaleX, scaleY;
    int drawActorToRight;
    int width, height;
    int xMove, yMove;
    int32_t drawTop, drawBottom;
    int outW, outH;                          /* _out: the main virtual screen, pitch 320 (v < 7), h */
    int zbuf;
    const uint8_t *srcPtr;                   /* the cel's data, after its CostumeInfo */
    uint8_t palette[32];                     /* ClassicCostumeRenderer::_palette */
} R;

/* markRectAsDirty for an actor (kMainVirtScreen, dirtybit = the actor) */
static void markAsDirty(int left, int right, int top, int bottom)
{
    gfx_markActorRect(left, right + 1, top, bottom, R.actorID);
}

/* paintCelByleRLECommon (scaletableSize 128, scaleIndexMask 0xFF): where the cel lands.
 * Returns the draw flag (1: cut by the edge, redraw next frame; 2: whole; 0: nothing). */
static int paintCel(int xMoveCur, int yMoveCur)
{
    int actorIsScaled, i, j;
    int linesToSkip = 0, startScaleIndexX, startScaleIndexY, step, skipLines = 0;
    int16_t rl, rr, rt, rb;                  /* Common::Rect: int16 */
    int cx = R.actorX, cy = R.actorY;        /* compData.x, compData.y */
    int bl = 0, bt = 0, br = R.outW, bb = R.outH;
    int skipWidth, scaleXStep;
    int drawFlag = 1;
    const int scaletableSize = 128, scaleIndexMask = 0xFF;
    const uint8_t *scaleTable = smallCostumeScaleTable;

    actorIsScaled = (R.scaleX != 0xFF) || (R.scaleY != 0xFF);

    if (actorIsScaled) {
        scaleXStep = -1;
        if (xMoveCur < 0) {
            xMoveCur = -xMoveCur;
            scaleXStep = 1;
        }
        if (R.drawActorToRight) {
            startScaleIndexX = j = (scaletableSize - xMoveCur) & scaleIndexMask;
            for (i = 0; i < xMoveCur; i++)
                if (scaleTable[j++ & scaleIndexMask] < R.scaleX)
                    cx -= scaleXStep;
            rl = rr = (int16_t)cx;
            j = startScaleIndexX;
            for (i = 0; i < R.width; i++) {
                if (rr < 0) {
                    linesToSkip++;
                    startScaleIndexX = j;
                }
                if (scaleTable[j++ & scaleIndexMask] < R.scaleX)
                    rr++;
            }
        } else {
            startScaleIndexX = j = (scaletableSize + xMoveCur) & scaleIndexMask;
            for (i = 0; i < xMoveCur; i++)
                if (scaleTable[j-- & scaleIndexMask] < R.scaleX)
                    cx += scaleXStep;
            rl = rr = (int16_t)cx;
            j = startScaleIndexX;
            for (i = 0; i < R.width; i++) {
                if (rl >= br) {
                    startScaleIndexX = j;
                    linesToSkip++;
                }
                if (scaleTable[j-- & scaleIndexMask] < R.scaleX)
                    rl--;
            }
        }
        if (linesToSkip)
            linesToSkip--;

        step = -1;
        if (yMoveCur < 0) {
            yMoveCur = -yMoveCur;
            step = -step;
        }
        startScaleIndexY = j = (scaletableSize - yMoveCur) & scaleIndexMask;
        for (i = 0; i < yMoveCur; i++)
            if (scaleTable[j++ & scaleIndexMask] < R.scaleY)
                cy -= step;
        rt = rb = (int16_t)cy;
        j = startScaleIndexY;
        for (i = 0; i < R.height; i++)
            if (scaleTable[j++ & scaleIndexMask] < R.scaleY)
                rb++;
    } else {
        if (!R.drawActorToRight)
            xMoveCur = -xMoveCur;
        cx += xMoveCur;
        cy += yMoveCur;
        if (R.drawActorToRight) {
            rl = (int16_t)cx;
            rr = (int16_t)(cx + R.width);
        } else {
            rl = (int16_t)(cx - R.width);
            rr = (int16_t)cx;
        }
        rt = (int16_t)cy;
        rb = (int16_t)(rt + R.height);
        startScaleIndexX = scaletableSize;
        startScaleIndexY = scaletableSize;
    }

    skipWidth = R.width;

    /* clipBoundsRect: nothing for the classic renderer */
    markAsDirty(rl, rr, rt, rb);

    if (rt >= bb || rb <= bt)
        return 0;
    if (rl >= br || rr <= bl)
        return 0;

    if (R.drawActorToRight) {
        if (!actorIsScaled)
            linesToSkip = bl - cx;
        if (linesToSkip > 0) {
            skipWidth -= linesToSkip;
            skipLines = linesToSkip;         /* skipCelLines: the decoder's */
            cx = bl;
        } else {
            linesToSkip = rr - br;
            if (linesToSkip <= 0)
                drawFlag = 2;
            else
                skipWidth -= linesToSkip;
        }
    } else {
        if (!actorIsScaled)
            linesToSkip = rr - br;
        if (linesToSkip > 0) {
            skipWidth -= linesToSkip;
            skipLines = linesToSkip;
            cx = br - 1;
        } else {
            linesToSkip = (bl - 1) - rl;
            if (linesToSkip <= 0)
                drawFlag = 2;
            else
                skipWidth -= linesToSkip;
        }
    }

    if (skipWidth <= 0)
        return 0;

    if (rl < bl)
        rl = (int16_t)bl;
    if (rt < bt)
        rt = (int16_t)bt;
    if (rt > bb)
        rt = (int16_t)bb;
    if (rb > bb)
        rb = (int16_t)bb;

    if (R.drawTop > rt)
        R.drawTop = rt;
    if (R.drawBottom < rb)
        R.drawBottom = rb;

    if (R.height + rt >= 256)
        return 2;

    /* the pixels: compData as ScummVM has it at byleRLEDecode */
    {
        CelDraw c;
        c.src = R.srcPtr;
        c.skipLines = skipLines;
        c.x = cx;
        c.y = cy;
        c.width = R.width;
        c.height = R.height;
        c.skipWidth = skipWidth;
        c.scaleX = R.scaleX;
        c.scaleY = R.scaleY;
        c.scaleXIndex = startScaleIndexX;
        c.scaleYIndex = startScaleIndexY;
        c.scaleXStep = R.drawActorToRight ? 1 : -1;
        if (loader.numColors == 32) {
            c.colorMask = 7;
            c.colorShr = 3;
        } else {
            c.colorMask = 15;
            c.colorShr = 4;
        }
        c.boundsRight = br;
        c.boundsBottom = bb;
        c.zbuf = R.zbuf;
        c.actor = R.actorID;
        memcpy(c.palette, R.palette, sizeof(c.palette));
        plat_drawCel(&c);
    }
    return drawFlag;
}

/* ClassicCostumeRenderer::drawLimb */
static int drawLimb(const Actor *a, int limb)
{
    const CostumeData *cost = &a->cost;
    const uint8_t *frameptr, *src;
    int i, code, xmoveCur, ymoveCur;

    if (cost->curpos[limb] == 0xFFFF || (cost->stopped & (1 << limb)))
        return 0;

    i = cost->curpos[limb] & 0x7FFF;
    frameptr = loader.baseptr + rd16(loader.frameOffsets + limb * 2);
    code = loader.animCmds[i] & 0x7F;

    if (code != 0x7B) {
        src = loader.baseptr + rd16(frameptr + code * 2);
        R.width = rd16(src);                         /* CostumeInfo */
        R.height = rd16(src + 2);
        xmoveCur = R.xMove + (int16_t)rd16(src + 4);
        ymoveCur = R.yMove + (int16_t)rd16(src + 6);
        R.xMove += (int16_t)rd16(src + 8);
        R.yMove -= (int16_t)rd16(src + 10);
        R.srcPtr = src + 12;
        return paintCel(xmoveCur, ymoveCur);
    }
    return 0;
}

/* Actor::drawActorCostume: prepareDrawActorCostume, drawCostume, and what the logic keeps */
void actor_drawCostume(Actor *a)
{
    int i, result = 0;

    if (a->costume == 0)
        return;
    if (!a->needRedraw)
        return;
    a->needRedraw = 0;

    actor_setupScale(a);
    PROF(16);

    /* prepareDrawActorCostume */
    R.actorID = a->number;
    R.actorX = a->x - g.mainXStart;
    R.actorY = a->y - a->elevation;
    if (a->boxscale & 0x8000) {              /* v4: a scale slot */
        R.scaleX = R.scaleY = getScaleFromSlot((a->boxscale & 0x7FFF) + 1, a->x, a->y);
    } else {
        R.scaleX = a->scalex;
        R.scaleY = a->scaley;
    }
    if (a->shadowMode & 0x20)                /* v4 has no _shadowTable: ScummVM would crash */
        plat_error("drawActorCostume: shadow mode 0x20 without a shadow table");
    loadCostume(&loader, a->costume);        /* setCostume */
    /* setPalette: the actor's colours (255: the costume's own) when the lights say so */
    if (VAR(VAR_CURRENT_LIGHTS) & 8) {       /* LIGHTMODE_actor_use_colors */
        for (i = 0; i < loader.numColors; i++) {
            uint8_t color = a->palette[i];
            if (color == 255)
                color = loader.palette[i];
            R.palette[i] = color;
        }
    } else {
        memset(R.palette, 8, loader.numColors);
        R.palette[12] = 0;
    }
    R.drawActorToRight = newDirToOldDir(a->facing) != 0 || loader.mirror;   /* setFacing */
    /* the z-plane (v < 7) */
    if (a->forceClip)
        R.zbuf = a->forceClip;
    else if (getClass(a->number, 20))        /* kObjectClassNeverClip */
        R.zbuf = 0;
    else {
        R.zbuf = getMaskFromBox(a->walkbox);
        if (R.zbuf > g.numZBuffer - 1)
            R.zbuf = g.numZBuffer - 1;
    }
    R.drawTop = 0x7FFFFFFF;
    R.drawBottom = 0;

    /* drawCostume */
    R.actorX += g.mainXStart & 7;
    R.outW = 320;
    R.outH = scumm_mainScreenHeight();
    R.xMove = R.yMove = 0;
    PROF(17);
    for (i = 0; i < 16; i++)
        result |= drawLimb(a, i);

    if (result & 1)                          /* partly hidden: redraw it next frame */
        a->needRedraw = 1;
    a->top = R.drawTop;
    a->bottom = R.drawBottom;
}

/* --- what the scripts ask ------------------------------------------------------------- */

/* ScummEngine::getActorFromPos (v > 2) */
int getActorFromPos(int x, int y)
{
    int i;
    if (!gfx_testAnyUsageBits(x / 8))
        return 0;
    for (i = 1; i < NUM_ACTORS; i++) {
        int16_t y1 = (int16_t)g.actors[i].top;       /* int -> int16, as ScummVM */
        int16_t y2 = (int16_t)g.actors[i].bottom;
        if (gfx_testUsageBit(x / 8, i) && !getClass(i, 32) && y >= y1 && y <= y2)
            return i;
    }
    return 0;
}

/* ScummEngine::playActorSounds (v <= 5): the first actor in the room whose costume asked for
 * its sound plays it, and every counter is cleared. */
void playActorSounds(void)
{
    int i, j;
    for (i = 1; i < NUM_ACTORS; i++) {
        if (g.actors[i].cost.soundCounter && actor_inCurrentRoom(&g.actors[i])) {
            g.currentScript = 0xFF;
            sound_queue(g.actors[i].sound);
            for (j = 1; j < NUM_ACTORS; j++)
                g.actors[j].cost.soundCounter = 0;
            return;
        }
    }
}
