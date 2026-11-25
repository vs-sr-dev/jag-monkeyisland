/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * script.c - the script VM: slots, nesting, variables, freeze, cutscenes, sentences.
 * Transcribed from ScummVM engines/scumm/script.cpp (v4 path, no HE, no workarounds).
 */
#include <stdio.h>
#include <string.h>
#include "scumm.h"

Scumm g;

static char tbuf[128];

static void initializeLocals(int slot, const int32_t *vars)
{
    int i;
    for (i = 0; i < NUM_SCRIPT_LOCAL; i++)
        g.localvar[slot][i] = vars ? vars[i] : 0;
}

static int getScriptSlot(void)
{
    int i;
    for (i = 1; i < NUM_SCRIPT_SLOT; i++)
        if (g.slot[i].status == SS_DEAD)
            return i;
    plat_error("Too many scripts running");
    return -1;
}

static void getScriptBaseAddress(void)
{
    ScriptSlot *ss;
    if (g.currentScript == 0xFF)
        return;
    ss = &g.slot[g.currentScript];
    switch (ss->where) {
    case WIO_LOCAL:
    case WIO_ROOM:
        g.scriptOrgPointer = g.roomPtr;
        break;
    case WIO_GLOBAL:
        g.scriptOrgPointer = res_script(ss->number);
        break;
    case WIO_INVENTORY: {                        /* the object's OC block (rtInventory) */
        int idx;
        for (idx = 0; idx < NUM_INVENTORY; idx++)
            if (g.inventory[idx] == ss->number)
                break;
        if (idx == NUM_INVENTORY)
            plat_error("getScriptBaseAddress: object not in the inventory");
        else
            g.scriptOrgPointer = g.inventoryPtr[idx];
        break;
    }
    default:
        plat_error("getScriptBaseAddress: flobject scripts not yet implemented");
    }
}

static void resetScriptPointer(void)
{
    if (g.currentScript == 0xFF)
        return;
    g.scriptPointer = g.scriptOrgPointer + g.slot[g.currentScript].offs;
}

void updateScriptPtr(void)
{
    if (g.currentScript == 0xFF)
        return;
    g.slot[g.currentScript].offs = (uint32_t)(g.scriptPointer - g.scriptOrgPointer);
}

static void executeScript(void)
{
    while (g.currentScript != 0xFF && !g.halted) {
        g.opcode = fetchScriptByte();
        g.slot[g.currentScript].didexec = 1;
        if (g.trace) {
            sprintf(tbuf, "Script %d, offset 0x%x: [%X] %s()", g.slot[g.currentScript].number,
                    (unsigned)(g.scriptPointer - g.scriptOrgPointer), g.opcode, opcodeNames[g.opcode]);
            plat_trace(tbuf);
        }
        executeOpcode(g.opcode);
    }
}

void executeOpcode(uint8_t op)
{
    opcodes_execute(op);
}

static void runScriptNested(int script)
{
    NestedScript *nest;
    ScriptSlot *slot;

    updateScriptPtr();
    if (g.numNestedScripts >= MAX_NESTING)
        plat_error("Too many nested scripts");
    nest = &g.nest[g.numNestedScripts];
    if (g.currentScript == 0xFF) {
        nest->number = 0;
        nest->where = 0xFF;
    } else {
        slot = &g.slot[g.currentScript];
        nest->number = slot->number;
        nest->where = slot->where;
        nest->slot = g.currentScript;
    }
    g.numNestedScripts++;

    g.currentScript = (uint8_t)script;
    getScriptBaseAddress();
    resetScriptPointer();
    executeScript();

    if (g.numNestedScripts != 0)
        g.numNestedScripts--;

    if (nest->number) {
        slot = &g.slot[nest->slot];
        if (slot->number == nest->number && slot->where == nest->where &&
            slot->status != SS_DEAD && slot->freezeCount == 0) {
            g.currentScript = nest->slot;
            getScriptBaseAddress();
            resetScriptPointer();
            return;
        }
    }
    g.currentScript = 0xFF;
}

void runScript(int script, int freezeResistant, int recursive, const int32_t *lvarptr)
{
    ScriptSlot *s;
    uint32_t scriptOffs;
    uint8_t scriptType;
    int slot;
    unsigned number;

    if (!script)
        return;
    if (!recursive)
        stopScript(script);

    number = (g.currentScript != 0xFF) ? g.slot[g.currentScript].number : 0;

    if (script < NUM_GLOBAL_SCRIPTS) {
        scriptOffs = 6;                         /* _resourceHeaderSize */
        scriptType = WIO_GLOBAL;
        if (g.trace) {
            sprintf(tbuf, "runScript(Global-%d) from %u-%d", script, number, g.roomResource);
            plat_trace(tbuf);
        }
    } else {
        scriptOffs = g.localScriptOffsets[script - NUM_GLOBAL_SCRIPTS];
        if (scriptOffs == 0)
            plat_error("Local script is not in this room");
        scriptType = WIO_LOCAL;
        if (g.trace) {
            sprintf(tbuf, "runScript(%d) from %u-%d", script, number, g.roomResource);
            plat_trace(tbuf);
        }
    }

    slot = getScriptSlot();
    s = &g.slot[slot];
    s->number = (uint16_t)script;
    s->offs = scriptOffs;
    s->status = SS_RUNNING;
    s->where = scriptType;
    s->freezeResistant = (uint8_t)freezeResistant;
    s->recursive = (uint8_t)recursive;
    s->freezeCount = 0;
    s->delayFrameCount = 0;
    s->cycle = 1;
    initializeLocals(slot, lvarptr);
    runScriptNested(slot);
}

/* getVerbEntrypoint, GF_SMALL_HEADER: the verb table sits 19 bytes into the OBCD block. */
int getVerbEntrypoint(int obj, int entry)
{
    const uint8_t *objptr, *verbptr;
    if (whereIsObject(obj) == WIO_NOT_FOUND)
        return 0;
    objptr = getOBCDFromObject(obj);
    if (!objptr)
        return 0;
    verbptr = objptr + 19;
    for (;;) {
        if (!*verbptr)
            return 0;
        if (*verbptr == entry || *verbptr == 0xFF)
            break;
        verbptr += 3;
    }
    return rd16(verbptr + 1);
}

void runObjectScript(int object, int entry, int freezeResistant, int recursive, const int32_t *vars)
{
    ScriptSlot *s;
    uint32_t obcd;
    int where, offs, slot;

    if (!object)
        return;
    if (!recursive)
        stopObjectScript(object);
    where = whereIsObject(object);
    if (where == WIO_NOT_FOUND)
        return;
    obcd = getOBCDOffs(object);
    slot = getScriptSlot();
    offs = getVerbEntrypoint(object, entry);
    if (offs == 0)
        return;
    s = &g.slot[slot];
    s->number = (uint16_t)object;
    s->offs = obcd + offs;
    s->status = SS_RUNNING;
    s->where = (uint8_t)where;
    s->freezeResistant = (uint8_t)freezeResistant;
    s->recursive = (uint8_t)recursive;
    s->freezeCount = 0;
    s->delayFrameCount = 0;
    s->cycle = 1;
    initializeLocals(slot, vars);
    runScriptNested(slot);
}

static void stopIn(int script, int global)
{
    ScriptSlot *ss = g.slot;
    int i;
    if (script == 0)
        return;
    for (i = 0; i < NUM_SCRIPT_SLOT; i++, ss++) {
        int isGlobal = (ss->where == WIO_GLOBAL || ss->where == WIO_LOCAL);
        int isObject = (ss->where == WIO_ROOM || ss->where == WIO_INVENTORY || ss->where == WIO_FLOBJECT);
        if (script == ss->number && ss->status != SS_DEAD && (global ? isGlobal : isObject)) {
            ss->number = 0;
            ss->status = SS_DEAD;
            if (g.currentScript == i)
                g.currentScript = 0xFF;
        }
    }
    for (i = 0; i < g.numNestedScripts; ++i) {
        NestedScript *n = &g.nest[i];
        int isGlobal = (n->where == WIO_GLOBAL || n->where == WIO_LOCAL);
        int isObject = (n->where == WIO_ROOM || n->where == WIO_INVENTORY || n->where == WIO_FLOBJECT);
        if (n->number == script && (global ? isGlobal : isObject)) {
            n->number = 0;
            n->slot = 0xFF;
            n->where = 0xFF;
        }
    }
}

void stopScript(int script)       { stopIn(script, 1); }
void stopObjectScript(int script) { stopIn(script, 0); }

/* stopObjectCode, the v3-v5 branch */
void stopObjectCode(void)
{
    ScriptSlot *ss = &g.slot[g.currentScript];
    if (ss->where != WIO_GLOBAL && ss->where != WIO_LOCAL) {
        stopObjectScript(ss->number);
    } else {
        if (ss->cutsceneOverride)
            plat_error("Script ending with active cutscene/override");
        ss->number = 0;
        ss->status = SS_DEAD;
    }
    g.currentScript = 0xFF;
}

void freezeScripts(int flag)
{
    int i;
    for (i = 0; i < NUM_SCRIPT_SLOT; i++) {
        if (g.currentScript != i && g.slot[i].status != SS_DEAD &&
            (!g.slot[i].freezeResistant || flag >= 0x80)) {
            g.slot[i].status |= 0x80;
            g.slot[i].freezeCount++;
        }
    }
    for (i = 0; i < NUM_SENTENCE; i++)
        g.sentence[i].freezeCount++;
    if (g.cutSceneScriptIndex != 0xFF) {
        g.slot[g.cutSceneScriptIndex].status &= 0x7F;
        g.slot[g.cutSceneScriptIndex].freezeCount = 0;
    }
}

void unfreezeScripts(void)
{
    int i;
    for (i = 0; i < NUM_SCRIPT_SLOT; i++) {
        if (g.slot[i].status & 0x80) {
            if (!--g.slot[i].freezeCount)
                g.slot[i].status &= 0x7F;
        }
    }
    for (i = 0; i < NUM_SENTENCE; i++)
        if (g.sentence[i].freezeCount > 0)
            g.sentence[i].freezeCount--;
}

void runAllScripts(void)
{
    int i;
    for (i = 0; i < NUM_SCRIPT_SLOT; i++)
        g.slot[i].didexec = 0;
    g.currentScript = 0xFF;
    for (i = 0; i < NUM_SCRIPT_SLOT && !g.halted; i++) {
        if (g.slot[i].cycle == 1 && g.slot[i].status == SS_RUNNING && !g.slot[i].didexec) {
            g.currentScript = (uint8_t)i;
            getScriptBaseAddress();
            resetScriptPointer();
            executeScript();
        }
    }
}

static void runRoomCode(int number, uint32_t offs)
{
    int slot = getScriptSlot();
    ScriptSlot *s = &g.slot[slot];
    s->status = SS_RUNNING;
    s->number = (uint16_t)number;
    s->where = WIO_ROOM;
    s->offs = offs;
    s->freezeResistant = 0;
    s->recursive = 0;
    s->freezeCount = 0;
    s->delayFrameCount = 0;
    s->cycle = 1;
    initializeLocals(slot, 0);
    runScriptNested(slot);
}

void runExitScript(void)
{
    if (VAR(VAR_EXIT_SCRIPT))
        runScript(VAR(VAR_EXIT_SCRIPT), 0, 0, 0);
    if (g.EXCD_offs)
        runRoomCode(SCRIPT_NUM_EXCD, g.EXCD_offs);
    if (VAR(VAR_EXIT_SCRIPT2))
        runScript(VAR(VAR_EXIT_SCRIPT2), 0, 0, 0);
}

void runEntryScript(void)
{
    if (VAR(VAR_ENTRY_SCRIPT))
        runScript(VAR(VAR_ENTRY_SCRIPT), 0, 0, 0);
    if (g.ENCD_offs)
        runRoomCode(SCRIPT_NUM_ENCD, g.ENCD_offs);
    if (VAR(VAR_ENTRY_SCRIPT2))
        runScript(VAR(VAR_ENTRY_SCRIPT2), 0, 0, 0);
}

void killScriptsAndResources(void)
{
    ScriptSlot *ss = g.slot;
    int i;
    for (i = 0; i < NUM_SCRIPT_SLOT; i++, ss++) {
        if (ss->where == WIO_ROOM || ss->where == WIO_FLOBJECT || ss->where == WIO_LOCAL) {
            ss->cutsceneOverride = 0;
            ss->status = SS_DEAD;
        }
    }
    nukeLocalObjectNames();
}

void decreaseScriptDelay(int amount)
{
    ScriptSlot *ss = g.slot;
    int i;
    for (i = 0; i < NUM_SCRIPT_SLOT; i++, ss++) {
        if (ss->status == SS_PAUSED) {
            ss->delay -= amount;
            if (ss->delay < 0) {
                ss->status = SS_RUNNING;
                ss->delay = 0;
            }
        }
    }
}

int isScriptInUse(int script)
{
    int i;
    for (i = 0; i < NUM_SCRIPT_SLOT; i++)
        if (g.slot[i].number == script)
            return 1;
    return 0;
}

int isScriptRunning(int script)
{
    int i;
    for (i = 0; i < NUM_SCRIPT_SLOT; i++) {
        ScriptSlot *ss = &g.slot[i];
        if (ss->number == script && (ss->where == WIO_GLOBAL || ss->where == WIO_LOCAL) && ss->status != SS_DEAD)
            return 1;
    }
    return 0;
}

int isRoomScriptRunning(int script)
{
    int i;
    for (i = 0; i < NUM_SCRIPT_SLOT; i++) {
        ScriptSlot *ss = &g.slot[i];
        if (ss->number == script && ss->where == WIO_ROOM && ss->status != SS_DEAD)
            return 1;
    }
    return 0;
}

void doSentence(int verb, int objectA, int objectB)
{
    SentenceTab *st;
    if (g.sentenceNum >= NUM_SENTENCE)
        plat_error("doSentence: too many sentences");
    st = &g.sentence[g.sentenceNum++];
    st->verb = (uint8_t)verb;
    st->objectA = (uint16_t)objectA;
    st->objectB = (uint16_t)objectB;
    st->preposition = (objectB != 0);
    st->freezeCount = 0;
}

void checkAndRunSentenceScript(void)
{
    int32_t localParamList[NUM_SCRIPT_LOCAL];
    int i, sentenceScript = VAR(VAR_SENTENCE_SCRIPT);
    SentenceTab *st;

    memset(localParamList, 0, sizeof(localParamList));
    if (isScriptInUse(sentenceScript)) {
        for (i = 0; i < NUM_SCRIPT_SLOT; i++) {
            ScriptSlot *ss = &g.slot[i];
            if (ss->number == sentenceScript && ss->status != SS_DEAD && ss->freezeCount == 0)
                return;
        }
    }
    if (!g.sentenceNum || g.sentence[g.sentenceNum - 1].freezeCount)
        return;
    g.sentenceNum--;
    st = &g.sentence[g.sentenceNum];
    if (st->preposition && st->objectB == st->objectA)
        return;
    localParamList[0] = st->verb;
    localParamList[1] = st->objectA;
    localParamList[2] = st->objectB;
    g.currentScript = 0xFF;
    if (sentenceScript)
        runScript(sentenceScript, 0, 0, localParamList);
}

void beginCutscene(const int32_t *args)
{
    int scr = g.currentScript;
    g.slot[scr].cutsceneOverride++;
    ++g.cutSceneStackPointer;
    if (g.cutSceneStackPointer >= MAX_CUTSCENE)
        plat_error("Cutscene stack overflow");
    g.cutSceneData[g.cutSceneStackPointer] = (int16_t)args[0];
    g.cutSceneScript[g.cutSceneStackPointer] = 0;
    g.cutScenePtr[g.cutSceneStackPointer] = 0;
    g.cutSceneScriptIndex = (uint8_t)scr;
    if (VAR(VAR_CUTSCENE_START_SCRIPT))
        runScript(VAR(VAR_CUTSCENE_START_SCRIPT), 0, 0, args);
    g.cutSceneScriptIndex = 0xFF;
}

/* abortCutscene (script.cpp): the cutscene's override point, if it set one, runs now */
void abortCutscene(void)
{
    const int idx = g.cutSceneStackPointer;
    uint32_t offs = g.cutScenePtr[idx];
    if (offs) {
        ScriptSlot *ss = &g.slot[g.cutSceneScript[idx]];
        ss->offs = offs;
        ss->status = SS_RUNNING;
        ss->freezeCount = 0;
        if (ss->cutsceneOverride > 0)
            ss->cutsceneOverride--;
        VAR(VAR_OVERRIDE) = 1;
        g.cutScenePtr[idx] = 0;
    }
}

void endCutscene(void)
{
    ScriptSlot *ss = &g.slot[g.currentScript];
    int32_t args[NUM_SCRIPT_LOCAL];

    if (ss->cutsceneOverride > 0)
        ss->cutsceneOverride--;
    memset(args, 0, sizeof(args));
    args[0] = g.cutSceneData[g.cutSceneStackPointer];
    VAR(VAR_OVERRIDE) = 0;
    if (g.cutScenePtr[g.cutSceneStackPointer] && ss->cutsceneOverride > 0)
        ss->cutsceneOverride--;
    g.cutSceneScript[g.cutSceneStackPointer] = 0;
    g.cutScenePtr[g.cutSceneStackPointer] = 0;
    if (g.cutSceneStackPointer == 0)
        plat_error("Cutscene stack underflow");
    g.cutSceneStackPointer--;
    if (VAR(VAR_CUTSCENE_END_SCRIPT))
        runScript(VAR(VAR_CUTSCENE_END_SCRIPT), 0, 0, args);
}

void beginOverride(void)
{
    int idx = g.cutSceneStackPointer;
    g.cutScenePtr[idx] = (uint32_t)(g.scriptPointer - g.scriptOrgPointer);
    g.cutSceneScript[idx] = g.currentScript;
    fetchScriptByte();
    fetchScriptWord();
}

void endOverride(void)
{
    int idx = g.cutSceneStackPointer;
    g.cutScenePtr[idx] = 0;
    g.cutSceneScript[idx] = 0;
    VAR(VAR_OVERRIDE) = 0;
}

uint8_t fetchScriptByte(void)
{
    return *g.scriptPointer++;
}

unsigned fetchScriptWord(void)
{
    unsigned a = rd16(g.scriptPointer);
    g.scriptPointer += 2;
    return a;
}

int fetchScriptWordSigned(void)
{
    return (int16_t)fetchScriptWord();
}

int readVar(unsigned var)
{
    if (var & 0x2000) {
        int a = (int)fetchScriptWord();
        if (a & 0x2000)
            var += readVar(a & ~0x2000);
        else
            var += a & 0xFFF;
        var &= ~0x2000u;
    }
    if (!(var & 0xF000)) {
        if (var >= NUM_VARIABLES) { plat_error("readVar: variable out of range"); return 0; }
        return g.vars[var];
    }
    if (var & 0x8000) {
        var &= 0x7FFF;
        if (var >= NUM_BITVARIABLES) { plat_error("readVar: bit variable out of range"); return 0; }
        return (g.bitVars[var >> 3] & (1 << (var & 7))) ? 1 : 0;
    }
    if (var & 0x4000) {
        var &= 0xFFF;
        if (var > 20) { plat_error("readVar: local variable out of range"); return 0; }
        return g.localvar[g.currentScript][var];
    }
    plat_error("Illegal varbits (r)");
    return -1;
}

void writeVar(unsigned var, int value)
{
    if (g.trace) {                               /* ScummVM's DEBUG_VARS line */
        sprintf(tbuf, "writeVar(%d, %d)", (int)var, value);
        plat_trace(tbuf);
    }
    if (!(var & 0xF000)) {
        if (var >= NUM_VARIABLES) { plat_error("writeVar: variable out of range"); return; }
        g.vars[var] = value;
        return;
    }
    if (var & 0x8000) {
        var &= 0x7FFF;
        if (var >= NUM_BITVARIABLES) { plat_error("writeVar: bit variable out of range"); return; }
        if (value)
            g.bitVars[var >> 3] |= (uint8_t)(1 << (var & 7));
        else
            g.bitVars[var >> 3] &= (uint8_t)~(1 << (var & 7));
        return;
    }
    if (var & 0x4000) {
        var &= 0xFFF;
        if (var > 20) { plat_error("writeVar: local variable out of range"); return; }
        g.localvar[g.currentScript][var] = value;
        return;
    }
    plat_error("Illegal varbits (w)");
}

void push(int a) { g.vmStack[g.stackPos++] = a; }
int pop(void) { return g.vmStack[--g.stackPos]; }

/* resStrLen: the length of a SCUMM string, whose escape codes (0xFF + code + 2 bytes,
 * except codes 1, 2, 3 and 8) may contain zero bytes. */
int resStrLen(const uint8_t *src)
{
    int num = 0;
    uint8_t chr;
    if (!src)
        src = g.scriptPointer;
    while ((chr = *src++) != 0) {
        num++;
        if (chr == 0xFF) {
            chr = *src++;
            num++;
            if (chr != 1 && chr != 2 && chr != 3 && chr != 8) {
                src += 2;
                num += 2;
            }
        }
    }
    return num;
}
