/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * gfx_ref.c - the reference renderer: ScummVM's own drawing, on the PC only.
 *
 * The oracle for the picture is ScummVM's screen (tools/oracle_scummvm.ps1 -Pixels); this file
 * is ScummVM's renderer for the v4 path, transcribed as the interpreter was (same names, same
 * order, the same memory layout), so that its frames equal ScummVM's pixel for pixel.  The
 * Jaguar draws differently (GPU, Blitter, OP) and is checked against this file's buffers.
 * Never linked into the ROM.
 *
 * ScummVM gfx.cpp: initScreens/initVirtScreen (the buffers), markRectAsDirty (the virtual
 * screen's part), drawDirtyScreenParts, updateDirtyScreen, drawStripToScreen (the text surface
 * composited over the game), initBGBuffers, Gdi::drawBitmap, getZPlanes, drawStrip,
 * decodeMask, decompressBitmap, decompressMaskImg(Or), drawStripBasicH/V, drawStripRaw,
 * writeRoomColor, resetBackground, restoreBackground, drawBox, fadeIn/fadeOut,
 * transitionEffect.
 *
 * The SCUMM screen trick, kept as is: a virtual screen's buffer has a pitch of 320 and room
 * strip x is drawn at byte x * 8 of its row, so pixel (X, Y) of the room is always at
 * Y * 320 + X; the visible window starts at xstart (the camera's left edge).  A scroll by one
 * strip only draws the new strip.  The buffer has 4 rows of slack for the rooms' widths.
 */
#include <stdio.h>
#include <string.h>
#include "../scumm/scumm.h"
#include "ref.h"

#define CHARSET_MASK_TRANSPARENCY 0xFD
#define NUM_STRIPS 40                       /* Gdi::_numStrips = screen width / 8 (v < 7) */
enum { kMainVirtScreen = 0, kTextVirtScreen = 1, kVerbVirtScreen = 2, kBannerVirtScreen = 3 };

typedef struct {
    int number, topline, w, h, pitch, hasTwoBuffers, scrollable;
    uint8_t *pixels, *backBuf;
    int tdirty[NUM_STRIPS], bdirty[NUM_STRIPS];
} VirtScreen;

#define VS_SIZE (320 * (200 + 4))
static uint8_t vsMem[4][VS_SIZE], vsBack[VS_SIZE];
static VirtScreen virtscr[4];

static uint8_t textSurface[320 * 200];          /* _textSurface, pitch 320 */
static uint8_t compositeBuf[320 * 200];

/* the system: the 320x200 screen and the palette the backend holds */
static uint8_t sysScreen[320 * 200];
static uint8_t sysPalette[768];

/* the z-plane buffers (rtBuffer 9) */
#define MASK_MAX (8 * (200 + 4) * NUM_STRIPS)
static uint8_t maskBuf[MASK_MAX];
static int imgBufOffs[8];
static int maskUsed;                             /* initBGBuffers' size: the planes in use */

/* Gdi state */
static int vertStripNextInc, objectMode, decomp_shr, decomp_mask;
static const uint8_t transparentColor = 255;

static ref_present_fn presentHook;

/* --- VirtScreen ------------------------------------------------------------------------ */
static int vs_xstart(const VirtScreen *vs)
{
    return vs->number == kMainVirtScreen ? g.mainXStart : 0;
}

static uint8_t *getPixels(VirtScreen *vs, int x, int y)
{
    return vs->pixels + y * vs->pitch + vs_xstart(vs) + x;
}

static uint8_t *getBackPixels(VirtScreen *vs, int x, int y)
{
    return vs->backBuf + y * vs->pitch + vs_xstart(vs) + x;
}

static uint8_t *getBasePtr(VirtScreen *vs, int x, int y)
{
    return vs->pixels + y * vs->pitch + x;
}

static void setDirtyRange(VirtScreen *vs, int top, int bottom)
{
    int i;
    for (i = 0; i < NUM_STRIPS; i++) {
        vs->tdirty[i] = top;
        vs->bdirty[i] = bottom;
    }
}

static void initVirtScreen(int slot, int top, int width, int height, int twobufs, int scrollable)
{
    VirtScreen *vs = &virtscr[slot];
    int size;

    vs->number = slot;
    vs->w = width;
    vs->topline = top;
    vs->h = height;
    vs->hasTwoBuffers = twobufs;
    vs->scrollable = scrollable;
    vs->backBuf = 0;
    vs->pitch = width;
    size = vs->pitch * vs->h;
    if (scrollable)
        size += vs->pitch * 4;
    if (size > VS_SIZE)
        plat_error("initVirtScreen: too big");
    vs->pixels = vsMem[slot];
    memset(vs->pixels, 0, VS_SIZE);              /* reset background */
    if (twobufs) {
        vs->backBuf = vsBack;
        memset(vs->backBuf, 0, VS_SIZE);         /* a new resource: zeroed */
    }
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

static int isLightOn(void)
{
    return (VAR(VAR_CURRENT_LIGHTS) & 2) != 0;     /* LIGHTMODE_room_lights_on */
}

/* --- to the screen ------------------------------------------------------------------- */

/* drawStripToScreen (v < 7, DOS VGA: no render mode post-processing): the text surface is
 * composited over the game, then the rectangle goes to the system screen */
static void drawStripToScreen(VirtScreen *vs, int x, int width, int top, int bottom)
{
    const uint8_t *src, *text;
    uint8_t *dst;
    int y, height, h, w;

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

    src = getPixels(vs, x, top);
    text = textSurface + y * 320 + x;
    dst = compositeBuf;
    for (h = 0; h < height; h++) {
        for (w = 0; w < width; w++) {
            uint8_t t = text[w];
            *dst++ = (t == CHARSET_MASK_TRANSPARENCY) ? src[w] : t;
        }
        src += vs->pitch;
        text += 320;
    }
    /* copyRectToScreen */
    for (h = 0; h < height; h++)
        memcpy(sysScreen + (y + h) * 320 + x, compositeBuf + h * width, width);
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

void plat_drawDirtyScreenParts(void)
{
    plat_drawDirtyScreenPartsOnly();
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

/* --- the message banner (gfx_gui.cpp: saveSurfacesPreGUI, restoreSurfacesPostGUI; gfx.cpp:
 * drawPixel; showOldStyleBannerAndPause's memsets) ------------------------------------------ */
static uint8_t tempText[320 * 200], tempMain[320 * 200], tempVerb[VS_SIZE];

void plat_saveSurfacesPreGUI(void)
{
    VirtScreen *m = &virtscr[kMainVirtScreen], *v = &virtscr[kVerbVirtScreen];
    int x, y;
    for (y = 0; y < m->h; y++)
        memcpy(tempMain + y * m->w, getBasePtr(m, vs_xstart(m), y), m->w);
    memcpy(tempVerb, getBasePtr(v, 0, 0), v->pitch * v->h);
    memcpy(tempText, textSurface, sizeof(textSurface));
    /* the text surface stamped on the main screen (v4-v6, not LOOM VGA): the banner over the
     * texts, not the texts deleted (setPixel at ((xstart + x) % pitch, y + (xstart + x) / pitch)) */
    for (y = 0; y < 200; y++)
        for (x = 0; x < 320; x++)
            if (tempText[x + y * 320] != CHARSET_MASK_TRANSPARENCY && x < m->pitch && y < m->h) {
                const int xx = vs_xstart(m) + x;
                *getBasePtr(m, xx % m->pitch, y + xx / m->pitch) = tempText[x + y * 320];
            }
}

void plat_restoreSurfacesPostGUI(void)
{
    VirtScreen *m = &virtscr[kMainVirtScreen], *v = &virtscr[kVerbVirtScreen];
    int y;
    memcpy(textSurface, tempText, sizeof(textSurface));
    for (y = 0; y < m->h; y++)
        memcpy(getBasePtr(m, vs_xstart(m), y), tempMain + y * m->w, m->w);
    setDirtyRange(m, 0, m->h);
    memcpy(getBasePtr(v, 0, 0), tempVerb, v->pitch * v->h);
    setDirtyRange(v, 0, v->h);
}

void plat_fillVirtScreen(int virt, int color)
{
    VirtScreen *vs = &virtscr[virt];
    memset(getBasePtr(vs, 0, 0), color, vs->w * vs->h);
}

void plat_drawPixel(int virt, int x, int y, int color)
{
    *getPixels(&virtscr[virt], x, y) = (uint8_t)color;
}

void plat_setDirtyRange(int virt, int top, int bottom)
{
    setDirtyRange(&virtscr[virt], top, bottom);
}

void plat_updateDirtyScreen(int virt)
{
    updateDirtyScreen(virt);
}

/* --- the room and object images (Gdi) ---------------------------------------------------- */

/* initBGBuffers: the buffers for the g.numZBuffer planes the logic counted */
void plat_initBGBuffers(int height)
{
    const int numZBuffer = g.numZBuffer;
    int itemsize, size, i;

    itemsize = (height + 4) * NUM_STRIPS;
    size = itemsize * numZBuffer;
    if (size > MASK_MAX)
        plat_error("initBGBuffers: too big");
    memset(maskBuf, 0, size);
    maskUsed = size;
    for (i = 0; i < 8; i++)
        imgBufOffs[i] = (i < numZBuffer ? i : numZBuffer - 1) * itemsize;
}

static uint8_t *gdi_getMaskBuffer(int x, int y, int z)
{
    return maskBuf + x + y * NUM_STRIPS + imgBufOffs[z];
}

uint8_t *ref_getMaskBuffer(int x, int y, int z)
{
    return gdi_getMaskBuffer((x + g.mainXStart) / 8, y, z);
}

/* the mask buffer as initBGBuffers sized it: g.numZBuffer x (height + 4) x 40 bytes */
const uint8_t *ref_maskArea(int *size)
{
    *size = maskUsed;
    return maskBuf;
}

/* the costume renderer's _out (drawCostume): the front buffer from getPixels(0, 0) moved left
 * by (xstart & 7), so that its columns line up with the strips */
uint8_t *ref_frontBuffer(int x, int y)
{
    VirtScreen *vs = &virtscr[kMainVirtScreen];
    return getPixels(vs, 0, 0) - (vs_xstart(vs) & 7) + y * vs->pitch + x;
}

int ref_mainPitch(void)
{
    return virtscr[kMainVirtScreen].pitch;
}

int ref_noCels, ref_noChars;

/* the front buffer's visible window: row y of the main screen, 320 pixels */
const uint8_t *ref_frontRow(int y)
{
    return getPixels(&virtscr[kMainVirtScreen], 0, y);
}

/* the back buffer's visible window: row y of the main screen, 320 pixels */
const uint8_t *ref_backRow(int y)
{
    return getBackPixels(&virtscr[kMainVirtScreen], 0, y);
}

/* getZPlanes (GF_SMALL_HEADER, not GF_16COLOR, not GF_OLD256) */
static int getZPlanes(const uint8_t *ptr, const uint8_t *zplane_list[9])
{
    int numzbuf = g.numZBuffer, i;
    zplane_list[0] = ptr;
    if (numzbuf > 1) {
        zplane_list[1] = ptr + rd32(ptr);
        for (i = 2; i < numzbuf; i++)
            zplane_list[i] = zplane_list[i - 1] + rd16(zplane_list[i - 1]);
    }
    return numzbuf;
}

static void writeRoomColor(uint8_t *dst, uint8_t color)
{
    *dst = g.roomPalette[color];                 /* _paletteMod 0 (not Amiga) */
}

#define READ_BIT (cl--, bit = bits & 1, bits >>= 1, bit)
#define FILL_BITS do {              \
        if (cl <= 8) {              \
            bits |= (*src++ << cl); \
            cl += 8;                \
        }                           \
    } while (0)

static void drawStripBasicH(uint8_t *dst, int dstPitch, const uint8_t *src, int height, int transpCheck)
{
    uint8_t color = *src++;
    unsigned bits = *src++;
    uint8_t cl = 8, bit;
    int8_t inc = -1;

    do {
        int x = 8;
        do {
            FILL_BITS;
            if (!transpCheck || color != transparentColor)
                writeRoomColor(dst, color);
            dst++;
            if (!READ_BIT) {
            } else if (!READ_BIT) {
                FILL_BITS;
                color = (uint8_t)(bits & decomp_mask);
                bits >>= decomp_shr;
                cl -= decomp_shr;
                inc = -1;
            } else if (!READ_BIT) {
                color += inc;
            } else {
                inc = -inc;
                color += inc;
            }
        } while (--x);
        dst += dstPitch - 8;
    } while (--height);
}

static void drawStripBasicV(uint8_t *dst, int dstPitch, const uint8_t *src, int height, int transpCheck)
{
    uint8_t color = *src++;
    unsigned bits = *src++;
    uint8_t cl = 8, bit;
    int8_t inc = -1;
    int x = 8;

    do {
        int h = height;
        do {
            FILL_BITS;
            if (!transpCheck || color != transparentColor)
                writeRoomColor(dst, color);
            dst += dstPitch;
            if (!READ_BIT) {
            } else if (!READ_BIT) {
                FILL_BITS;
                color = (uint8_t)(bits & decomp_mask);
                bits >>= decomp_shr;
                cl -= decomp_shr;
                inc = -1;
            } else if (!READ_BIT) {
                color += inc;
            } else {
                inc = -inc;
                color += inc;
            }
        } while (--h);
        dst -= vertStripNextInc;
    } while (--x);
}
#undef READ_BIT
#undef FILL_BITS

static void drawStripRaw(uint8_t *dst, int dstPitch, const uint8_t *src, int height, int transpCheck)
{
    int x;
    do {
        for (x = 0; x < 8; x++) {
            uint8_t color = *src++;
            if (!transpCheck || color != transparentColor)
                writeRoomColor(dst + x, color);
        }
        dst += dstPitch;
    } while (--height);
}

/* decompressBitmap: the codecs a v4 VGA game has (not GF_16COLOR) */
static int decompressBitmap(uint8_t *dst, int dstPitch, const uint8_t *src, int numLinesToProcess)
{
    uint8_t code = *src++;
    int transpStrip = 0;

    decomp_shr = code % 10;
    decomp_mask = 0xFF >> (8 - decomp_shr);
    if (code == 1) {                             /* BMCOMP_RAW256 */
        drawStripRaw(dst, dstPitch, src, numLinesToProcess, 0);
    } else if (code >= 14 && code <= 18) {       /* BMCOMP_ZIGZAG_V4..V8 */
        drawStripBasicV(dst, dstPitch, src, numLinesToProcess, 0);
    } else if (code >= 24 && code <= 28) {       /* BMCOMP_ZIGZAG_H4..H8 */
        drawStripBasicH(dst, dstPitch, src, numLinesToProcess, 0);
    } else if (code >= 34 && code <= 38) {       /* BMCOMP_ZIGZAG_VT4..VT8 */
        transpStrip = 1;
        drawStripBasicV(dst, dstPitch, src, numLinesToProcess, 1);
    } else if (code >= 44 && code <= 48) {       /* BMCOMP_ZIGZAG_HT4..HT8 */
        transpStrip = 1;
        drawStripBasicH(dst, dstPitch, src, numLinesToProcess, 1);
    } else {
        plat_error("Gdi::decompressBitmap: a codec v4 VGA does not have");
    }
    return transpStrip;
}

/* drawStrip, GF_SMALL_HEADER: LE32 length, one LE32 offset per strip */
static int drawStrip(uint8_t *dstPtr, VirtScreen *vs, int stripnr, const uint8_t *smap_ptr, int height)
{
    int offset = -1, smapLen = (int)rd32(smap_ptr);
    if (stripnr * 4 + 4 < smapLen)
        offset = (int)rd32(smap_ptr + stripnr * 4 + 4);
    if (offset < 0 || offset > smapLen - 1) {
        printf("drawStrip: room %d (%d wide), object %d, state %d, strip %d of %d (smapLen %d), "
               "camera %d, xstart %d\n", g.currentRoom, g.roomWidth, g.drawingObject,
               g.drawingObject ? getState(g.drawingObject) : 0, stripnr,
               (int)((rd32(smap_ptr + 4) - 4) / 4), smapLen, g.camera.cur_x, g.mainXStart);
        plat_error("drawStrip: screen strip out of range");
    }
    return decompressBitmap(dstPtr, vs->pitch, smap_ptr + offset, height);
}

static void decompressMaskImg(uint8_t *dst, const uint8_t *src, int height)
{
    uint8_t b, c;
    while (height) {
        b = *src++;
        if (b & 0x80) {
            b &= 0x7F;
            c = *src++;
            do {
                *dst = c;
                dst += NUM_STRIPS;
                --height;
            } while (--b && height);
        } else {
            do {
                *dst = *src++;
                dst += NUM_STRIPS;
                --height;
            } while (--b && height);
        }
    }
}

static void decompressMaskImgOr(uint8_t *dst, const uint8_t *src, int height)
{
    uint8_t b, c;
    while (height) {
        b = *src++;
        if (b & 0x80) {
            b &= 0x7F;
            c = *src++;
            do {
                *dst |= c;
                dst += NUM_STRIPS;
                --height;
            } while (--b && height);
        } else {
            do {
                *dst |= *src++;
                dst += NUM_STRIPS;
                --height;
            } while (--b && height);
        }
    }
}

/* decodeMask (GF_SMALL_HEADER: a z-plane's strip offsets are LE16 after its LE16 size) */
static void decodeMask(int x, int y, int height, int stripnr, int numzbuf,
                       const uint8_t *zplane_list[9], int transpStrip, int flag)
{
    int i, h;
    if (flag & DB_DRAW_MASK_ON_ALL)
        plat_error("decodeMask: dbDrawMaskOnAll is not a v4 flag");
    for (i = 1; i < numzbuf; i++) {
        unsigned offs;
        uint8_t *mask_ptr;
        if (!zplane_list[i])
            continue;
        offs = rd16(zplane_list[i] + stripnr * 2 + 2);
        mask_ptr = gdi_getMaskBuffer(x, y, i);
        if (offs) {
            const uint8_t *z_plane_ptr = zplane_list[i] + offs;
            if (transpStrip && (flag & DB_ALLOW_MASK_OR))
                decompressMaskImgOr(mask_ptr, z_plane_ptr, height);
            else
                decompressMaskImg(mask_ptr, z_plane_ptr, height);
        } else {
            if (!(transpStrip && (flag & DB_ALLOW_MASK_OR)))
                for (h = 0; h < height; h++)
                    mask_ptr[h * NUM_STRIPS] = 0;
        }
    }
}

static void copy8Col(uint8_t *dst, int dstPitch, const uint8_t *src, int height)
{
    do {
        memcpy(dst, src, 8);
        dst += dstPitch;
        src += dstPitch;
    } while (--height);
}

static void clear8Col(uint8_t *dst, int dstPitch, int height)
{
    do {
        memset(dst, 0, 8);
        dst += dstPitch;
    } while (--height);
}

/* Gdi::drawBitmap on the main virtual screen */
void plat_drawBitmap(const uint8_t *ptr, int x, int y, int width, int height,
                     int stripnr, int numstrip, int flag)
{
    VirtScreen *vs = &virtscr[kMainVirtScreen];
    const uint8_t *zplane_list[9];
    const int lightsOn = isLightOn();
    int numzbuf, sx, limit, k, transpStrip;
    uint8_t *dstPtr;

    if (!ptr || height <= 0)
        plat_error("drawBitmap: no image");
    numzbuf = getZPlanes(ptr, zplane_list);
    vertStripNextInc = height * vs->pitch - 1;
    objectMode = (flag & DB_OBJECT_MODE) == DB_OBJECT_MODE;
    (void)objectMode;

    sx = x - vs_xstart(vs) / 8;
    if (sx < 0) {
        numstrip -= -sx;
        x += -sx;
        stripnr += -sx;
        sx = 0;
    }
    limit = (g.roomWidth > vs->w ? g.roomWidth : vs->w) / 8 - x;
    if (limit > numstrip)
        limit = numstrip;
    if (limit > NUM_STRIPS - sx)
        limit = NUM_STRIPS - sx;

    for (k = 0; k < limit; ++k, ++stripnr, ++sx, ++x) {
        if (y < vs->tdirty[sx])
            vs->tdirty[sx] = y;
        if (y + height > vs->bdirty[sx])
            vs->bdirty[sx] = y + height;

        dstPtr = vs->backBuf + y * vs->pitch + x * 8;
        transpStrip = drawStrip(dstPtr, vs, stripnr, ptr, height);
        {
            uint8_t *frontBuf = getBasePtr(vs, x * 8, y);
            if (lightsOn)
                copy8Col(frontBuf, vs->pitch, dstPtr, height);
            else
                clear8Col(frontBuf, vs->pitch, height);
        }
        decodeMask(x, y, height, stripnr, numzbuf, zplane_list, transpStrip, flag);
    }
}

/* Gdi::resetBackground: an actor's strip back from the background */
void plat_resetBackground(int top, int bottom, int strip)
{
    VirtScreen *vs = &virtscr[kMainVirtScreen];
    uint8_t *backbuff_ptr, *bgbak_ptr;
    int numLinesToProcess;

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

    bgbak_ptr = vs->backBuf + top * vs->pitch + (strip + vs_xstart(vs) / 8) * 8;
    backbuff_ptr = getBasePtr(vs, (strip + vs_xstart(vs) / 8) * 8, top);
    numLinesToProcess = bottom - top;
    if (numLinesToProcess) {
        if (isLightOn())
            copy8Col(backbuff_ptr, vs->pitch, bgbak_ptr, numLinesToProcess);
        else
            clear8Col(backbuff_ptr, vs->pitch, numLinesToProcess);
    }
}

static void blit(uint8_t *dst, int dstPitch, const uint8_t *src, int srcPitch, int w, int h)
{
    do {
        memcpy(dst, src, w);
        dst += dstPitch;
        src += srcPitch;
    } while (--h);
}

static void fill(uint8_t *dst, int dstPitch, uint8_t color, int w, int h)
{
    do {
        memset(dst, color, w);
        dst += dstPitch;
    } while (--h);
}

/* restoreBackground, the pixels: the logic has found the virtual screen, clipped the
 * rectangle to it (its own coordinates, right and bottom exclusive) and marked it */
void plat_restoreBackground(int virt, int left, int top, int right, int bottom, int backColor)
{
    VirtScreen *vs = &virtscr[virt];
    uint8_t *screenBuf = getPixels(vs, left, top);
    const int width = right - left, height = bottom - top;

    if (width <= 0 || height <= 0)
        return;
    if (vs->hasTwoBuffers && g.currentRoom != 0 && isLightOn()) {
        blit(screenBuf, vs->pitch, getBackPixels(vs, left, top), vs->pitch, width, height);
        if (vs->number == kMainVirtScreen && text_hasMask())
            fill(textSurface + top * 320 + left, 320, CHARSET_MASK_TRANSPARENCY, width, height);
    } else {
        fill(screenBuf, vs->pitch, (uint8_t)backColor, width, height);
    }
}

/* restoreCharsetBg, the pixels: over the room only the text surface is cleared (the screen
 * is redrawn from the buffers the logic marked); a screen without a background is cleared */
void plat_restoreCharsetBg(int virt)
{
    VirtScreen *vs = &virtscr[virt];
    uint8_t *screenBuf = getPixels(vs, 0, 0);
    if (vs->hasTwoBuffers && g.currentRoom != 0 && isLightOn()) {
        if (vs->number != kMainVirtScreen)
            blit(screenBuf, vs->pitch, getBackPixels(vs, 0, 0), vs->pitch, vs->w, vs->h);
    } else {
        memset(screenBuf, 0, vs->h * vs->pitch);
    }
    if (vs->hasTwoBuffers)
        ref_clearTextSurface();
}

/* printCharIntern + CharsetRendererClassic::drawBitsN: no clipping in x (printChar has
 * refused a glyph past either side); in y against the surface's height */
void plat_drawChar(int virt, int toText, int x, int y, int drawTop, const uint8_t *src,
                   int bpp, int width, int height, const uint8_t *cmap)
{
    VirtScreen *vs = &virtscr[virt];
    uint8_t *dst;
    int sh, spitch, yy, xx, color, pitch;
    uint8_t numbits, bits;

    if (ref_noChars)
        return;
    if (toText) {
        dst = textSurface + y * 320 + x;
        sh = 200;
        spitch = 320;
    } else {
        dst = getPixels(vs, x, y);
        sh = vs->h;
        spitch = vs->pitch;
    }
    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8)
        plat_error("drawBitsN: bpp");
    pitch = spitch - width;
    bits = *src++;
    numbits = 8;
    for (yy = 0; yy < height && yy + drawTop < sh; yy++) {
        for (xx = 0; xx < width; xx++) {
            color = (bits >> (8 - bpp)) & 0xFF;
            if (color && yy + drawTop >= 0)
                *dst = cmap[color];
            dst++;
            bits = (uint8_t)(bits << bpp);
            numbits = (uint8_t)(numbits - bpp);
            if (numbits == 0) {
                bits = *src++;
                numbits = 8;
            }
        }
        dst += pitch;
    }
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
        blit(getPixels(vs, x, y), vs->pitch, getBackPixels(vs, x, y), vs->pitch, width, height);
        if (text_hasMask())
            fill(textSurface + y * 320 + x, 320, CHARSET_MASK_TRANSPARENCY, width, height);
    } else {
        fill(getPixels(vs, x, y), vs->pitch, (uint8_t)color, width, height);
    }
}

/* --- the text surface ------------------------------------------------------------------ */
void ref_clearTextSurface(void)
{
    memset(textSurface, CHARSET_MASK_TRANSPARENCY, sizeof(textSurface));
}

uint8_t *ref_textSurface(void)
{
    return textSurface;
}

/* --- effects --------------------------------------------------------------------------- */
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

/* transitionEffect: 8x8 blocks of the main screen shown in the effect's order; each
 * iteration ends with waitForTimer, where ScummVM shows the screen: presentHook */
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
        if (presentHook)
            presentHook();                       /* waitForTimer(delay, true) */
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
    case 128: case 134: case 135:                /* dissolveEffect: the logic's order, plat_dissolve */
        break;
    default:                                     /* 130-133: scrollEffect is v5/v6 */
        plat_error("fadeIn: effect not in the reference renderer");
    }
}

void plat_fadeOut(int effect, int run)
{
    VirtScreen *vs = &virtscr[kMainVirtScreen];
    setDirtyRange(vs, 0, 0);
    if (!run)
        return;
    memset(getPixels(vs, 0, 0), 0, vs->pitch * vs->h);
    switch (effect) {
    case 1: case 2: case 3: case 4: case 5: case 6:
        transitionEffect(effect - 1);
        break;
    case 129:
        setDirtyRange(vs, 0, vs->h);
        updateDirtyScreen(kMainVirtScreen);
        break;
    case 128: case 134: case 135:                /* dissolveEffect: plat_dissolve */
        break;
    default:
        plat_error("fadeOut: effect not in the reference renderer");
    }
}

/* dissolveEffect, the drawing: the blocks in the logic's order; a width that is a multiple of
 * 4 goes through drawStripToScreen (the text composited), any other straight to the screen
 * (copyRectToScreen, DOS VGA: no dithering).  The screen is shown (waitForTimer(1)) every
 * blitsBeforeRefresh blocks of a 1x1 dissolve, and after every block of the top row of an
 * NxM one (offset < the screen's height, as ScummVM tests it). */
static uint16_t dissolveOrder[320 * 200];

uint16_t *plat_dissolveBuffer(int n)
{
    if (n > (int)(sizeof(dissolveOrder) / sizeof(dissolveOrder[0])))
        plat_error("dissolveEffect: too many blocks");
    return dissolveOrder;
}

void plat_dissolve(int width, int height, const uint16_t *offsets, int n)
{
    VirtScreen *vs = &virtscr[kMainVirtScreen];
    const int is1x1Pattern = width == 1 && height == 1;
    const int h = vs->h / height + (vs->h % height ? 1 : 0);
    const int blitsToFreeze = h / 8;
    const int blitsBeforeRefresh = n / (blitsToFreeze == 0 ? 18 : blitsToFreeze);
    int i, blits = 0, canHalt = 0;

    for (i = 0; i < n; i++) {
        int x = offsets[i] % vs->pitch, y = offsets[i] / vs->pitch;
        if ((width & 3) == 0) {
            drawStripToScreen(vs, x, width, y, y + height);
        } else {
            const uint8_t *src = getPixels(vs, x, y);
            int yy, ht = height;
            y += vs->topline;
            if (y + ht > 200)
                ht = 200 - y;
            for (yy = 0; yy < ht; yy++)
                memcpy(sysScreen + (y + yy) * 320 + x, src + yy * vs->pitch, width);
        }
        canHalt |= is1x1Pattern && ++blits >= blitsBeforeRefresh;
        if (canHalt)
            blits = 0;
        canHalt |= !is1x1Pattern && offsets[i] < vs->h;
        if (canHalt) {
            canHalt = 0;
            if (presentHook)
                presentHook();                   /* waitForTimer(1, true) */
        }
    }
}

/* --- the cursor: the backend draws it over the screen at the mouse, minus its hotspot,
 * when it is shown and the real mouse is over the window -------------------------------- */
static uint8_t cursorImg[16 * 16];
static int cursorW, cursorH, cursorHotX, cursorHotY, cursorShown;
static int mouseX, mouseY, mouseInWindow;
static uint8_t frameOut[320 * 200];

void plat_setCursor(const uint8_t *img, int w, int h, int hotX, int hotY)
{
    if (w * h > (int)sizeof(cursorImg))
        plat_error("setCursor: too big");
    memcpy(cursorImg, img, w * h);
    cursorW = w;
    cursorH = h;
    cursorHotX = hotX;
    cursorHotY = hotY;
}

void plat_showMouse(int show)
{
    cursorShown = show;
}

int ref_cursor(const uint8_t **img, int *w, int *h, int *hotX, int *hotY)
{
    *img = cursorImg;
    *w = cursorW;
    *h = cursorH;
    *hotX = cursorHotX;
    *hotY = cursorHotY;
    return cursorShown;
}

void ref_setMouse(int x, int y)
{
    mouseX = x;
    mouseY = y;
    mouseInWindow = 1;
}

const uint8_t *ref_frame(void)
{
    int x, y;
    memcpy(frameOut, sysScreen, sizeof(frameOut));
    if (!cursorShown || !mouseInWindow)
        return frameOut;
    for (y = 0; y < cursorH; y++)
        for (x = 0; x < cursorW; x++) {
            int sx = mouseX - cursorHotX + x, sy = mouseY - cursorHotY + y;
            uint8_t c = cursorImg[y * cursorW + x];
            if (c != 0xFF && sx >= 0 && sx < 320 && sy >= 0 && sy < 200)
                frameOut[sy * 320 + sx] = c;
        }
    return frameOut;
}

/* --- the system side --------------------------------------------------------------------- */
static int paletteChanged;

void plat_setPalette(int first, int num, const uint8_t *rgb)
{
    memcpy(sysPalette + first * 3, rgb, num * 3);
    paletteChanged = 1;
}

int ref_paletteChanged(void)
{
    int c = paletteChanged;
    paletteChanged = 0;
    return c;
}

void ref_init(void)
{
    memset(sysScreen, 0, sizeof(sysScreen));
    memset(sysPalette, 0, sizeof(sysPalette));
    ref_clearTextSurface();
}

void ref_setPresentHook(ref_present_fn fn)
{
    presentHook = fn;
}

const uint8_t *ref_screen(void)
{
    return sysScreen;
}

const uint8_t *ref_palette(void)
{
    return sysPalette;
}
