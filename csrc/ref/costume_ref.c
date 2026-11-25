/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * costume_ref.c - the reference renderer's costume pixels (PC only).
 *
 * ScummVM base-costume.cpp: BaseCostumeRenderer::skipCelLines and byleRLEDecode (the
 * unoptimised version, the classic renderer's path: no AKOS shadow modes, no hit test), with
 * the v4 horizontal scale step (always +1: "From MONKEY1 EGA disasm").  costume.c in the logic
 * works out the cel's place, scale and z-plane (paintCelByleRLECommon) and hands them over in
 * a CelDraw; this file decodes the run-length data into the main virtual screen's front buffer,
 * masked by the z-plane.  v4 has no shadow table: costume.c stops on shadow mode 0x20.
 */
#include "../scumm/scumm.h"
#include "ref.h"

#define NUM_STRIPS 40

static uint8_t revBitMask(int x)
{
    return (uint8_t)(0x80 >> x);
}

void plat_drawCel(const CelDraw *c)
{
    const uint8_t *src = c->src;
    const uint8_t *scaleTable = smallCostumeScaleTable;
    const int pitch = ref_mainPitch();
    uint8_t len = 0, repLen = 0;
    uint16_t color, repColor = 0;
    int x = c->x, skipWidth = c->skipWidth, scaleXIndex = c->scaleXIndex;
    int y, scaleIndexY;
    uint16_t height;
    uint8_t *destPtr, *dst, maskbit;
    const uint8_t *maskPtr, *mask;

    if (ref_noCels)
        return;
    /* skipCelLines: the columns cut off by the screen edge, as a run count */
    if (c->skipLines) {
        int num = c->skipLines * c->height;
        for (;;) {
            repLen = *src++;
            repColor = (uint16_t)(repLen >> c->colorShr);
            repLen &= (uint8_t)c->colorMask;
            if (!repLen)
                repLen = *src++;
            if ((num -= repLen) <= 0) {
                repLen = (uint8_t)(1 - num);
                break;
            }
        }
    }

    destPtr = ref_frontBuffer(c->x, c->y);       /* compData.destPtr = _out.getBasePtr(x, y) */
    maskPtr = ref_getMaskBuffer(0, c->y, c->zbuf);   /* getMaskBuffer(0, compData.y, _zbuf) */

    dst = destPtr;
    len = repLen;
    color = repColor;
    y = c->y;
    height = (uint16_t)c->height;
    scaleIndexY = c->scaleYIndex;
    maskbit = revBitMask(x & 7);
    mask = maskPtr + x / 8;

    if (len)
        goto StartPos;
    do {
        len = *src++;
        color = (uint16_t)(len >> c->colorShr);
        len &= (uint8_t)c->colorMask;
        if (!len)
            len = *src++;
        do {
            if (c->scaleY == 255 || scaleTable[scaleIndexY++ & 0xFF] < c->scaleY) {
                const int masked = (y < 0 || y >= c->boundsBottom)
                    || (x < 0 || x >= c->boundsRight)
                    || (*mask & maskbit);
                if (color && !masked)
                    *dst = c->palette[color];
                dst += pitch;
                mask += NUM_STRIPS;
                y++;
            }
            if (!--height) {
                if (!--skipWidth)
                    return;
                height = (uint16_t)c->height;
                y = c->y;
                scaleIndexY = c->scaleYIndex;
                if (c->scaleX == 255 || scaleTable[scaleXIndex] < c->scaleX) {
                    x += c->scaleXStep;
                    if (x < 0 || x >= c->boundsRight)
                        return;
                    maskbit = revBitMask(x & 7);
                    destPtr += c->scaleXStep;
                }
                scaleXIndex = (scaleXIndex + 1) & 0xFF;      /* v4: always +1 */
                dst = destPtr;
                mask = maskPtr + x / 8;
            }
        StartPos:;
        } while (--len);
    } while (1);
}
