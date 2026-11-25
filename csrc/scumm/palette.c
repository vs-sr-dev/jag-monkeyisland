/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * palette.c - the palette as the game state holds it, and its effects.
 *
 * ScummVM palette.cpp: resetPalette (DOS VGA boot), setPaletteFromTable, setPaletteFromPtr,
 * setPalColor, setDirtyColors, setCurrentPalette, getPalettePtr, initCycl, stopCycle,
 * cyclePalette (the GF_SMALL_HEADER path), moveMemInPalRes, palManipulateInit (v5, from three
 * strings), palManipulate, setShadowPalette (the 7-argument form of SO_ROOM_SHADOW),
 * darkenPalette, updatePalette.
 *
 * The scripts never read a colour back, but the palette is the game's, not the platform's:
 * the room's colours, the cycles, the fades and the shadow remap are state, saved with the
 * game.  The platform only receives the result: updatePalette hands it the changed range
 * through plat_setPalette (on the PC the reference renderer, on the Jaguar the CLUT).
 *
 * v4 paths only, as ScummVM takes them for MI1 DOS VGA (GF_SMALL_HEADER, _shadowPalRemap);
 * where v5/v6 take another path, a comment says so.
 */
#include <string.h>
#include "scumm.h"

/* resetPalette: the EGA colours, what mode 13h offers a VGA game at boot (ScummVM #15869) */
static const uint8_t tableEGAPalette[16 * 3] = {
    0x00, 0x00, 0x00,  0x00, 0x00, 0xAA,  0x00, 0xAA, 0x00,  0x00, 0xAA, 0xAA,
    0xAA, 0x00, 0x00,  0xAA, 0x00, 0xAA,  0xAA, 0x55, 0x00,  0xAA, 0xAA, 0xAA,
    0x55, 0x55, 0x55,  0x55, 0x55, 0xFF,  0x55, 0xFF, 0x55,  0x55, 0xFF, 0xFF,
    0xFF, 0x55, 0x55,  0xFF, 0x55, 0xFF,  0xFF, 0xFF, 0x55,  0xFF, 0xFF, 0xFF
};

static int abs_i(int v) { return v < 0 ? -v : v; }

void setDirtyColors(int min, int max)
{
    if (g.palDirtyMin > min)
        g.palDirtyMin = min;
    if (g.palDirtyMax < max)
        g.palDirtyMax = max;
}

void setPalColor(int idx, int r, int gr, int b)
{
    g.currentPalette[idx * 3 + 0] = (uint8_t)r;
    g.currentPalette[idx * 3 + 1] = (uint8_t)gr;
    g.currentPalette[idx * 3 + 2] = (uint8_t)b;
    setDirtyColors(idx, idx);
}

/* resetPalette(isBootUp = true), DOS VGA v4: the EGA colours, the shadow palette the
 * identity (GID_MONKEY_VGA), then updatePalette */
void resetPalette(void)
{
    int i;
    g.palDirtyMin = 256;
    g.palDirtyMax = -1;
    for (i = 0; i < 16; i++)                     /* setPaletteFromTable */
        setPalColor(i, tableEGAPalette[i * 3], tableEGAPalette[i * 3 + 1], tableEGAPalette[i * 3 + 2]);
    for (i = 0; i < 256; i++)
        g.shadowPalette[i] = (uint8_t)i;
    setDirtyColors(0, 255);
    updatePalette();
}

/* setPaletteFromPtr(ptr, -1), GF_SMALL_HEADER: an LE16 byte count, then RGB 8-bit.
 * (v5/v6 keep 6-bit-looking entries >= 252 out; v4 copies every colour.) */
static void setPaletteFromPtr(const uint8_t *ptr)
{
    int numcolor = rd16(ptr) / 3, i;
    ptr += 2;
    if (numcolor < 0 || numcolor > 256)
        plat_error("setPaletteFromPtr: numcolor");
    for (i = 0; i < numcolor; i++) {
        g.currentPalette[i * 3 + 0] = *ptr++;
        g.currentPalette[i * 3 + 1] = *ptr++;
        g.currentPalette[i * 3 + 2] = *ptr++;
    }
    setDirtyColors(0, numcolor - 1);
}

/* getPalettePtr: the room's CLUT ("PA"), from its count field (v5 the same; v6 PALS) */
static const uint8_t *getPalettePtr(void)
{
    if (!g.roomPtr)
        plat_error("getPalettePtr: no room");
    return res_find_data(g.roomPtr, "PA");
}

/* setCurrentPalette */
void setCurrentPalette(int palindex)
{
    const uint8_t *pals;
    g.curPalIndex = palindex;
    pals = getPalettePtr();
    if (!pals)
        plat_error("setCurrentPalette: the room has no palette");
    setPaletteFromPtr(pals);
}

/* initCycl, GF_SMALL_HEADER: 16 entries of BE16 delay, start, end */
void initCycl(const uint8_t *ptr)
{
    int j;
    memset(g.colorCycle, 0, sizeof(g.colorCycle));
    for (j = 0; j < 16; ++j) {
        ColorCycle *cycl = &g.colorCycle[j];
        cycl->delay = (uint16_t)((ptr[0] << 8) | ptr[1]);
        ptr += 2;
        cycl->counter = 0;
        cycl->start = *ptr++;
        cycl->end = *ptr++;
        if (cycl->delay && cycl->delay != 0x0AAA)
            cycl->counter = cycl->start;
    }
}

/* resetRoomSubBlocks, the palette part: the CLUT (setCurrentPalette(0)) and the cycles */
void palette_resetRoom(void)
{
    const uint8_t *ptr;
    if (res_find_data(g.roomPtr, "PA"))
        setCurrentPalette(0);
    ptr = res_find_data(g.roomPtr, "CC");      /* CYCL */
    if (ptr)
        initCycl(ptr);
}

void stopCycle(int i)
{
    if (i < 0 || i > 16)
        plat_error("stopCycle: cycle");
    if (i != 0) {
        g.colorCycle[i - 1].delay = 0;
        return;
    }
    for (i = 0; i < 16; i++)
        g.colorCycle[i].delay = 0;
}

/* doCyclePalette: one step of [cycleStart, cycleEnd], entries of `size` bytes */
static void doCyclePalette(uint8_t *palette, int cycleStart, int cycleEnd, int size, int forward)
{
    uint8_t *start = palette + cycleStart * size;
    uint8_t *end = palette + cycleEnd * size;
    int num = cycleEnd - cycleStart;
    uint8_t tmp[6];

    if (forward) {
        memmove(tmp, end, size);
        memmove(start + size, start, num * size);
        memmove(start, tmp, size);
    } else {
        memmove(tmp, start, size);
        memmove(start, start + size, num * size);
        memmove(end, tmp, size);
    }
}

/* moveMemInPalRes: the cycles carried into a running palManipulate */
static void moveMemInPalRes(int start, int end, int direction)
{
    if (!g.palManipCounter)
        return;
    doCyclePalette(g.palManipPalette, start, end, 3, !direction);
    doCyclePalette((uint8_t *)g.palManipIntermediatePal, start, end, 6, !direction);
}

/* cyclePalette, GF_SMALL_HEADER ("verified from LOOM CD and MI1 VGA disasms"): every frame,
 * each cycle with a counter rotates the shadow palette over its range; the delay is not used.
 * (v5+: the cycles rotate _currentPalette by the delay against VAR_TIMER.) */
void cyclePalette(void)
{
    int i, j;
    for (i = 0; i < 16; i++) {
        ColorCycle *c = &g.colorCycle[i];
        if (c->counter) {
            uint8_t start, end;
            c->counter++;
            if (c->counter > c->end)
                c->counter = c->start;
            start = c->start;
            end = c->end;
            if (start <= end) {
                uint8_t cycleVal = (uint8_t)c->counter;
                for (j = start; j <= end; j++) {
                    g.shadowPalette[j] = cycleVal--;
                    if (cycleVal < start)
                        cycleVal = end;
                }
            }
            setDirtyColors(c->start, c->end);
            moveMemInPalRes(c->start, c->end, 0);
        }
    }
}

/* palManipulateInit (ScummEngine, v5: the target colours from three string resources) */
void palManipulateInit(int resID, int start, int end, int time)
{
    const uint8_t *string1, *string2, *string3, *pal;
    uint8_t *target;
    uint16_t *between;
    int i;

    if (resID < 0 || resID + 2 >= NUM_STRINGS)
        plat_error("palManipulateInit: string resources out of range");
    string1 = g.strings[resID] + start;
    string2 = g.strings[resID + 1] + start;
    string3 = g.strings[resID + 2] + start;

    g.palManipStart = start;
    g.palManipEnd = end;
    g.palManipCounter = 0;

    pal = g.currentPalette + start * 3;
    target = g.palManipPalette + start * 3;
    between = g.palManipIntermediatePal + start * 3;
    for (i = start; i < end; ++i) {
        *target++ = *string1++;
        *target++ = *string2++;
        *target++ = *string3++;
        *between++ = (uint16_t)(*pal++ << 8);
        *between++ = (uint16_t)(*pal++ << 8);
        *between++ = (uint16_t)(*pal++ << 8);
    }
    g.palManipCounter = time;
}

void palManipulate(void)
{
    const uint8_t *target;
    uint8_t *pal;
    uint16_t *between;
    int i, k;

    if (!g.palManipCounter)
        return;
    target = g.palManipPalette + g.palManipStart * 3;
    pal = g.currentPalette + g.palManipStart * 3;
    between = g.palManipIntermediatePal + g.palManipStart * 3;
    for (i = g.palManipStart; i < g.palManipEnd; ++i) {
        for (k = 0; k < 3; k++) {
            int j = (*between = (uint16_t)(*between + (((*target++ << 8) - *between) / g.palManipCounter)));
            *pal++ = (uint8_t)(j >> 8);
            between++;
        }
    }
    setDirtyColors(g.palManipStart, g.palManipEnd);
    g.palManipCounter--;
}

/* setShadowPalette(redScale, greenScale, blueScale, startColor, endColor, start, end):
 * SO_ROOM_SHADOW.  Each colour of [start, end) scaled, matched to the nearest of the room's
 * [startColor, endColor] in 6-bit space. */
void setShadowPalette(int redScale, int greenScale, int blueScale, int startColor, int endColor,
                      int start, int end)
{
    const uint8_t *basepal = getPalettePtr();
    const uint8_t *pal = basepal + start * 3, *compareptr;
    uint8_t *table = g.shadowPalette + start;
    int i, j;

    /* ScummVM indexes getPalettePtr() directly here, from the CLUT's count field: kept as the
     * oracle has it (see darkenPalette). */
    for (i = start; i < end; i++) {
        int r = ((pal[0] >> 2) * redScale) >> 8;
        int gr = ((pal[1] >> 2) * greenScale) >> 8;
        int b = ((pal[2] >> 2) * blueScale) >> 8;
        uint8_t bestitem = 0;
        unsigned bestsum = 32000;
        pal += 3;
        compareptr = basepal + startColor * 3;
        for (j = startColor; j <= endColor; j++, compareptr += 3) {
            int ar = compareptr[0] >> 2, ag = compareptr[1] >> 2, ab = compareptr[2] >> 2;
            unsigned sum = (unsigned)(abs_i(ar - r) + abs_i(ag - gr) + abs_i(ab - b));
            if (sum < bestsum) {
                bestsum = sum;
                bestitem = (uint8_t)j;
            }
        }
        *table++ = bestitem;
    }
}

/* darkenPalette (v4: max 255; v5/v6 clamp at 252).
 * Note: ScummVM reads the colours from getPalettePtr(), which for GF_SMALL_HEADER points at the
 * CLUT's LE16 count, while setPaletteFromPtr skips those two bytes.  Transcribed as the oracle
 * has it; whether the original interpreter reads them the same way is checked when a room is
 * seen using SO_ROOM_INTENSITY with a scale other than 0 (docs/todo.md). */
void darkenPalette(int redScale, int greenScale, int blueScale, int startColor, int endColor)
{
    const int max = 255;
    if (startColor <= endColor) {
        const uint8_t *palptr = getPalettePtr(), *cptr;
        int color, j;
        for (j = startColor; j <= endColor; j++) {
            cptr = palptr + j * 3;
            color = *cptr++ * redScale / 0xFF;
            if (color > max) color = max;
            g.currentPalette[j * 3 + 0] = (uint8_t)color;
            color = *cptr++ * greenScale / 0xFF;
            if (color > max) color = max;
            g.currentPalette[j * 3 + 1] = (uint8_t)color;
            color = *cptr++ * blueScale / 0xFF;
            if (color > max) color = max;
            g.currentPalette[j * 3 + 2] = (uint8_t)color;
        }
        setDirtyColors(startColor, endColor);
    }
}

/* updatePalette: the changed range, through the shadow palette (_shadowPalRemap, which MI1
 * DOS VGA has), to the platform */
void updatePalette(void)
{
    uint8_t colors[3 * 256], *p = colors;
    int first, num, i;

    if (g.palDirtyMax == -1)
        return;
    first = g.palDirtyMin;
    num = g.palDirtyMax - first + 1;
    for (i = g.palDirtyMin; i <= g.palDirtyMax; i++) {
        const uint8_t *data = g.currentPalette + g.shadowPalette[i] * 3;
        *p++ = data[0];
        *p++ = data[1];
        *p++ = data[2];
    }
    g.palDirtyMax = -1;
    g.palDirtyMin = 256;
    plat_setPalette(first, num, colors);
}
