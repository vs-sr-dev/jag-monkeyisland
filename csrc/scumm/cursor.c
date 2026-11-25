/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * cursor.c - the mouse cursor: its shape, its hotspot, its colour animation.
 *
 * ScummVM cursor.cpp: ScummEngine_v5::resetCursors, setBuiltinCursor (VGA, 1x),
 * animateCursor; the end of scummLoop (CursorMan.showMouse).  v4 games use the v5 engine's
 * cursor: four built-in 16x16 one-bit shapes, drawn in one colour that cycles 15, 15, 7, 8,
 * one step every two loop rounds.  The image goes to the platform (plat_setCursor), which
 * draws it at the mouse; showing it depends on _cursor.state.
 */
#include <string.h>
#include "scumm.h"

static const uint16_t default_cursor_images[4][16] = {
    /* cross-hair */
    { 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0000, 0x7e3f,
      0x0000, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0080, 0x0000 },
    /* hourglass */
    { 0x0000, 0x7ffe, 0x6006, 0x300c, 0x1818, 0x0c30, 0x0660, 0x03c0,
      0x0660, 0x0c30, 0x1998, 0x33cc, 0x67e6, 0x7ffe, 0x0000, 0x0000 },
    /* arrow */
    { 0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7c00, 0x7e00, 0x7f00,
      0x7f80, 0x78c0, 0x7c00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0180 },
    /* hand */
    { 0x1e00, 0x1200, 0x1200, 0x1200, 0x1200, 0x13ff, 0x1249, 0x1249,
      0xf249, 0x9001, 0x9001, 0x9001, 0x8001, 0x8001, 0x8001, 0xffff },
};

static const uint8_t default_cursor_hotspots[8] = { 8, 7,  8, 7,  1, 1,  5, 0 };
static const uint8_t default_cursor_colors[4] = { 15, 15, 7, 8 };

/* resetCursors (the v5 engine's constructor) */
void resetCursors(void)
{
    memcpy(g.cursorImages, default_cursor_images, sizeof(g.cursorImages));
    memcpy(g.cursorHotspots, default_cursor_hotspots, sizeof(g.cursorHotspots));
}

/* setBuiltinCursor: the current shape in colour idx, transparent 0xFF, to the platform */
void setBuiltinCursor(int idx)
{
    const uint16_t *src = g.cursorImages[g.currentCursor];
    const uint8_t color = default_cursor_colors[idx];
    int i, j;

    memset(g.grabbedCursor, 0xFF, sizeof(g.grabbedCursor));
    g.cursor.hotspotX = g.cursorHotspots[2 * g.currentCursor];
    g.cursor.hotspotY = g.cursorHotspots[2 * g.currentCursor + 1];
    g.cursor.width = 16;
    g.cursor.height = 16;
    for (i = 0; i < 16; i++)
        for (j = 0; j < 16; j++)
            if (src[i] & (1 << j))
                g.grabbedCursor[16 * i + (15 - j)] = color;
    /* updateCursor: transparent colour 255 */
    plat_setCursor(g.grabbedCursor, g.cursor.width, g.cursor.height, g.cursor.hotspotX,
                   g.cursor.hotspotY);
}

/* ScummEngine_v5::animateCursor: every other round the next colour */
void animateCursor(void)
{
    if (g.cursor.animate) {
        if (!(g.cursor.animateIndex & 1))
            setBuiltinCursor((g.cursor.animateIndex >> 1) & 3);
        g.cursor.animateIndex++;
    }
}

/* updateCursor: the image as it is, to the platform (a load) */
void updateCursor(void)
{
    plat_setCursor(g.grabbedCursor, g.cursor.width, g.cursor.height, g.cursor.hotspotX,
                   g.cursor.hotspotY);
}
