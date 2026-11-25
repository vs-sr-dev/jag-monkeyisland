/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * ops_v5.c - the opcodes, v5 with the v4 overrides (ScummVM script_v5.cpp, script_v4.cpp).
 *
 * The 256-entry dispatch table and the opcode names are generated from ScummVM's
 * setupOpcodes by tools/gen_opcodes.py into opcodes_gen.h: every o5_ / o4_ function defined
 * here is wired in; any other opcode halts the interpreter with its name, so the trace shows
 * exactly where the next missing piece is.
 */
#include <stdio.h>
#include <string.h>
#include "scumm.h"

#define PARAM_1 0x80
#define PARAM_2 0x40
#define PARAM_3 0x20

static int getVar(void) { return readVar(fetchScriptWord()); }

static int getVarOrDirectByte(uint8_t mask)
{
    if (g.opcode & mask)
        return getVar();
    return fetchScriptByte();
}

static int getVarOrDirectWord(uint8_t mask)
{
    if (g.opcode & mask)
        return getVar();
    return fetchScriptWordSigned();
}

static void getResultPos(void)
{
    int a;
    g.resultVarNumber = (int)fetchScriptWord();
    if (g.resultVarNumber & 0x2000) {
        a = (int)fetchScriptWord();
        if (a & 0x2000)
            g.resultVarNumber += readVar(a & ~0x2000);
        else
            g.resultVarNumber += a & 0xFFF;
        g.resultVarNumber &= ~0x2000;
    }
}

static void setResult(int value) { writeVar((unsigned)g.resultVarNumber, value); }

static void jumpRelative(int cond)
{
    int16_t offset = (int16_t)fetchScriptWord();
    if (!cond)
        g.scriptPointer += offset;
}

static int getWordVararg(int32_t *ptr)
{
    int i;
    for (i = 0; i < NUM_SCRIPT_LOCAL; i++)
        ptr[i] = 0;
    i = 0;
    while ((g.opcode = fetchScriptByte()) != 0xFF)
        ptr[i++] = getVarOrDirectWord(PARAM_1);
    return i;
}

static void halt(const char *what)
{
    if (!g.halted) {
        g.halted = 1;
        snprintf(g.haltMsg, sizeof(g.haltMsg), "%s", what);
    }
    g.currentScript = 0xFF;
}

/* --- strings (rtString resources as fixed buffers) ---------------------------- */

/* loadPtrToResource(rtString, idx, src): src NULL = the string inline in the script */
static void loadString(int idx, const uint8_t *src)
{
    int len;
    const uint8_t *from = src ? src : g.scriptPointer;
    len = resStrLen(from) + 1;
    if (idx < 0 || idx >= NUM_STRINGS || len > STRING_MAX) {
        halt("loadString: string out of range");
        return;
    }
    memcpy(g.strings[idx], from, (size_t)len);
    if (!src)
        g.scriptPointer += len;
}

/* --- arithmetic, comparisons, jumps -------------------------------------------- */

static void o5_move(void)      { getResultPos(); setResult(getVarOrDirectWord(PARAM_1)); }
static void o5_add(void)       { int a; getResultPos(); a = getVarOrDirectWord(PARAM_1); setResult(readVar(g.resultVarNumber) + a); }
static void o5_subtract(void)  { int a; getResultPos(); a = getVarOrDirectWord(PARAM_1); setResult(readVar(g.resultVarNumber) - a); }
static void o5_multiply(void)  { int a; getResultPos(); a = getVarOrDirectWord(PARAM_1); setResult(readVar(g.resultVarNumber) * a); }
static void o5_divide(void)
{
    int a;
    getResultPos();
    a = getVarOrDirectWord(PARAM_1);
    if (a == 0) { halt("Divide by zero"); return; }
    setResult(readVar(g.resultVarNumber) / a);
}
static void o5_and(void)       { int a; getResultPos(); a = getVarOrDirectWord(PARAM_1); setResult(readVar(g.resultVarNumber) & a); }
static void o5_or(void)        { int a; getResultPos(); a = getVarOrDirectWord(PARAM_1); setResult(readVar(g.resultVarNumber) | a); }
static void o5_increment(void) { getResultPos(); setResult(readVar(g.resultVarNumber) + 1); }
static void o5_decrement(void) { getResultPos(); setResult(readVar(g.resultVarNumber) - 1); }

static void o5_isEqual(void)
{
    int16_t a, b;
    unsigned var = fetchScriptWord();
    a = (int16_t)readVar(var);
    b = (int16_t)getVarOrDirectWord(PARAM_1);
    jumpRelative(b == a);
}
static void o5_isNotEqual(void)     { int16_t a = (int16_t)getVar(); int16_t b = (int16_t)getVarOrDirectWord(PARAM_1); jumpRelative(b != a); }
static void o5_isGreater(void)      { int16_t a = (int16_t)getVar(); int16_t b = (int16_t)getVarOrDirectWord(PARAM_1); jumpRelative(b > a); }
static void o5_isGreaterEqual(void) { int16_t a = (int16_t)getVar(); int16_t b = (int16_t)getVarOrDirectWord(PARAM_1); jumpRelative(b >= a); }
static void o5_isLess(void)         { int16_t a = (int16_t)getVar(); int16_t b = (int16_t)getVarOrDirectWord(PARAM_1); jumpRelative(b < a); }
static void o5_isLessEqual(void)
{
    unsigned var = fetchScriptWord();
    int16_t a = (int16_t)readVar(var);
    int16_t b = (int16_t)getVarOrDirectWord(PARAM_1);
    jumpRelative(b <= a);
}
static void o5_equalZero(void)    { int a = getVar(); jumpRelative(a == 0); }
static void o5_notEqualZero(void) { int a = getVar(); jumpRelative(a != 0); }
static void o5_jumpRelative(void) { jumpRelative(0); }

static void o5_getRandomNr(void)
{
    int max;
    getResultPos();
    max = getVarOrDirectByte(PARAM_1);
    setResult((int)scumm_random((unsigned)max));
}

static void o5_setVarRange(void)
{
    int a, b;
    getResultPos();
    a = fetchScriptByte();
    do {
        if (g.opcode & 0x80)
            b = fetchScriptWordSigned();
        else
            b = fetchScriptByte();
        setResult(b);
        g.resultVarNumber++;
    } while (--a);
}

static void o5_expression(void)
{
    int dst, i;
    g.stackPos = 0;
    getResultPos();
    dst = g.resultVarNumber;
    while ((g.opcode = fetchScriptByte()) != 0xFF) {
        switch (g.opcode & 0x1F) {
        case 1: push(getVarOrDirectWord(PARAM_1)); break;
        case 2: i = pop(); push(i + pop()); break;
        case 3: i = pop(); push(pop() - i); break;
        case 4: i = pop(); push(i * pop()); break;
        case 5:
            i = pop();
            if (i == 0) { halt("Divide by zero"); return; }
            push(pop() / i);
            break;
        case 6:
            g.opcode = fetchScriptByte();
            executeOpcode(g.opcode);
            push(g.vars[0]);
            break;
        default:
            break;
        }
    }
    g.resultVarNumber = dst;
    setResult(pop());
}

/* --- script control ---------------------------------------------------------- */

static void o5_breakHere(void)
{
    updateScriptPtr();
    g.currentScript = 0xFF;
}

static void o5_delay(void)
{
    int delay = fetchScriptByte();
    delay |= fetchScriptByte() << 8;
    delay |= fetchScriptByte() << 16;
    g.slot[g.currentScript].delay = delay;
    g.slot[g.currentScript].status = SS_PAUSED;
    o5_breakHere();
}

static void o5_delayVariable(void)
{
    g.slot[g.currentScript].delay = getVar();
    g.slot[g.currentScript].status = SS_PAUSED;
    o5_breakHere();
}

static void o5_stopObjectCode(void) { stopObjectCode(); }

static void o5_startScript(void)
{
    int op = g.opcode, script;
    int32_t data[NUM_SCRIPT_LOCAL];
    script = getVarOrDirectByte(PARAM_1);
    getWordVararg(data);
    /* The copy-protection screen (script 152), as ScummVM: skipped without copy protection
     * (the game's own later releases did so: KIXX XL, LucasArts Classic Adventures).  The
     * port keeps it (the user: the Dial-A-Pirate wheel is part of the game); the oracle runs
     * without it unless asked (oracle_scummvm.ps1 -CopyProtection, scumm_pc --copyprot 1). */
    if (!g.copyProtection && script == 152)
        return;
    runScript(script, (op & 0x20) != 0, (op & 0x40) != 0, data);
}

static void o5_chainScript(void)
{
    int32_t vars[NUM_SCRIPT_LOCAL];
    int script, cur;
    script = getVarOrDirectByte(PARAM_1);
    getWordVararg(vars);
    cur = g.currentScript;
    g.slot[cur].number = 0;
    g.slot[cur].status = SS_DEAD;
    g.currentScript = 0xFF;
    runScript(script, g.slot[cur].freezeResistant, g.slot[cur].recursive, vars);
}

static void o5_stopScript(void)
{
    int script = getVarOrDirectByte(PARAM_1);
    if (!script)
        stopObjectCode();
    else
        stopScript(script);
}

static void o5_stopObjectScript(void) { stopObjectScript(getVarOrDirectWord(PARAM_1)); }

static void o5_startObject(void)
{
    int obj, script;
    int32_t data[NUM_SCRIPT_LOCAL];
    obj = getVarOrDirectWord(PARAM_1);
    script = getVarOrDirectByte(PARAM_2);
    getWordVararg(data);
    runObjectScript(obj, script, 0, 0, data);
}

static void o5_isScriptRunning(void)
{
    getResultPos();
    setResult(isScriptRunning(getVarOrDirectByte(PARAM_1)));
}

static void o5_freezeScripts(void)
{
    int scr = getVarOrDirectByte(PARAM_1);
    if (scr != 0)
        freezeScripts(scr);
    else
        unfreezeScripts();
}

static void o5_cutscene(void)
{
    int32_t args[NUM_SCRIPT_LOCAL];
    getWordVararg(args);
    beginCutscene(args);
}

static void o5_endCutscene(void) { endCutscene(); }

static void o5_beginOverride(void)
{
    if (fetchScriptByte() != 0)
        beginOverride();
    else
        endOverride();
}

static void o5_doSentence(void)
{
    int verb, objectA, objectB;
    verb = getVarOrDirectByte(PARAM_1);
    if (verb == 0xFE) {
        g.sentenceNum = 0;
        stopScript(VAR(VAR_SENTENCE_SCRIPT));
        clearClickedStatus();
        return;
    }
    objectA = getVarOrDirectWord(PARAM_2);
    objectB = getVarOrDirectWord(PARAM_3);
    doSentence(verb, objectA, objectB);
}

static void o5_debug(void) { (void)getVarOrDirectWord(PARAM_1); }

/* --- resources, rooms -------------------------------------------------------- */

static void o5_resourceRoutines(void)
{
    int resid = 0, op;
    g.opcode = fetchScriptByte();
    if (g.opcode != 17)
        resid = getVarOrDirectByte(PARAM_1);
    op = g.opcode & 0x3F;
    (void)resid;
    switch (op) {
    case 20:                   /* SO_LOAD_OBJECT */
        (void)getVarOrDirectWord(PARAM_2);
        halt("o5_resourceRoutines: SO_LOAD_OBJECT (flobjects) not yet implemented");
        break;
    default:                   /* load, nuke, lock, unlock, heap, charsets: all in place */
        break;
    }
}

static void o5_loadRoom(void)
{
    int room = getVarOrDirectByte(PARAM_1);
    /* GF_SMALL_HEADER: only a room other than the current one is (re)loaded */
    if (room != g.currentRoom)
        startScene(room);
    g.fullRedraw = 1;
}

static void o5_pseudoRoom(void)
{
    int i = fetchScriptByte(), j;
    while ((j = fetchScriptByte()) != 0) {
        if (j >= 0x80)
            g.resourceMapper[j & 0x7F] = (uint8_t)i;
    }
}

static void o5_roomOps(void)
{
    int a = 0, b = 0;
    g.opcode = fetchScriptByte();
    switch (g.opcode & 0x1F) {
    case 1:                    /* SO_ROOM_SCROLL */
        a = getVarOrDirectWord(PARAM_1);
        b = getVarOrDirectWord(PARAM_2);
        if (a < g.screenWidth / 2) a = g.screenWidth / 2;
        if (b < g.screenWidth / 2) b = g.screenWidth / 2;
        if (a > g.roomWidth - g.screenWidth / 2) a = g.roomWidth - g.screenWidth / 2;
        if (b > g.roomWidth - g.screenWidth / 2) b = g.roomWidth - g.screenWidth / 2;
        VAR(VAR_CAMERA_MIN_X) = a;
        VAR(VAR_CAMERA_MAX_X) = b;
        break;
    case 2:                    /* SO_ROOM_COLOR */
        a = getVarOrDirectWord(PARAM_1);
        b = getVarOrDirectWord(PARAM_2);
        if (a < 0 || a > 256)
            plat_error("o5_roomOps: 2: room color slot");
        g.roomPalette[b & 0xFF] = (uint8_t)a;
        g.roomPaletteGen++;
        g.fullRedraw = 1;
        break;
    case 3:                    /* SO_ROOM_SCREEN */
        a = getVarOrDirectWord(PARAM_1);
        b = getVarOrDirectWord(PARAM_2);
        scumm_initScreens(a, b);
        break;
    case 4:                    /* SO_ROOM_PALETTE (GF_SMALL_HEADER: the shadow palette) */
        a = getVarOrDirectWord(PARAM_1);
        b = getVarOrDirectWord(PARAM_2);
        if (a < 0 || a > 256)
            plat_error("o5_roomOps: 4: room color slot");
        g.shadowPalette[b & 0xFF] = (uint8_t)a;
        setDirtyColors(b & 0xFF, b & 0xFF);
        break;
    case 5: case 6:            /* SO_ROOM_SHAKE_ON / OFF */
        break;
    case 7: {                  /* SO_ROOM_SCALE */
        int c, d, e;
        a = getVarOrDirectByte(PARAM_1);
        b = getVarOrDirectByte(PARAM_2);
        g.opcode = fetchScriptByte();
        c = getVarOrDirectByte(PARAM_1);
        d = getVarOrDirectByte(PARAM_2);
        g.opcode = fetchScriptByte();
        e = getVarOrDirectByte(PARAM_2);
        setScaleSlot(e - 1, 0, b, a, 0, d, c);
        break;
    }
    case 8: {                  /* SO_ROOM_INTENSITY */
        int c;
        a = getVarOrDirectByte(PARAM_1);
        b = getVarOrDirectByte(PARAM_2);
        c = getVarOrDirectByte(PARAM_3);
        darkenPalette(a, a, a, b, c);
        break;
    }
    case 9:                    /* SO_ROOM_SAVEGAME */
        (void)getVarOrDirectByte(PARAM_1);
        (void)getVarOrDirectByte(PARAM_2);
        halt("o5_roomOps: SO_ROOM_SAVEGAME not yet implemented");
        break;
    case 10:                   /* SO_ROOM_FADE */
        a = getVarOrDirectWord(PARAM_1);
        if (a) {
            g.switchRoomEffect = a & 0xFF;
            g.switchRoomEffect2 = (a >> 8) & 0xFF;
        } else {
            fadeIn(g.newEffect);
        }
        break;
    case 11: case 12: {        /* SO_RGB_ROOM_INTENSITY, SO_ROOM_SHADOW */
        int c, d, e, sub = g.opcode & 0x1F;
        a = getVarOrDirectWord(PARAM_1);
        b = getVarOrDirectWord(PARAM_2);
        c = getVarOrDirectWord(PARAM_3);
        g.opcode = fetchScriptByte();
        d = getVarOrDirectByte(PARAM_1);
        e = getVarOrDirectByte(PARAM_2);
        if (sub == 11)
            darkenPalette(a, b, c, d, e);
        else
            setShadowPalette(a, b, c, d, e, 0, 256);
        break;
    }
    case 13: case 14:          /* SO_SAVE_STRING, SO_LOAD_STRING */
        (void)getVarOrDirectByte(PARAM_1);
        while (fetchScriptByte())
            ;
        halt("o5_roomOps: save/load string not yet implemented");
        break;
    case 15: {                 /* SO_ROOM_TRANSFORM */
        int c, d;
        a = getVarOrDirectByte(PARAM_1);
        g.opcode = fetchScriptByte();
        b = getVarOrDirectByte(PARAM_1);
        c = getVarOrDirectByte(PARAM_2);
        g.opcode = fetchScriptByte();
        d = getVarOrDirectByte(PARAM_1);
        palManipulateInit(a, b, c, d);
        break;
    }
    case 16:                   /* SO_CYCLE_SPEED (GF_SMALL_HEADER's cyclePalette ignores it) */
        a = getVarOrDirectByte(PARAM_1);
        b = getVarOrDirectByte(PARAM_2);
        if (a < 1 || a > 16)
            plat_error("o5_roomOps: 16: color cycle");
        g.colorCycle[a - 1].delay = (uint16_t)(b != 0 ? 0x4000 / (b * 0x4C) : 0);
        break;
    default:
        halt("o5_roomOps: unknown subopcode");
    }
}

/* o5_systemOps: MI1 uses only SO_QUIT (script 152, the third wrong answer at the
 * Dial-A-Pirate wheel; sc_001's is in the debug boot path).  quitGame sets _quitRequested at
 * once: the scripts run on to their breakHere, scummLoop returns after
 * checkAndRunSentenceScript, and ScummEngine::go ends; the platform reads g.quit. */
static void o5_systemOps(void)
{
    int subOp = fetchScriptByte();
    switch (subOp) {
    case 3:                                      /* SO_QUIT */
        g.quit = 1;
        break;
    case 1:                                      /* SO_RESTART: not in MI1's scripts */
        halt("o5_systemOps: SO_RESTART");
        break;
    case 2:                                      /* SO_PAUSE: not in MI1's scripts */
        halt("o5_systemOps: SO_PAUSE");
        break;
    default:
        halt("o5_systemOps: unknown subopcode");
    }
}

static void o4_oldRoomEffect(void)
{
    int a;
    g.opcode = fetchScriptByte();
    if ((g.opcode & 0x1F) == 3) {
        a = getVarOrDirectWord(PARAM_1);
        if (a) {
            g.switchRoomEffect = a & 0xFF;
            g.switchRoomEffect2 = (a >> 8) & 0xFF;
        } else {
            fadeIn(g.newEffect);
        }
    }
}

static void o5_lights(void)
{
    int a, c;
    a = getVarOrDirectByte(PARAM_1);
    (void)fetchScriptByte();
    c = fetchScriptByte();
    if (c == 0)
        VAR(VAR_CURRENT_LIGHTS) = a;
    /* c == 1: flashlight size, unused by MI1 */
    g.fullRedraw = 1;
}

static void o5_setCameraAt(void)
{
    int at = getVarOrDirectWord(PARAM_1);       /* setCameraAtEx, v < 7 */
    g.camera.mode = 1;
    g.camera.cur_x = at;
    setCameraAt(at, 0);
}

/* --- objects ------------------------------------------------------------------ */

static void o5_setClass(void)
{
    int obj = getVarOrDirectWord(PARAM_1), cls;
    while ((g.opcode = fetchScriptByte()) != 0xFF) {
        cls = getVarOrDirectWord(PARAM_1);
        if (cls == 0) {
            g.classData[obj] = 0;
            if (obj >= 1 && obj < NUM_ACTORS) {
                g.actors[obj].ignoreBoxes = 0;
                g.actors[obj].forceClip = 0;
            }
        } else {
            putClass(obj, cls, (cls & 0x80) ? 1 : 0);
        }
    }
}

static void o5_ifClassOfIs(void)
{
    int obj, cls, cond = 1, b;
    obj = getVarOrDirectWord(PARAM_1);
    while ((g.opcode = fetchScriptByte()) != 0xFF) {
        cls = getVarOrDirectWord(PARAM_1);
        b = getClass(obj, cls);
        if (((cls & 0x80) && !b) || (!(cls & 0x80) && b))
            cond = 0;
    }
    jumpRelative(cond);
}

static void o5_setState(void)
{
    int obj = getVarOrDirectWord(PARAM_1);
    int state = getVarOrDirectByte(PARAM_2);
    putState(obj, state);
    markObjectRectAsDirty(obj);
    if (gfx_bgNeedsRedraw())
        clearDrawObjectQueue();
}

/* o5_drawObject, GF_SMALL_HEADER: x and y (in strips/8-pixel units, 255 = unchanged) */
static void o5_drawObject(void)
{
    int state = 1, obj, idx, i, xpos, ypos;
    ObjectData *od;
    int x, y, w, h;

    obj = getVarOrDirectWord(PARAM_1);
    xpos = getVarOrDirectWord(PARAM_2);
    ypos = getVarOrDirectWord(PARAM_3);

    idx = getObjectIndex(obj);
    if (idx == -1)
        return;
    od = &g.objs[idx];
    if (xpos != 0xFF) {
        od->walk_x += (int16_t)((xpos * 8) - od->x_pos);
        od->x_pos = (int16_t)(xpos * 8);
        od->walk_y += (int16_t)((ypos * 8) - od->y_pos);
        od->y_pos = (int16_t)(ypos * 8);
    }
    addObjectToDrawQue(idx);
    x = od->x_pos;
    y = od->y_pos;
    w = od->width;
    h = od->height;
    i = NUM_LOCAL_OBJECTS - 1;
    do {
        if (g.objs[i].obj_nr && g.objs[i].x_pos == x && g.objs[i].y_pos == y &&
            g.objs[i].width == w && g.objs[i].height == h)
            putState(g.objs[i].obj_nr, 0);
    } while (--i);
    putState(obj, state);
}

static void o4_ifState(void)
{
    int a = getVarOrDirectWord(PARAM_1);
    int b = getVarOrDirectByte(PARAM_2);
    jumpRelative(getState(a) == b);
}

static void o4_ifNotState(void)
{
    int a = getVarOrDirectWord(PARAM_1);
    int b = getVarOrDirectByte(PARAM_2);
    jumpRelative(getState(a) != b);
}

static void o5_getObjectOwner(void)
{
    getResultPos();
    setResult(getOwner(getVarOrDirectWord(PARAM_1)));
}

/* --- actors (script-visible state) --------------------------------------------- */

static void o5_putActor(void)
{
    int act = getVarOrDirectByte(PARAM_1);
    int x = getVarOrDirectWord(PARAM_2);
    int y = getVarOrDirectWord(PARAM_3);
    Actor *a = derefActor(act);
    actor_put(a, x, y, a->room);
}

static void o5_putActorInRoom(void)
{
    int act = getVarOrDirectByte(PARAM_1);
    int room = getVarOrDirectByte(PARAM_2);
    Actor *a = derefActor(act);
    if (a->visible && g.currentRoom != room && text_isTalking(a->number))
        stopTalk();
    a->room = (int16_t)room;
    if (!room)
        actor_put(a, 0, 0, 0);
}

static void o5_getActorRoom(void)
{
    int act;
    getResultPos();
    act = getVarOrDirectByte(PARAM_1);
    if (!isValidActor(act)) {
        setResult(0);
        return;
    }
    setResult(g.actors[act].room);
}

static void o5_animateActor(void)
{
    int act = getVarOrDirectByte(PARAM_1);
    int anim = getVarOrDirectByte(PARAM_2);
    if (!isValidActor(act))
        return;
    actor_animate(derefActor(act), anim);
}

static void o5_actorOps(void)
{
    static const uint8_t convertTable[20] =
        { 1, 0, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 20 };
    int act = getVarOrDirectByte(PARAM_1), i, j;
    Actor *a = derefActor(act);

    while ((g.opcode = fetchScriptByte()) != 0xFF) {
        g.opcode = (uint8_t)((g.opcode & 0xE0) | convertTable[(g.opcode & 0x1F) - 1]);
        switch (g.opcode & 0x1F) {
        case 0:  (void)getVarOrDirectByte(PARAM_1); break;
        case 1:  actor_setCostume(a, getVarOrDirectByte(PARAM_1)); break;
        case 2:  i = getVarOrDirectByte(PARAM_1); j = getVarOrDirectByte(PARAM_2);
                 actor_setWalkSpeed(a, i, j); break;
        case 3:  a->sound = (uint8_t)getVarOrDirectByte(PARAM_1); break;
        case 4:  a->walkFrame = (uint8_t)getVarOrDirectByte(PARAM_1); break;
        case 5:  a->talkStartFrame = (uint8_t)getVarOrDirectByte(PARAM_1);
                 a->talkStopFrame = (uint8_t)getVarOrDirectByte(PARAM_2); break;
        case 6:  a->standFrame = (uint8_t)getVarOrDirectByte(PARAM_1); break;
        case 7:  (void)getVarOrDirectByte(PARAM_1); (void)getVarOrDirectByte(PARAM_2);
                 (void)getVarOrDirectByte(PARAM_3); break;
        case 8:  actor_init(a, 0); break;
        case 9:  a->elevation = (int16_t)getVarOrDirectWord(PARAM_1); break;
        case 10: a->initFrame = 1; a->walkFrame = 2; a->standFrame = 3;
                 a->talkStartFrame = 4; a->talkStopFrame = 5; break;
        case 11: i = getVarOrDirectByte(PARAM_1); j = getVarOrDirectByte(PARAM_2);
                 if (i < 0 || i > 31)
                     plat_error("o5_actorOps: palette slot");
                 a->palette[i] = (uint8_t)j;       /* Actor::setPalette */
                 a->needRedraw = 1;
                 break;
        case 12: a->talkColor = (uint8_t)getVarOrDirectByte(PARAM_1); break;
        case 13: {                                 /* SO_ACTOR_NAME */
            int len = resStrLen(g.scriptPointer) + 1;
            if (len > (int)sizeof(a->name)) {
                plat_error("SO_ACTOR_NAME: the name does not fit");
                return;
            }
            memcpy(a->name, g.scriptPointer, (size_t)len);
            g.scriptPointer += len;
            break;
        }
        case 14: a->initFrame = (uint8_t)getVarOrDirectByte(PARAM_1); break;
        case 16: a->width = (uint8_t)getVarOrDirectByte(PARAM_1); break;
        case 17: i = getVarOrDirectByte(PARAM_1);  /* v4: one parameter */
                 a->boxscale = (uint8_t)i; a->scalex = a->scaley = (uint8_t)i; break;
        case 18: a->forceClip = 0; break;
        case 19: a->forceClip = (uint8_t)getVarOrDirectByte(PARAM_1); break;
        case 20: case 21:
            a->ignoreBoxes = !(g.opcode & 1);
            a->forceClip = 0;
            if (actor_inCurrentRoom(a))
                actor_put(a, a->x, a->y, a->room);
            break;
        case 22: a->animSpeed = (uint8_t)getVarOrDirectByte(PARAM_1);   /* setAnimSpeed */
                 a->animProgress = 0; break;
        case 23: a->shadowMode = (uint8_t)getVarOrDirectByte(PARAM_1); break;
        default: halt("o5_actorOps: default case");
        }
    }
}

/* --- cursor, verbs, strings, text --------------------------------------------- */

static void o5_cursorCommand(void)
{
    int32_t table[NUM_SCRIPT_LOCAL];
    switch ((g.opcode = fetchScriptByte()) & 0x1F) {
    case 1: g.cursor.state = 1; verbMouseOver(0); break;
    case 2: g.cursor.state = 0; verbMouseOver(0); break;
    case 3: g.userPut = 1; break;
    case 4: g.userPut = 0; break;
    case 5: g.cursor.state++; verbMouseOver(0); break;
    case 6: g.cursor.state--; verbMouseOver(0); break;
    case 7: g.userPut++; break;
    case 8: g.userPut--; break;
    case 10: (void)getVarOrDirectByte(PARAM_1); (void)getVarOrDirectByte(PARAM_2); break;
    case 11: (void)getVarOrDirectByte(PARAM_1); (void)getVarOrDirectByte(PARAM_2);
             (void)getVarOrDirectByte(PARAM_3); break;
    case 12: {                                 /* SO_CURSOR_SET */
        int i = getVarOrDirectByte(PARAM_1);
        if (i < 0 || i > 3)
            plat_error("SO_CURSOR_SET: unsupported cursor id");
        g.currentCursor = i;
        break;
    }
    case 13: initCharset(getVarOrDirectByte(PARAM_1)); break;
    case 14: {                                 /* the charset colour map (v4: a word table) */
        int i, cs = g.stringDefault[1].charset;
        memset(table, 0, sizeof(table));
        getWordVararg(table);
        for (i = 0; i < 16; i++)
            g.charsetColorMap[i] = g.charsetData[cs][i] = (uint8_t)table[i];
        break;
    }
    default: break;
    }
    VAR(VAR_CURSORSTATE) = g.cursor.state;
    VAR(VAR_USERPUT) = g.userPut;
}

static void o5_stringOps(void)
{
    int a, b, c;
    g.opcode = fetchScriptByte();
    switch (g.opcode & 0x1F) {
    case 1:                    /* loadstring */
        loadString(getVarOrDirectByte(PARAM_1), 0);
        break;
    case 2:                    /* copystring */
        a = getVarOrDirectByte(PARAM_1);
        b = getVarOrDirectByte(PARAM_2);
        if (a >= 0 && a < NUM_STRINGS && b >= 0 && b < NUM_STRINGS)
            memcpy(g.strings[a], g.strings[b], STRING_MAX);
        break;
    case 3:                    /* set string char */
        a = getVarOrDirectByte(PARAM_1);
        b = getVarOrDirectByte(PARAM_2);
        c = getVarOrDirectByte(PARAM_3);
        if (a >= 0 && a < NUM_STRINGS && b >= 0 && b < STRING_MAX)
            g.strings[a][b] = (uint8_t)c;
        break;
    case 4:                    /* get string char */
        getResultPos();
        a = getVarOrDirectByte(PARAM_1);
        b = getVarOrDirectByte(PARAM_2);
        setResult((a >= 0 && a < NUM_STRINGS && b >= 0 && b < STRING_MAX) ? g.strings[a][b] : 0);
        break;
    case 5:                    /* create empty string */
        a = getVarOrDirectByte(PARAM_1);
        b = getVarOrDirectByte(PARAM_2);
        if (a >= 0 && a < NUM_STRINGS)
            memset(g.strings[a], 0, STRING_MAX);
        (void)b;
        break;
    default:
        break;
    }
}

static int getVerbSlot(int id, int mode)
{
    int i;
    for (i = 1; i < NUM_VERBS; i++)
        if (g.verbs[i].verbid == id && g.verbs[i].saveid == mode)
            return i;
    return 0;
}

/* killVerb: the slot is freed, its colours and position stay (the name resource goes) */
static void killVerb(int slot)
{
    VerbSlot *vs;
    if (slot == 0)
        return;
    vs = &g.verbs[slot];
    vs->verbid = 0;
    vs->curmode = 0;
    vs->name[0] = 0;
    vs->hasName = 0;
    if (vs->saveid == 0) {                       /* v <= 6 */
        drawVerb(slot, 0);
        verbMouseOver(0);
    }
    vs->saveid = 0;
}

static void o5_saveRestoreVerbs(void)
{
    int a, b, c, slot, slot2;
    g.opcode = fetchScriptByte();
    a = getVarOrDirectByte(PARAM_1);
    b = getVarOrDirectByte(PARAM_2);
    c = getVarOrDirectByte(PARAM_3);
    switch (g.opcode) {
    case 1:                                      /* SO_SAVE_VERBS */
        for (; a <= b; a++) {
            slot = getVerbSlot(a, 0);
            if (slot && g.verbs[slot].saveid == 0) {
                g.verbs[slot].saveid = (uint8_t)c;
                drawVerb(slot, 0);
                verbMouseOver(0);
            }
        }
        break;
    case 2:                                      /* SO_RESTORE_VERBS */
        for (; a <= b; a++) {
            slot = getVerbSlot(a, c);
            if (slot) {
                slot2 = getVerbSlot(a, 0);
                if (slot2)
                    killVerb(slot2);
                slot = getVerbSlot(a, c);
                g.verbs[slot].saveid = 0;
                drawVerb(slot, 0);
                verbMouseOver(0);
            }
        }
        break;
    case 3:                                      /* SO_DELETE_VERBS */
        for (; a <= b; a++) {
            slot = getVerbSlot(a, c);
            if (slot)
                killVerb(slot);
        }
        break;
    default:
        halt("o5_saveRestoreVerbs: unknown subopcode");
    }
}

static void o5_verbOps(void)
{
    int verb, slot;
    VerbSlot *vs;
    verb = getVarOrDirectByte(PARAM_1);
    slot = getVerbSlot(verb, 0);
    vs = &g.verbs[slot];
    vs->verbid = (uint16_t)verb;
    while ((g.opcode = fetchScriptByte()) != 0xFF) {
        switch (g.opcode & 0x1F) {
        case 1:  (void)getVarOrDirectWord(PARAM_1); halt("o5_verbOps: SO_VERB_IMAGE"); break;
        case 2: {                                  /* SO_VERB_NAME */
            int len = resStrLen(g.scriptPointer) + 1;
            if (len > (int)sizeof(vs->name)) {
                plat_error("SO_VERB_NAME: the name does not fit");
                return;
            }
            memcpy(vs->name, g.scriptPointer, (size_t)len);
            g.scriptPointer += len;
            vs->hasName = 1;
            if (slot == 0) {
                vs->name[0] = 0;
                vs->hasName = 0;
            }
            vs->type = 0;
            vs->imgindex = 0;
            break;
        }
        case 3:  vs->color = (uint8_t)getVarOrDirectByte(PARAM_1); break;
        case 4:  vs->hicolor = (uint8_t)getVarOrDirectByte(PARAM_1); break;
        case 5:  vs->x = (int16_t)getVarOrDirectWord(PARAM_1);
                 vs->y = (int16_t)getVarOrDirectWord(PARAM_2); break;
        case 6:  vs->curmode = 1; break;
        case 7:  vs->curmode = 0; break;
        case 8:  killVerb(slot); break;
        case 9:                                    /* SO_VERB_NEW */
            slot = getVerbSlot(verb, 0);
            if (slot == 0) {
                for (slot = 1; slot < NUM_VERBS; slot++)
                    if (g.verbs[slot].verbid == 0)
                        break;
                if (slot == NUM_VERBS) { halt("Too many verbs"); return; }
            }
            vs = &g.verbs[slot];
            vs->verbid = (uint16_t)verb;
            vs->color = 2;
            vs->hicolor = 0;
            vs->dimcolor = 8;
            vs->type = 0;
            vs->charset = (uint8_t)g.stringDefault[0].charset;
            vs->curmode = 0;
            vs->saveid = 0;
            vs->key = 0;
            vs->center = 0;
            vs->imgindex = 0;
            break;
        case 16: vs->dimcolor = (uint8_t)getVarOrDirectByte(PARAM_1); break;
        case 17: vs->curmode = 2; break;
        case 18: vs->key = (uint8_t)getVarOrDirectByte(PARAM_1); break;
        case 19: vs->center = 1; break;
        case 20: {                                 /* SO_VERB_NAME_STR */
            int s = getVarOrDirectWord(PARAM_1);
            if (s > 0 && s < NUM_STRINGS && g.strings[s][0]) {
                memcpy(vs->name, g.strings[s], sizeof(vs->name) - 1);
                vs->hasName = 1;
            } else {
                vs->name[0] = 0;
                vs->hasName = 0;
            }
            if (slot == 0)
                vs->hasName = 0;
            vs->type = 0;
            vs->imgindex = 0;
            break;
        }
        case 22: (void)getVarOrDirectWord(PARAM_1); (void)getVarOrDirectByte(PARAM_2);
                 halt("o5_verbOps: assign object"); break;
        case 23: vs->bkcolor = (uint8_t)getVarOrDirectByte(PARAM_1); break;
        default: halt("o5_verbOps: unknown subop"); return;
        }
    }
    drawVerb(slot, 0);
    verbMouseOver(0);
}

/* decodeParseString: the operands are parsed exactly; the text itself is drawn and timed
 * by the text slice (actorTalk / CHARSET_1).  Until then the message is consumed at once. */

static void decodeParseString(void)
{
    int textSlot;
    TextSlot *st;
    switch (g.actorToPrintStrFor) {
    case 252: textSlot = 3; break;
    case 253: textSlot = 2; break;
    case 254: textSlot = 1; break;
    default:  textSlot = 0;
    }
    st = &g.string[textSlot];
    *st = g.stringDefault[textSlot];             /* loadDefault */
    while ((g.opcode = fetchScriptByte()) != 0xFF) {
        switch (g.opcode & 0xF) {
        case 0:                                    /* SO_AT */
            st->xpos = (int16_t)getVarOrDirectWord(PARAM_1);
            st->ypos = (int16_t)getVarOrDirectWord(PARAM_2);
            st->overhead = 0;
            break;
        case 1: st->color = (uint8_t)getVarOrDirectByte(PARAM_1); break;
        case 2: st->right = (int16_t)getVarOrDirectWord(PARAM_1); break;
        case 3: halt("decodeParseString: SO_ERASE"); return;
        case 4: st->center = 1; st->overhead = 0; break;
        case 6: st->center = 0; st->overhead = 0; break;     /* SO_LEFT, v4 */
        case 7: st->overhead = 1; break;
        case 8: halt("decodeParseString: SO_SAY_VOICE"); return;
        case 15: {                                 /* SO_TEXTSTRING */
            int len = resStrLen(g.scriptPointer);
            printString(textSlot, g.scriptPointer);
            g.scriptPointer += len + 1;
            return;
        }
        default: halt("decodeParseString: unhandled case"); return;
        }
    }
    g.stringDefault[textSlot] = *st;             /* saveDefault */
}

/* --- sound: the queue as ScummVM's Sound (startSound queues, processSound at the end of
 * the loop drains it).  Until the audio slice nothing plays, so a drained sound is over. */

static void o5_startSound(void)
{
    int sound = getVarOrDirectByte(PARAM_1);
    VAR(VAR_MUSIC_TIMER) = 0;
    sound_queue(sound);
}

static void o5_startMusic(void) { sound_queue(getVarOrDirectByte(PARAM_1)); }
static void o5_stopSound(void)  { sound_stop(getVarOrDirectByte(PARAM_1)); }
static void o5_stopMusic(void)  { sound_stop_all(); }

static void o5_isSoundRunning(void)
{
    int snd;
    getResultPos();
    snd = getVarOrDirectByte(PARAM_1);
    if (snd)
        snd = sound_running(snd);
    setResult(snd);
}

static void o5_print(void)
{
    g.actorToPrintStrFor = getVarOrDirectByte(PARAM_1);
    decodeParseString();
}

static void o5_printEgo(void)
{
    g.actorToPrintStrFor = (uint8_t)VAR(VAR_EGO);
    decodeParseString();
}


/* --- walking, the camera, the inventory, the boxes (ScummVM script_v5.cpp, script_v4.cpp) --- */

static void o5_getActorX(void)
{
    getResultPos();
    setResult(getObjX(getVarOrDirectWord(PARAM_1)));
}

static void o5_getActorY(void)
{
    getResultPos();
    setResult(getObjY(getVarOrDirectWord(PARAM_1)));
}

static void o5_getActorMoving(void)
{
    getResultPos();
    setResult(derefActor(getVarOrDirectByte(PARAM_1))->moving);
}

static void o5_getActorFacing(void)
{
    getResultPos();
    setResult(newDirToOldDir(derefActor(getVarOrDirectByte(PARAM_1))->facing));
}

static void o5_getActorElevation(void)
{
    getResultPos();
    setResult(derefActor(getVarOrDirectByte(PARAM_1))->elevation);
}

static void o5_getActorCostume(void)
{
    getResultPos();
    setResult(derefActor(getVarOrDirectByte(PARAM_1))->costume);
}

static void o5_getActorWalkBox(void)
{
    getResultPos();
    setResult(derefActor(getVarOrDirectByte(PARAM_1))->walkbox);
}

static void o5_getActorWidth(void)
{
    getResultPos();
    setResult(derefActor(getVarOrDirectByte(PARAM_1))->width);
}

static void o5_isActorInBox(void)
{
    int act = getVarOrDirectByte(PARAM_1);
    int box = getVarOrDirectByte(PARAM_2);
    Actor *a = derefActor(act);
    jumpRelative(checkXYInBoxBounds(box, a->x, a->y));
}

/* getObjActToObjActDist: the larger of |dx| and |dy|; an actor measures to an object at the
 * object's point moved into its boxes */
static int getObjActToObjActDist(int o1, int o2)
{
    int x, y, x2, y2, dx, dy;
    Actor *acta = 0, *actb = 0;
    if (isValidActor(o1))
        acta = &g.actors[o1];
    if (isValidActor(o2))
        actb = &g.actors[o2];
    if (acta && actb && acta->room == actb->room && acta->room && !actor_inCurrentRoom(acta))
        return 0;
    if (getObjectOrActorXY(o1, &x, &y) == -1)
        return 0xFF;
    if (getObjectOrActorXY(o2, &x2, &y2) == -1)
        return 0xFF;
    if (acta && !actb)
        actor_adjustXYToBeInBox(acta, x2, y2, &x2, &y2);
    dx = x - x2; if (dx < 0) dx = -dx;
    dy = y - y2; if (dy < 0) dy = -dy;
    return dx > dy ? dx : dy;                    /* getDist */
}

static void o5_getDist(void)
{
    int o1, o2;
    getResultPos();
    o1 = getVarOrDirectWord(PARAM_1);
    o2 = getVarOrDirectWord(PARAM_2);
    setResult(getObjActToObjActDist(o1, o2));
}

/* o5_getClosestObjActor: among VAR_ACTOR_RANGE_MIN..MAX, nothing beyond 255 (as the original) */
static void o5_getClosestObjActor(void)
{
    int obj, act, dist, closest_obj = 0xFF, closest_dist = 0xFF;
    getResultPos();
    act = getVarOrDirectWord(PARAM_1);
    obj = VAR(VAR_ACTOR_RANGE_MAX);
    do {
        dist = getObjActToObjActDist(act, obj);
        if (dist < closest_dist) {
            closest_dist = dist;
            closest_obj = obj;
        }
    } while (--obj >= VAR(VAR_ACTOR_RANGE_MIN));
    setResult(closest_obj);
}

/* o5_getStringWidth: a string slot measured in the current charset */
static void o5_getStringWidth(void)
{
    int string;
    getResultPos();
    string = getVarOrDirectByte(PARAM_1);
    if (string < 0 || string >= NUM_STRINGS) {
        halt("o5_getStringWidth: no such string");
        return;
    }
    setResult(text_getStringWidth(0, g.strings[string]));
}

static void o5_getVerbEntrypoint(void)
{
    int a, b;
    getResultPos();
    a = getVarOrDirectWord(PARAM_1);
    b = getVarOrDirectWord(PARAM_2);
    setResult(getVerbEntrypoint(a, b));
}

/* o5_drawBox: a filled rectangle; no usage bits (markRectAsDirty without an actor), so the
 * logic only reads its parameters.  The fill is the platform's. */
static void o5_drawBox(void)
{
    int x, y, x2, y2, color;
    x = getVarOrDirectWord(PARAM_1);
    y = getVarOrDirectWord(PARAM_2);
    g.opcode = fetchScriptByte();
    x2 = getVarOrDirectWord(PARAM_1);
    y2 = getVarOrDirectWord(PARAM_2);
    color = getVarOrDirectByte(PARAM_3);
    plat_drawBox(x, y, x2, y2, color);
}

static void o5_walkActorTo(void)
{
    Actor *a = derefActor(getVarOrDirectByte(PARAM_1));
    int x = getVarOrDirectWord(PARAM_2);
    int y = getVarOrDirectWord(PARAM_3);
    actor_startWalk(a, x, y, -1);
}

static void o5_walkActorToObject(void)
{
    Actor *a = derefActor(getVarOrDirectByte(PARAM_1));
    int obj = getVarOrDirectWord(PARAM_2);
    if (whereIsObject(obj) != WIO_NOT_FOUND) {
        int x, y, dir;
        getObjectXYPos(obj, &x, &y, &dir);
        actor_startWalk(a, x, y, dir);
    }
}

/* ScummEngine_v5::walkActorToActor */
static void o5_walkActorToActor(void)
{
    int nr = getVarOrDirectByte(PARAM_1);
    int nr2 = getVarOrDirectByte(PARAM_2);
    int dist = fetchScriptByte(), x, y;
    Actor *a, *to;
    if (!isValidActor(nr))
        return;
    a = derefActor(nr);
    if (!actor_inCurrentRoom(a))
        return;
    if (!isValidActor(nr2))
        return;
    to = derefActor(nr2);
    if (!actor_inCurrentRoom(to))
        return;
    if (dist == 0xFF) {
        dist = a->scalex * a->width / 0xFF;
        dist += (to->scalex * to->width / 0xFF) / 2;
    }
    x = to->x;
    y = to->y;
    if (x < a->x)
        x += dist;
    else
        x -= dist;
    actor_startWalk(a, x, y, -1);
}

static void o5_faceActor(void)
{
    int act = getVarOrDirectByte(PARAM_1);
    int obj = getVarOrDirectWord(PARAM_2);
    if (isValidActor(act))                       /* derefActorSafe */
        actor_faceToObject(&g.actors[act], obj);
}

static void o5_putActorAtObject(void)
{
    Actor *a = derefActor(getVarOrDirectByte(PARAM_1));
    int obj = getVarOrDirectWord(PARAM_2), x, y;
    if (whereIsObject(obj) != WIO_NOT_FOUND) {
        getObjectXYPos(obj, &x, &y, 0);
    } else {
        x = 240;
        y = 120;
    }
    actor_put(a, x, y, a->room);
}

static void o5_loadRoomWithEgo(void)
{
    Actor *a;
    int obj, room, x, y, x2, y2, dir, oldDir;

    obj = getVarOrDirectWord(PARAM_1);
    room = getVarOrDirectByte(PARAM_2);
    a = derefActor(VAR(VAR_EGO));
    actor_put(a, a->x, a->y, room);
    oldDir = a->facing;
    g.egoPositioned = 0;

    x = fetchScriptWordSigned();
    y = fetchScriptWordSigned();

    VAR(VAR_WALKTO_OBJ) = obj;
    startScene(a->room);
    VAR(VAR_WALKTO_OBJ) = 0;

    if (whereIsObject(obj) != WIO_ROOM)
        plat_error("o5_loadRoomWithEgo: object not in the room");
    if (!g.egoPositioned) {
        getObjectXYPos(obj, &x2, &y2, &dir);
        actor_put(a, x2, y2, g.currentRoom);
        if (a->facing == oldDir)
            actor_setDirection(a, dir + 180);
    }
    a->moving = 0;

    g.camera.cur_x = g.camera.dest_x = a->x;
    setCameraFollows(a, 0);
    g.fullRedraw = 1;

    if (x != -1)
        actor_startWalk(a, x, y, -1);
}

static void o5_actorFollowCamera(void)
{
    actorFollowCamera(getVarOrDirectByte(PARAM_1));
}

static void o5_panCameraTo(void)
{
    g.camera.dest_x = getVarOrDirectWord(PARAM_1);
    g.camera.mode = 3;                           /* kPanningCameraMode */
    g.camera.movingToActor = 0;
}

static void o5_wait(void)
{
    const uint8_t *oldaddr = g.scriptPointer - 1;
    g.opcode = fetchScriptByte();
    switch (g.opcode & 0x1F) {
    case 1: {                                    /* SO_WAIT_FOR_ACTOR */
        int act = getVarOrDirectByte(PARAM_1);
        if (act >= 0 && act < NUM_ACTORS && g.actors[act].moving)   /* derefActorSafe */
            break;
        return;
    }
    case 2:                                      /* SO_WAIT_FOR_MESSAGE */
        if (VAR(VAR_HAVE_MSG))
            break;
        return;
    case 3:                                      /* SO_WAIT_FOR_CAMERA */
        if (g.camera.cur_x / 8 != g.camera.dest_x / 8)
            break;
        return;
    case 4:                                      /* SO_WAIT_FOR_SENTENCE */
        if (g.sentenceNum) {
            if (g.sentence[g.sentenceNum - 1].freezeCount && !isScriptInUse(VAR(VAR_SENTENCE_SCRIPT)))
                return;
        } else if (!isScriptInUse(VAR(VAR_SENTENCE_SCRIPT))) {
            return;
        }
        break;
    default:
        halt("o5_wait: unknown subopcode");
        return;
    }
    g.scriptPointer = oldaddr;
    o5_breakHere();
}

static void o5_matrixOps(void)
{
    int a, b;
    g.opcode = fetchScriptByte();
    switch (g.opcode & 0x1F) {
    case 1:
        a = getVarOrDirectByte(PARAM_1);
        b = getVarOrDirectByte(PARAM_2);
        setBoxFlags(a, b);
        break;
    case 2:
        a = getVarOrDirectByte(PARAM_1);
        b = getVarOrDirectByte(PARAM_2);
        setBoxScale(a, b);
        break;
    case 3:
        a = getVarOrDirectByte(PARAM_1);
        b = getVarOrDirectByte(PARAM_2);
        setBoxScale(a, (b - 1) | 0x8000);
        break;
    case 4:
        createBoxMatrix();
        break;
    default:
        break;
    }
}

static void o5_actorFromPos(void)
{
    int x, y;
    getResultPos();
    x = getVarOrDirectWord(PARAM_1);
    y = getVarOrDirectWord(PARAM_2);
    setResult(getActorFromPos(x, y));
}

static void o5_findObject(void)
{
    int x, y;
    getResultPos();
    x = getVarOrDirectByte(PARAM_1);
    y = getVarOrDirectByte(PARAM_2);
    setResult(findObject(x, y));
}

static void o5_findInventory(void)
{
    int x, y;
    getResultPos();
    x = getVarOrDirectByte(PARAM_1);
    y = getVarOrDirectByte(PARAM_2);
    setResult(findInventory(x, y));
}

static void o5_getInventoryCount(void)
{
    getResultPos();
    setResult(getInventoryCount(getVarOrDirectByte(PARAM_1)));
}

/* o5_setObjectName: the new name follows inline.  (ScummVM's WORKAROUND for bug #10571 is an
 * enhancement, off in the oracle; the original stopped with a fatal error there.) */
static void o5_setObjectName(void)
{
    int obj = getVarOrDirectWord(PARAM_1);
    const uint8_t *name = g.scriptPointer;
    g.scriptPointer += resStrLen(name) + 1;
    setObjectName(obj, name);
}

static void o5_setOwnerOf(void)
{
    int obj = getVarOrDirectWord(PARAM_1);
    int owner = getVarOrDirectByte(PARAM_2);
    setOwnerOf(obj, owner);
}

static void o4_pickupObject(void)
{
    int obj = getVarOrDirectWord(PARAM_1);
    if (obj < 1) {
        halt("pickupObjectOld: invalid object");
        return;
    }
    if (getObjectIndex(obj) == -1)
        return;
    if (whereIsObject(obj) == WIO_INVENTORY)     /* don't take an object twice */
        return;
    addObjectToInventory(obj, g.roomResource);
    markObjectRectAsDirty(obj);
    putOwner(obj, VAR(VAR_EGO));
    putClass(obj, 32, 1);                        /* kObjectClassUntouchable */
    putState(obj, 1);
    clearDrawObjectQueue();
    runInventoryScript(1);
}

#include "opcodes_gen.h"
