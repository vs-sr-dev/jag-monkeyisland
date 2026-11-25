/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * ref.h - the reference renderer (csrc/ref/, PC only): what the PC harness reads from it.
 */
#ifndef REF_H
#define REF_H
#include <stdint.h>

typedef void (*ref_present_fn)(void);

void ref_init(void);
/* called where ScummVM shows the screen inside a frame (an effect's waitForTimer) */
void ref_setPresentHook(ref_present_fn fn);
const uint8_t *ref_screen(void);            /* the system screen, 320x200 palette indices */
const uint8_t *ref_frame(void);             /* what the window shows: the screen and the cursor */
void ref_setMouse(int x, int y);            /* the real mouse, over the window from now on */
const uint8_t *ref_palette(void);           /* the system palette, 256 x RGB 8-bit */
int ref_paletteChanged(void);               /* set by plat_setPalette; cleared by the call */

/* for the other parts of the renderer */
void ref_clearTextSurface(void);
uint8_t *ref_textSurface(void);
uint8_t *ref_getMaskBuffer(int x, int y, int z);   /* ScummEngine::getMaskBuffer */
uint8_t *ref_frontBuffer(int x, int y);            /* the main virtual screen, as _out sees it */
int ref_mainPitch(void);
const uint8_t *ref_backRow(int y);                 /* the back buffer's visible row y */
const uint8_t *ref_maskArea(int *size);            /* the z-plane buffer, the part in use */
const uint8_t *ref_frontRow(int y);                /* the main screen's visible row y */

/* staged checks of the Jaguar (scumm_pc --no-cels, --no-chars): the costumes' or the glyphs'
 * pixels not drawn; the logic is the same */
extern int ref_noCels, ref_noChars;
/* the cursor as plat_setCursor / plat_showMouse left it: returns shown */
int ref_cursor(const uint8_t **img, int *w, int *h, int *hotX, int *hotY);

#endif
