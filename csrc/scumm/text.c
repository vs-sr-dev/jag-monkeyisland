/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * text.c - messages: what the actors say, how long it stays, and the text metrics that decide
 * where a line breaks (which changes how long it stays).
 *
 * ScummVM string.cpp: printString, actorTalk (actor.cpp), stopTalk (actor.cpp),
 * runActorTalkScript (actor.cpp), displayDialog, handleNextCharsetCode, newLine,
 * convertMessageToString and its helpers, initCharset, restoreCharsetBg (gfx.cpp);
 * charset.cpp: CharsetRendererCommon::setCurID, getFontHeight, CharsetRendererClassic::
 * getCharWidth, printChar (its advance and _hasMask), CharsetRenderer::getStringWidth,
 * addLinebreaks.  The v4 paths, subtitles on, no talkie, no CJK.
 *
 * What the scripts see: VAR_HAVE_MSG (0xFF while a message is being shown, 1 when its time
 * ran out, 0 when it is gone), the talk delay (60 jiffies plus VAR_CHARINC per printed
 * character: a line break replaces a space and is not printed, so the charset widths change
 * the timing), the talk animation frames, VAR_TALK_ACTOR.
 *
 * The drawing: printChar marks the glyph's rectangle and hands the glyph to the platform
 * (plat_drawChar: into the text surface over the room, or into a virtual screen);
 * restoreCharsetBg marks the text's screen restored (USAGE_BIT_RESTORED: actors on it are
 * redrawn, which recomputes their scale) before the platform clears the text; drawVerb
 * restores a verb's old rectangle (restoreBackground) and draws it in its colour.
 */
#include <string.h>
#include "scumm.h"

static struct {
    int curId;
    const uint8_t *font;                     /* _fontPtr: the 90x.LFL file + 21 (v4) */
    int fontHeight;
    int left, top, right, startLeft, center, disableOffsX, firstChar, hasMask;
    int color, textScreenID;
} cs;

static uint8_t charsetBuffer[512];
static int charsetBufPos, keepText, msgCount, nextLeft, nextTop, useTalkAnims;
static uint8_t charsetColor;

/* --- the charset (charset.cpp) --------------------------------------------------------- */

/* CharsetRenderer::setColor (translateColor: nothing for VGA) */
static void setColor(int color)
{
    cs.color = color & 0xFF;
}

static void setCurID(int id)
{
    if (id == -1)
        return;
    if (id < 0 || id >= NUM_CHARSETS || !g.img->charset[id]) {
        plat_error("setCurID: charset not found");
        return;
    }
    cs.curId = id;
    cs.font = g.img->charset[id] + 4 + 17;   /* the resource skips the file's LE32 size */
    cs.fontHeight = cs.font[1];
}

int text_fontHeight(void)
{
    return cs.fontHeight;
}

static int getCharWidth(int chr)
{
    uint32_t offs = rd32(cs.font + chr * 4 + 4);
    return offs ? cs.font[offs] + (int8_t)cs.font[offs + 2] : 0;
}

int text_getStringWidth(int arg, const uint8_t *text)
{
    int pos = 0, width = 1, chr, oldID = cs.curId;
    while ((chr = text[pos++]) != 0) {
        if (chr == '\n' || chr == '\r')
            break;
        if (chr == '@')
            continue;
        if (chr == 255 || chr == 254) {
            chr = text[pos++];
            if (chr == 3)                                    /* wait */
                break;
            if (chr == 8) {                                  /* verb on next line */
                if (arg == 1)
                    break;
                while (text[pos++] == ' ') {}
                continue;
            }
            if (chr == 10 || chr == 21 || chr == 12 || chr == 13) {
                pos += 2;
                continue;
            }
            if (chr == 9 || chr == 1 || chr == 2)            /* newline */
                break;
            if (chr == 14) {
                int set = text[pos] | (text[pos + 1] << 8);
                pos += 2;
                setCurID(set);
                continue;
            }
        }
        width += getCharWidth(chr);
    }
    setCurID(oldID);
    return width;
}

static void addLinebreaks(int a, uint8_t *str, int pos, int maxwidth)
{
    int lastspace = -1, curw = 1, chr, oldID = cs.curId;
    while ((chr = str[pos++]) != 0) {
        if (chr == '@')
            continue;
        if (chr == 255 || chr == 254) {
            chr = str[pos++];
            if (chr == 3)
                break;
            if (chr == 8) {
                if (a == 1)
                    curw = 1;
                else
                    while (str[pos] == ' ')
                        str[pos++] = '@';
                continue;
            }
            if (chr == 10 || chr == 21 || chr == 12 || chr == 13) {
                pos += 2;
                continue;
            }
            if (chr == 1) {
                curw = 1;
                continue;
            }
            if (chr == 2)
                break;
            if (chr == 14) {
                int set = str[pos] | (str[pos + 1] << 8);
                pos += 2;
                setCurID(set);
                continue;
            }
        }
        if (chr == ' ')
            lastspace = pos - 1;
        curw += getCharWidth(chr);
        if (lastspace == -1)
            continue;
        if (curw > maxwidth) {
            str[lastspace] = 0xD;
            curw = 1;
            pos = lastspace + 1;
            lastspace = -1;
        }
    }
    setCurID(oldID);
}

/* the rectangle the text drawn so far covers (_charset->_str) */
static struct { int left, top, right, bottom; } str;

/* CharsetRendererClassic::printChar: the pen advance, the rectangle the text covers, the mask
 * flag (text over the room, which a scroll or stopTalk takes away; drawString prints
 * ignoring the mask), and the glyph (printCharIntern) */
static void printChar(int chr, int ignoreCharsetMask)
{
    const uint8_t *charPtr;
    uint32_t charOffs;
    int origWidth, origHeight, offsX, offsY, vs, drawTop;

    if ((vs = scumm_findVirtScreen(cs.top)) < 0 && (vs = scumm_findVirtScreen(cs.top + cs.fontHeight)) < 0)
        return;
    if (chr == '@')
        return;
    g.charsetColorMap[1] = (uint8_t)cs.color;
    charOffs = rd32(cs.font + chr * 4 + 4);
    if (!charOffs)
        return;
    charPtr = cs.font + charOffs;
    origWidth = charPtr[0];
    origHeight = charPtr[1];
    offsX = cs.disableOffsX ? 0 : (int8_t)charPtr[2];
    offsY = (int8_t)charPtr[3];

    if (cs.firstChar)
        str.left = str.top = str.right = str.bottom = 0;
    cs.top += offsY;
    cs.left += offsX;
    if (cs.left + origWidth > cs.right + 1 || cs.left < 0) {
        cs.left += origWidth;
        cs.top -= offsY;
        return;
    }
    cs.disableOffsX = 0;
    if (cs.firstChar) {
        str.left = str.right = cs.left;
        str.top = str.bottom = cs.top;
        cs.firstChar = 0;
    }
    if (cs.left < str.left)
        str.left = cs.left;
    if (cs.top < str.top)
        str.top = cs.top;
    drawTop = cs.top - scumm_virtscrTop(vs);
    markRectAsDirty(vs, cs.left, cs.left + origWidth, drawTop, drawTop + origHeight, 0);
    if (!ignoreCharsetMask) {
        cs.hasMask = 1;
        cs.textScreenID = vs;
    }
    /* printCharIntern: over the room (the main screen has two buffers) the glyph goes to the
     * text surface, at the screen's y; otherwise into the virtual screen */
    if (!ignoreCharsetMask && vs == 0)
        plat_drawChar(vs, 1, cs.left, cs.top, cs.top, charPtr + 4, cs.font[0], origWidth,
                      origHeight, g.charsetColorMap);
    else
        plat_drawChar(vs, 0, cs.left, drawTop, drawTop, charPtr + 4, cs.font[0], origWidth,
                      origHeight, g.charsetColorMap);
    cs.left += origWidth;
    if (str.right < cs.left)
        str.right = cs.left;                 /* no shadow with the classic charset */
    if (str.bottom < cs.top + origHeight)
        str.bottom = cs.top + origHeight;
    cs.top -= offsY;
}

/* --- string slots ------------------------------------------------------------------------ */

void text_init(void)
{
    int i;
    memset(&cs, 0, sizeof(cs));
    cs.curId = -1;
    charsetBufPos = keepText = msgCount = nextLeft = nextTop = useTalkAnims = 0;
    for (i = 0; i < 6; i++) {
        TextSlot *d = &g.stringDefault[i];
        memset(d, 0, sizeof(*d));
        d->xpos = 2;
        d->ypos = 5;
        d->right = (int16_t)(g.screenWidth - 1);
        d->color = 0xF;
    }
}

void initCharset(int charsetno)
{
    g.stringDefault[0].charset = (uint8_t)charsetno;
    g.stringDefault[1].charset = (uint8_t)charsetno;
    memcpy(g.charsetColorMap, g.charsetData[charsetno], sizeof(g.charsetColorMap));
}

/* --- talking ------------------------------------------------------------------------------ */

static int getTalkingActor(void) { return VAR(VAR_TALK_ACTOR); }
static void setTalkingActor(int i) { VAR(VAR_TALK_ACTOR) = i; }

/* restoreCharsetBg (gfx.cpp): the screen the text was printed on is marked restored and the
 * text taken away */
static void restoreCharsetBg(void)
{
    nextLeft = g.string[0].xpos;
    nextTop = g.string[0].ypos;              /* + _screenTop, 0 before v7 */
    if (cs.hasMask || g.postGUICharMask) {
        int vs = cs.textScreenID, h = scumm_virtscrHeight(vs);
        g.postGUICharMask = 0;
        cs.hasMask = 0;
        str.left = -1;
        cs.left = -1;
        if (!h)
            return;
        markRectAsDirty(vs, 0, g.screenWidth, 0, h, USAGE_BIT_RESTORED);
        plat_restoreCharsetBg(vs);
    }
}

static void runActorTalkScript(Actor *a, int f)
{
    if (!getTalkingActor() || a->room != g.currentRoom || a->frame == f)
        return;
    if (a->talkScript) {
        int32_t args[NUM_SCRIPT_LOCAL];
        memset(args, 0, sizeof(args));
        args[1] = f;
        args[0] = a->number;
        runScript(a->talkScript, 1, 0, args);
    } else {
        actor_startAnim(a, f);
    }
}

int text_isTalking(int act)
{
    return getTalkingActor() == act;
}

int text_hasMask(void)
{
    return cs.hasMask;
}

void stopTalk(void)
{
    int act;
    g.haveMsg = 0;
    g.talkDelay = 0;
    act = getTalkingActor();
    if (act && act < 0x80) {
        Actor *a = derefActor(act);
        if (actor_inCurrentRoom(a) && useTalkAnims) {
            runActorTalkScript(a, a->talkStopFrame);
            useTalkAnims = 0;
        }
        setTalkingActor(0xFF);
    }
    keepText = 0;
    restoreCharsetBg();
}

/* --- messages ----------------------------------------------------------------------------- */

static int convertMessageToString(const uint8_t *msg, uint8_t *dst, int dstSize);

static int convertIntMessage(uint8_t *dst, int dstSize, int var)
{
    char tmp[16];
    int n = 0, num = readVar((unsigned)var), i;
    unsigned u = num < 0 ? (unsigned)-num : (unsigned)num;
    do {
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (num < 0)
        tmp[n++] = '-';
    if (n > dstSize - 1)
        n = dstSize - 1;
    for (i = 0; i < n; i++)
        dst[i] = (uint8_t)tmp[n - 1 - i];
    return n;
}

static int convertVerbMessage(uint8_t *dst, int dstSize, int var)
{
    int num = readVar((unsigned)var), k;
    if (num)
        for (k = 1; k < NUM_VERBS; k++)
            if (num == g.verbs[k].verbid && !g.verbs[k].type && !g.verbs[k].saveid)
                return convertMessageToString(g.verbs[k].name, dst, dstSize);
    return 0;
}

/* getObjOrActorName */
static const uint8_t *getObjOrActorName(int obj)
{
    const uint8_t *objptr;
    if (obj < NUM_ACTORS) {
        const Actor *a = derefActor(obj);
        return a->name[0] ? (const uint8_t *)a->name : (const uint8_t *)" ";
    }
    objptr = getNewName(obj);
    if (objptr)
        return objptr;
    objptr = getOBCDFromObject(obj);
    if (!objptr)
        return 0;
    return objptr + objptr[18];
}

static int convertNameMessage(uint8_t *dst, int dstSize, int var)
{
    int num = readVar((unsigned)var);
    if (num) {
        const uint8_t *ptr = getObjOrActorName(num);
        if (ptr)
            return convertMessageToString(ptr, dst, dstSize);
    }
    return 0;
}

static int convertStringMessage(uint8_t *dst, int dstSize, int var)
{
    /* v4: the parameter is the string number itself */
    if (var > 0 && var < NUM_STRINGS && g.strings[var][0])
        return convertMessageToString(g.strings[var], dst, dstSize);
    return 0;
}

static int convertMessageToString(const uint8_t *msg, uint8_t *dst, int dstSize)
{
    unsigned num = 0;
    unsigned val;
    uint8_t chr;
    uint8_t *start = dst, *end = dst + dstSize;

    if (!msg)
        return 0;
    for (;;) {
        chr = msg[num++];
        if (chr == 0)
            break;
        if (chr == 0xFF) {
            chr = msg[num++];
            if (chr == 1 || chr == 2 || chr == 3 || chr == 8) {
                *dst++ = 0xFF;
                *dst++ = chr;
            } else {
                val = msg[num] | (msg[num + 1] << 8);
                switch (chr) {
                case 4: dst += convertIntMessage(dst, (int)(end - dst), (int)val); break;
                case 5: dst += convertVerbMessage(dst, (int)(end - dst), (int)val); break;
                case 6: dst += convertNameMessage(dst, (int)(end - dst), (int)val); break;
                case 7: dst += convertStringMessage(dst, (int)(end - dst), (int)val); break;
                case 9: case 10: case 12: case 13: case 14:
                    *dst++ = 0xFF;
                    *dst++ = chr;
                    *dst++ = msg[num + 0];
                    *dst++ = msg[num + 1];
                    break;
                default:
                    num -= 2;
                }
                num += 2;
            }
        } else if (chr != '@') {
            *dst++ = chr;
        }
        if (dst >= end - 4) {
            plat_error("convertMessageToString: buffer overflow");
            break;
        }
    }
    *dst = 0;
    return (int)(dst - start);
}

/* 0 at the end of the message part (c = 2 keep the text, 3 wait) */
static int handleNextCharsetCode(Actor *a, int *code)
{
    int c = 0, oldy, endLoop = 0;
    uint8_t *buffer = charsetBuffer + charsetBufPos;
    while (!endLoop) {
        c = *buffer++;
        if (!(c == 0xFF || c == 0xFE))
            break;
        c = *buffer++;
        switch (c) {
        case 1:
            c = 13;
            msgCount = g.screenWidth;
            endLoop = 1;
            break;
        case 2:
            g.haveMsg = 0;
            keepText = 1;
            endLoop = 1;
            break;
        case 3:
            g.haveMsg = 0xFF;
            keepText = 0;
            msgCount = 0;
            endLoop = 1;
            break;
        case 8:
            break;
        case 9: {
            int frme = buffer[0] | (buffer[1] << 8);
            buffer += 2;
            if (a)
                actor_startAnim(a, frme);
            break;
        }
        case 10:                                             /* talkie: none in v4 floppy */
            buffer += 14;
            break;
        case 12: {
            int color = buffer[0] | (buffer[1] << 8);
            buffer += 2;
            setColor(color == 0xFF ? charsetColor : color);
            break;
        }
        case 13:
            buffer += 2;
            break;
        case 14:
            oldy = cs.fontHeight;
            setCurID(*buffer++);
            buffer += 2;
            nextTop -= cs.fontHeight - oldy;
            break;
        default:
            plat_error("handleNextCharsetCode: invalid code");
            endLoop = 1;
        }
    }
    charsetBufPos = (int)(buffer - charsetBuffer);
    *code = c;
    return c != 2 && c != 3;
}

static int newLine(void)
{
    nextLeft = g.string[0].xpos;
    if (cs.center) {
        int stringWidth = text_getStringWidth(0, charsetBuffer + charsetBufPos);
        nextLeft -= stringWidth / 2;
        if (nextLeft < 0)
            nextLeft = 0;
    }
    if (g.string[0].height)
        nextTop += g.string[0].height;
    else
        nextTop += cs.fontHeight;
    cs.disableOffsX = 1;
    return 1;
}

void displayDialog(void)
{
    Actor *a = 0;
    int c = 0, maxWidth;

    if (!g.haveMsg)
        return;
    if ((g.camera.dest_x / 8) != (g.camera.cur_x / 8) || g.camera.cur_x != g.camera.last_x)
        return;

    if (getTalkingActor() != 0xFF && isValidActor(getTalkingActor()))
        a = &g.actors[getTalkingActor()];    /* derefActorSafe: actor 0 is valid */

    if (a && g.string[0].overhead) {
        g.string[0].xpos = (int16_t)(a->x - g.mainXStart);
        g.string[0].ypos = (int16_t)(a->y - a->elevation);
        if (VAR(VAR_V5_TALK_STRING_Y) < 0)
            g.string[0].ypos = (int16_t)(VAR(VAR_V5_TALK_STRING_Y) + a->y + a->elevation);
        else
            g.string[0].ypos = (int16_t)VAR(VAR_V5_TALK_STRING_Y);
        if (g.string[0].ypos < 1)
            g.string[0].ypos = 1;
        if (g.string[0].xpos < 80)
            g.string[0].xpos = 80;
        if (g.string[0].xpos > g.screenWidth - 80)
            g.string[0].xpos = (int16_t)(g.screenWidth - 80);
    }

    cs.top = g.string[0].ypos;
    cs.startLeft = cs.left = g.string[0].xpos;
    cs.right = g.string[0].right;
    cs.center = g.string[0].center;
    setColor(charsetColor);
    setCurID(g.string[0].charset);           /* a->_charset is 0 in v4 */

    if (g.talkDelay)
        return;

    if (g.haveMsg == 1) {
        stopTalk();
        return;
    }

    if (a && !g.string[0].no_talk_anim) {
        runActorTalkScript(a, a->talkStartFrame);
        useTalkAnims = 1;
    }

    g.talkDelay = 60;                        /* VAR_DEFAULT_TALK_DELAY does not exist in v4 */

    if (!keepText) {
        restoreCharsetBg();
        msgCount = 0;
    }

    maxWidth = cs.right - g.string[0].xpos - 1;
    if (cs.center) {
        if (maxWidth > nextLeft)
            maxWidth = nextLeft;
        maxWidth *= 2;
    }
    addLinebreaks(0, charsetBuffer + charsetBufPos, 0, maxWidth);

    if (cs.center) {
        nextLeft -= text_getStringWidth(0, charsetBuffer + charsetBufPos) / 2;
        if (nextLeft < 0)
            nextLeft = 0;
    }

    cs.disableOffsX = cs.firstChar = !keepText;

    while (handleNextCharsetCode(a, &c)) {
        if (c == 0) {
            g.haveMsg = 1;
            keepText = 0;
            msgCount = 0;
            break;
        }
        if (c == 13) {
            if (!newLine())
                break;
            continue;
        }
        cs.left = nextLeft;
        cs.top = nextTop;
        printChar(c, 0);
        nextLeft = cs.left;
        nextTop = cs.top;
        g.talkDelay += VAR(VAR_CHARINC);
    }
}

void actorTalk(const uint8_t *msg)
{
    Actor *a;
    convertMessageToString(msg, charsetBuffer, sizeof(charsetBuffer));

    if (g.actorToPrintStrFor == 0xFF) {
        if (!keepText)
            stopTalk();
        setTalkingActor(0xFF);
    } else {
        int oldact;
        a = derefActor(g.actorToPrintStrFor);
        if (!actor_inCurrentRoom(a)) {
            oldact = 0xFF;
        } else {
            if (!keepText)
                stopTalk();
            setTalkingActor(a->number);
            if (!g.string[0].no_talk_anim) {
                runActorTalkScript(a, a->talkStartFrame);
                useTalkAnims = 1;
            }
            oldact = getTalkingActor();
        }
        if (oldact >= 0x80)
            return;
    }

    if (getTalkingActor() > 0x7F)
        charsetColor = g.string[0].color;
    else
        charsetColor = derefActor(getTalkingActor())->talkColor;

    charsetBufPos = 0;
    g.talkDelay = 0;
    g.haveMsg = 0xFF;
    VAR(VAR_HAVE_MSG) = 0xFF;
    displayDialog();
}

/* drawString: text in a slot (1 the scripts' text, 4 a verb), the layout without the pixels:
 * where it ends (the slot's xpos, the verb's rectangle) is what the game uses */
static void drawString(int a, const uint8_t *msg)
{
    uint8_t buf[270], *space, *tmp;
    int i, c, fontHeight;
    TextSlot *st = &g.string[a];

    convertMessageToString(msg, buf, sizeof(buf));
    cs.top = st->ypos;
    cs.startLeft = cs.left = st->xpos;
    cs.right = st->right;
    cs.center = st->center;
    setColor(st->color);
    cs.disableOffsX = cs.firstChar = 1;
    setCurID(st->charset);
    fontHeight = cs.fontHeight;

    space = 0;                               /* v4: trailing spaces are cut */
    for (tmp = buf; *tmp; tmp++) {
        if (*tmp == ' ') {
            if (!space)
                space = tmp;
        } else {
            space = 0;
        }
    }
    if (space)
        *space = 0;

    if (cs.center)
        cs.left -= text_getStringWidth(a, buf) / 2;

    if (!buf[0]) {
        str.left = str.right = cs.left;
        str.top = str.bottom = cs.top;
    }

    for (i = 0; (c = buf[i++]) != 0;) {
        if (c == 0xFF || c == 0xFE) {
            c = buf[i++];
            switch (c) {
            case 9: case 10: case 13: case 14:
                i += 2;
                break;
            case 1: case 8:
                if (cs.center)
                    cs.left = cs.startLeft - text_getStringWidth(a, buf + i);
                else
                    cs.left = cs.startLeft;
                if (g.string[0].height)
                    nextTop += g.string[0].height;
                else
                    cs.top += fontHeight;
                break;
            case 12: {
                int color = buf[i] + (buf[i + 1] << 8);
                i += 2;
                setColor(color == 0xFF ? st->color : color);
                break;
            }
            default:
                break;
            }
        } else {
            printChar(c, 1);
        }
    }
    if (a == 0) {
        nextLeft = cs.left;
        nextTop = cs.top;
    }
    st->xpos = (int16_t)str.right;
}

/* restoreVerbBG: the rectangle the verb was drawn in, back to the background */
static void restoreVerbBG(int verb)
{
    VerbSlot *vs = &g.verbs[verb];
    if (vs->oldLeft != -1) {
        restoreBackground(vs->oldLeft, vs->oldTop, vs->oldRight, vs->oldBottom, vs->bkcolor);
        vs->oldLeft = -1;
    }
}

/* drawVerb: a text verb that is on and not saved is drawn in its colour (mode 1: the
 * highlight), and takes its rectangle from its text; otherwise its old rectangle is cleared */
void drawVerb(int verb, int mode)
{
    VerbSlot *vs;
    int tmp;
    if (!verb)
        return;
    vs = &g.verbs[verb];
    if (!vs->saveid && vs->curmode && vs->verbid) {
        if (vs->type == 1) {                 /* kImageVerbType */
            plat_error("drawVerb: image verbs not yet implemented");
            return;
        }
        restoreVerbBG(verb);
        g.string[4].charset = vs->charset;
        g.string[4].xpos = vs->x;
        g.string[4].ypos = vs->y;
        g.string[4].right = (int16_t)(g.screenWidth - 1);
        g.string[4].center = vs->center;
        if (vs->curmode == 2)
            g.string[4].color = vs->dimcolor;
        else if (mode && vs->hicolor)
            g.string[4].color = vs->hicolor;
        else
            g.string[4].color = vs->color;
        if (!vs->hasName)
            return;
        tmp = cs.center;
        drawString(4, vs->name);
        cs.center = tmp;
        vs->right = (int16_t)str.right;
        vs->bottom = (int16_t)str.bottom;
        vs->oldLeft = (int16_t)str.left;
        vs->oldTop = (int16_t)str.top;
        vs->oldRight = (int16_t)str.right;
        vs->oldBottom = (int16_t)str.bottom;
        str.left = str.right;
    } else {
        restoreVerbBG(verb);
    }
}

/* verbMouseOver: the old highlight drawn plain, the new one highlighted */
void verbMouseOver(int verb)
{
    if (g.verbMouseOver != verb) {
        if (g.verbs[g.verbMouseOver].type != 1) {
            drawVerb(g.verbMouseOver, 0);
            g.verbMouseOver = verb;
        }
        if (g.verbs[verb].type != 1 && g.verbs[verb].hicolor) {
            drawVerb(verb, 1);
            g.verbMouseOver = verb;
        }
    }
}

/* --- the message banner (ScummVM's original GUI: isUsingOriginalGUI, its default) --------- */

/* drawGUIText (gfx_gui.cpp): string slot 5, charset 1 (v > 3), no clip rectangle */
static void drawGUIText(const uint8_t *buttonString, int textXPos, int textYPos, int textColor,
                        int centerFlag)
{
    const int tmpRight = g.string[5].right;
    g.string[5].xpos = (int16_t)textXPos;
    g.string[5].ypos = (int16_t)textYPos;
    g.string[5].right = (int16_t)(g.screenWidth - 1);
    g.string[5].center = (uint8_t)centerFlag;
    g.string[5].color = (uint8_t)textColor;
    g.string[5].charset = 1;
    drawString(5, buttonString);
    g.string[5].right = (int16_t)tmpRight;
}

/* showOldStyleBannerAndPause (gfx_gui.cpp), DOS v4, waitTime -1 (until a key or a click): the
 * banner over the screen's rows 80-91, the engine paused (no round runs, the sound stops),
 * then everything as it was.  Returns the key's ASCII, 0 for a click.  The shake is switched
 * off and not back on, as ScummVM (the port shows no shake yet). */
static int showOldStyleBannerAndPause(const uint8_t *msg, int color)
{
    uint8_t bannerMsg[512];
    int oldId, bannerMsgHeight, startingPointY, ks;
    const int mainTop = scumm_virtscrTop(0);

    convertMessageToString(msg, bannerMsg, sizeof(bannerMsg));
    plat_saveSurfacesPreGUI();               /* not _mainMenuIsActive */
    if (g.currentRoom != 0 && cs.textScreenID == 0)
        restoreCharsetBg();
    plat_pauseEngine(1);
    g.forceBannerVirtScreen = 1;
    oldId = cs.curId;
    setCurID(1);
    bannerMsgHeight = scumm_virtscrHeight(VIRT_BANNER);
    startingPointY = scumm_virtscrTop(VIRT_BANNER);
    /* bannerMsgWidth (getGUIStringWidth, at least 100): FM-Towns only */
    plat_fillVirtScreen(VIRT_BANNER, 0);      /* boxColor 0 */
    drawLine(0, startingPointY, g.screenWidth - 1, startingPointY, color);
    drawLine(0, startingPointY + bannerMsgHeight - 1, g.screenWidth - 1,
             startingPointY + bannerMsgHeight - 1, color);
    drawGUIText(bannerMsg, g.screenWidth / 2, startingPointY + 2, color, 1);
    g.forceBannerVirtScreen = 0;
    plat_drawDirtyScreenPartsOnly();
    plat_updateDirtyScreen(VIRT_BANNER);
    ks = plat_waitBannerInput();
    plat_fillVirtScreen(VIRT_BANNER, 0);
    plat_setDirtyRange(VIRT_BANNER, 0, bannerMsgHeight);
    plat_updateDirtyScreen(VIRT_BANNER);
    plat_setDirtyRange(0, startingPointY - mainTop, startingPointY - mainTop + bannerMsgHeight);
    plat_updateDirtyScreen(0);
    plat_restoreSurfacesPostGUI();
    g.postGUICharMask = 1;                   /* the text surface came back (not LOOM) */
    plat_pauseEngine(0);
    clearClickedStatus();
    if (oldId)
        setCurID(oldId);
    return ks;
}

/* processKeyboard's pause key with the original GUI (input.cpp): "Game Paused.  Press SPACE to
 * Continue." (getGUIString(gsPause): the game's string 4, which the boot script sets) */
void text_pauseBanner(void)
{
    (void)showOldStyleBannerAndPause(g.strings[4], 12);
}

/* showMessageDialog (string.cpp): a script's print(252, ...), "Insert Disk 22..." at the
 * stump in the forest, the debug keys' banners */
static void showMessageDialog(const uint8_t *msg)
{
    uint8_t buf[500];
    convertMessageToString(msg, buf, sizeof(buf));
    if (g.string[3].color == 0)
        g.string[3].color = 4;
    VAR(VAR_KEYPRESS) = showOldStyleBannerAndPause(msg, g.string[3].color);
}

/* printString: slot 0 talks, 1 draws text, 2 is a debug message, 3 a banner */
void printString(int m, const uint8_t *msg)
{
    switch (m) {
    case 0:
        actorTalk(msg);
        break;
    case 1:
        drawString(1, msg);
        break;
    case 2:                                  /* debugMessage */
        break;
    case 3:
        showMessageDialog(msg);
        break;
    default:
        break;
    }
}

/* --- save and load: the charset renderer and the message being said ---------------------- */
void text_saveLoad(Ser *s)
{
    SER(s, cs.curId); SER(s, cs.left); SER(s, cs.top); SER(s, cs.right); SER(s, cs.startLeft);
    SER(s, cs.center); SER(s, cs.disableOffsX); SER(s, cs.firstChar); SER(s, cs.hasMask);
    SER(s, cs.color); SER(s, cs.textScreenID);
    ser_bytes(s, charsetBuffer, sizeof(charsetBuffer));
    SER(s, charsetBufPos); SER(s, keepText); SER(s, msgCount); SER(s, nextLeft); SER(s, nextTop);
    SER(s, useTalkAnims); SER(s, charsetColor);
    SER(s, str.left); SER(s, str.top); SER(s, str.right); SER(s, str.bottom);
}

void text_afterLoad(void)
{
    const int id = cs.curId;
    cs.curId = -1;
    setCurID(id);                            /* the font pointer from the charset number */
    cs.hasMask = 0;
    plat_clearTextSurface();
}

/* redrawVerbs (verbs.cpp): every verb drawn again, the one under the cursor highlighted (a
 * load's ScummEngine_v5::scummLoop_handleSaveLoad) */
void redrawVerbs(void)
{
    int i, verb = 0, x, y;
    if (g.cursor.state > 0) {
        scumm_mousePos(&x, &y);
        verb = findVerbAtPos(x, y);
    }
    for (i = 0; i < NUM_VERBS; i++) {
        if (i == verb && g.verbs[verb].hicolor)
            drawVerb(i, 1);
        else
            drawVerb(i, 0);
    }
    g.verbMouseOver = verb;
}
