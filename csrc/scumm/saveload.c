/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * saveload.c - the game's state saved and loaded (ScummVM saveload.cpp, saveLoadWithSerializer
 * and loadState: what a v4 game needs of them).
 *
 * One function serializes both ways (a Ser loading or saving), as ScummVM's Serializer does;
 * the fields are little-endian of their C width (int is 32 bits on the PC and the Jaguar), so
 * one save serves both.  Pointers into the game image become (disk, offset); pointers the
 * room or the charset give are made again after a load.  Not ScummVM's file format: its
 * header, its resource lists and its versions are not needed here.
 *
 * Taken at scummLoop's handleSaveLoad (loop.c), between the music timer and runAllScripts:
 * no script runs then, so the VM's state is its slots, their offsets and their locals.
 * A load rebuilds what ScummVM's loadState rebuilds: the room's static data, the screens,
 * the buffers, a complete redraw, the palette, the music started again at its position.
 */
#include <string.h>
#include "scumm.h"

#define SAVE_MAGIC   0x4A4D4953UL            /* "JMIS" */
#define SAVE_VERSION 1

/* --- the serializer ---------------------------------------------------------------------- */
void ser_int(Ser *s, void *p, int size, int isSigned)
{
    uint32_t v = 0;
    int i;
    if (s->err)
        return;
    if (s->pos + (uint32_t)size > s->max) {
        s->err = 1;
        return;
    }
    if (s->loading) {
        for (i = 0; i < size; i++)
            v |= (uint32_t)s->buf[s->pos + i] << (8 * i);
        if (isSigned && size < 4 && (v & (1UL << (8 * size - 1))))
            v |= ~0UL << (8 * size);             /* sign-extended */
        if (size == 1)
            *(uint8_t *)p = (uint8_t)v;
        else if (size == 2)
            *(uint16_t *)p = (uint16_t)v;
        else
            *(uint32_t *)p = v;
    } else {
        if (size == 1)
            v = *(uint8_t *)p;
        else if (size == 2)
            v = *(uint16_t *)p;
        else
            v = *(uint32_t *)p;
        for (i = 0; i < size; i++)
            s->buf[s->pos + i] = (uint8_t)(v >> (8 * i));
    }
    s->pos += (uint32_t)size;
}

void ser_bytes(Ser *s, void *p, uint32_t n)
{
    if (s->err)
        return;
    if (s->pos + n > s->max) {
        s->err = 1;
        return;
    }
    if (s->loading)
        memcpy(p, s->buf + s->pos, n);
    else
        memcpy(s->buf + s->pos, p, n);
    s->pos += n;
}

/* a pointer into the game image's disks: (disk 1-4, offset), disk 0 = none */
void ser_imgptr(Ser *s, const uint8_t **p)
{
    uint8_t disk = 0;
    uint32_t offs = 0;
    int d;
    if (!s->loading && *p) {
        for (d = 1; d <= 4; d++)
            if (g.img->disk[d] && *p >= g.img->disk[d] && *p < g.img->disk[d] + g.img->disk_size[d]) {
                disk = (uint8_t)d;
                offs = (uint32_t)(*p - g.img->disk[d]);
                break;
            }
        if (!disk)
            plat_error("save: a pointer outside the game image");
    }
    SER(s, disk);
    SER(s, offs);
    if (s->loading)
        *p = disk ? g.img->disk[disk] + offs : 0;
}

/* --- the interpreter's struct (saveLoadWithSerializer's ScummEngine part) ------------------ */
static void ser_actor(Ser *s, Actor *a)
{
    SER(s, a->x); SER(s, a->y); SER(s, a->room); SER(s, a->costume); SER(s, a->elevation);
    SER(s, a->facing); SER(s, a->targetFacing); SER(s, a->number);
    SER(s, a->visible); SER(s, a->moving); SER(s, a->ignoreBoxes); SER(s, a->forceClip);
    SER(s, a->shadowMode); SER(s, a->ignoreTurns);
    SER(s, a->costumeNeedsInit); SER(s, a->needRedraw); SER(s, a->needBgReset);
    SER(s, a->initFrame); SER(s, a->walkFrame); SER(s, a->standFrame);
    SER(s, a->talkStartFrame); SER(s, a->talkStopFrame); SER(s, a->frame);
    SER(s, a->talkColor); SER(s, a->width); SER(s, a->scalex); SER(s, a->scaley);
    SER(s, a->animSpeed); SER(s, a->animProgress); SER(s, a->boxscale); SER(s, a->walkbox);
    SER(s, a->speedx); SER(s, a->speedy); SER(s, a->lastValidX); SER(s, a->lastValidY);
    SER(s, a->talkPosX); SER(s, a->talkPosY); SER(s, a->walkScript); SER(s, a->talkScript);
    SER(s, a->sound);
    SER(s, a->walkdata.dest.x); SER(s, a->walkdata.dest.y); SER(s, a->walkdata.destbox);
    SER(s, a->walkdata.destdir); SER(s, a->walkdata.cur.x); SER(s, a->walkdata.cur.y);
    SER(s, a->walkdata.curbox); SER(s, a->walkdata.next.x); SER(s, a->walkdata.next.y);
    SER(s, a->walkdata.point3.x); SER(s, a->walkdata.point3.y);
    SER(s, a->walkdata.deltaXFactor); SER(s, a->walkdata.deltaYFactor);
    SER(s, a->walkdata.xfrac); SER(s, a->walkdata.yfrac);
    ser_bytes(s, a->palette, sizeof(a->palette));
    ser_bytes(s, a->name, sizeof(a->name));
    SER(s, a->top); SER(s, a->bottom);
    SER(s, a->cost.animCounter); SER(s, a->cost.soundCounter); SER(s, a->cost.stopped);
    SER_ARR(s, a->cost.curpos); SER_ARR(s, a->cost.start); SER_ARR(s, a->cost.end);
    SER_ARR(s, a->cost.frame);
}

static void ser_scumm(Ser *s)
{
    int i;

    /* VM: the slots and their locals; the nest is empty between rounds, kept anyway */
    for (i = 0; i < NUM_SCRIPT_SLOT; i++) {
        ScriptSlot *ss = &g.slot[i];
        SER(s, ss->offs); SER(s, ss->delay); SER(s, ss->number); SER(s, ss->delayFrameCount);
        SER(s, ss->status); SER(s, ss->where); SER(s, ss->freezeResistant);
        SER(s, ss->recursive); SER(s, ss->freezeCount); SER(s, ss->didexec);
        SER(s, ss->cutsceneOverride); SER(s, ss->cycle);
        SER_ARR(s, g.localvar[i]);
    }
    for (i = 0; i < MAX_NESTING; i++) {
        SER(s, g.nest[i].number); SER(s, g.nest[i].where); SER(s, g.nest[i].slot);
    }
    SER(s, g.numNestedScripts);
    SER(s, g.currentScript);
    SER_ARR(s, g.cutScenePtr);
    SER_ARR(s, g.cutSceneScript);
    SER_ARR(s, g.cutSceneData);
    SER(s, g.cutSceneScriptIndex);
    SER(s, g.cutSceneStackPointer);
    for (i = 0; i < NUM_SENTENCE; i++) {
        SER(s, g.sentence[i].verb); SER(s, g.sentence[i].preposition);
        SER(s, g.sentence[i].freezeCount); SER(s, g.sentence[i].objectA);
        SER(s, g.sentence[i].objectB);
    }
    SER(s, g.sentenceNum);
    SER_ARR(s, g.vars);
    ser_bytes(s, g.bitVars, sizeof(g.bitVars));

    /* objects */
    SER_ARR(s, g.classData);
    ser_bytes(s, g.objectOwnerTable, sizeof(g.objectOwnerTable));
    ser_bytes(s, g.objectStateTable, sizeof(g.objectStateTable));
    for (i = 0; i < NUM_LOCAL_OBJECTS; i++) {
        ObjectData *od = &g.objs[i];
        SER(s, od->OBIMoffset); SER(s, od->OBCDoffset); SER(s, od->walk_x); SER(s, od->walk_y);
        SER(s, od->obj_nr); SER(s, od->x_pos); SER(s, od->y_pos); SER(s, od->width);
        SER(s, od->height); SER(s, od->actordir); SER(s, od->parent); SER(s, od->parentstate);
        SER(s, od->state); SER(s, od->fl_object_index); SER(s, od->flags);
    }
    SER(s, g.numObjectsInRoom);
    SER_ARR(s, g.inventory);
    for (i = 0; i < NUM_INVENTORY; i++)
        ser_imgptr(s, &g.inventoryPtr[i]);

    /* room: its number and what scripts change; its static data comes back from the image */
    SER(s, g.currentRoom); SER(s, g.roomResource);
    ser_bytes(s, g.resourceMapper, sizeof(g.resourceMapper));
    SER(s, g.camera.cur_x); SER(s, g.camera.cur_y); SER(s, g.camera.dest_x);
    SER(s, g.camera.dest_y); SER(s, g.camera.mode); SER(s, g.camera.follows);
    SER(s, g.camera.last_x); SER(s, g.camera.movingToActor); SER(s, g.camera.leftTrigger);
    SER(s, g.camera.rightTrigger);
    SER(s, g.screenStartStrip); SER(s, g.screenEndStrip); SER(s, g.mainXStart);
    ser_bytes(s, g.roomPalette, sizeof(g.roomPalette));

    /* actors, strings, verbs, cursor */
    for (i = 0; i < NUM_ACTORS; i++)
        ser_actor(s, &g.actors[i]);
    ser_bytes(s, g.strings, sizeof(g.strings));
    for (i = 0; i < 6; i++) {
        TextSlot *t = &g.string[i], *d = &g.stringDefault[i];
        SER(s, t->xpos); SER(s, t->ypos); SER(s, t->right); SER(s, t->height);
        SER(s, t->color); SER(s, t->charset); SER(s, t->center); SER(s, t->overhead);
        SER(s, t->no_talk_anim);
        SER(s, d->xpos); SER(s, d->ypos); SER(s, d->right); SER(s, d->height);
        SER(s, d->color); SER(s, d->charset); SER(s, d->center); SER(s, d->overhead);
        SER(s, d->no_talk_anim);
    }
    SER(s, g.actorToPrintStrFor);
    for (i = 0; i < NUM_VERBS; i++) {
        VerbSlot *v = &g.verbs[i];
        SER(s, v->x); SER(s, v->y); SER(s, v->right); SER(s, v->bottom);
        SER(s, v->oldLeft); SER(s, v->oldTop); SER(s, v->oldRight); SER(s, v->oldBottom);
        SER(s, v->color); SER(s, v->hicolor); SER(s, v->dimcolor); SER(s, v->bkcolor);
        SER(s, v->type); SER(s, v->charset); SER(s, v->curmode); SER(s, v->saveid);
        SER(s, v->key); SER(s, v->center); SER(s, v->prep); SER(s, v->hasName);
        SER(s, v->verbid); SER(s, v->imgindex);
        ser_bytes(s, v->name, sizeof(v->name));
    }
    SER(s, g.cursor.state); SER(s, g.cursor.animate); SER(s, g.cursor.animateIndex);
    SER(s, g.cursor.width); SER(s, g.cursor.height); SER(s, g.cursor.hotspotX);
    SER(s, g.cursor.hotspotY);
    for (i = 0; i < 4; i++)
        SER_ARR(s, g.cursorImages[i]);
    ser_bytes(s, g.cursorHotspots, sizeof(g.cursorHotspots));
    ser_bytes(s, g.grabbedCursor, sizeof(g.grabbedCursor));
    SER(s, g.userPut); SER(s, g.currentCursor); SER(s, g.haveMsg); SER(s, g.talkDelay);
    SER(s, g.egoPositioned); SER(s, g.verbMouseOver);
    ser_bytes(s, g.charsetColorMap, sizeof(g.charsetColorMap));
    ser_bytes(s, g.charsetData, sizeof(g.charsetData));
    SER(s, g.newEffect); SER(s, g.switchRoomEffect); SER(s, g.switchRoomEffect2);
    SER(s, g.doEffect); SER(s, g.screenEffectFlag); SER(s, g.fullRedraw);

    /* palette */
    ser_bytes(s, g.currentPalette, sizeof(g.currentPalette));
    ser_bytes(s, g.shadowPalette, sizeof(g.shadowPalette));
    SER(s, g.palDirtyMin); SER(s, g.palDirtyMax); SER(s, g.curPalIndex);
    for (i = 0; i < 16; i++) {
        ColorCycle *c = &g.colorCycle[i];
        SER(s, c->delay); SER(s, c->counter); SER(s, c->flags); SER(s, c->start); SER(s, c->end);
    }
    ser_bytes(s, g.palManipPalette, sizeof(g.palManipPalette));
    SER_ARR(s, g.palManipIntermediatePal);
    SER(s, g.palManipStart); SER(s, g.palManipEnd); SER(s, g.palManipCounter);

    /* the game's random source, and the banner's mark */
    SER(s, g.randSeed);
    SER(s, g.postGUICharMask);
}

static void ser_all(Ser *s)
{
    ser_scumm(s);
    loop_saveLoad(s);
    boxes_saveLoad(s);
    gfx_saveLoad(s);
    text_saveLoad(s);
    world_saveLoad(s);
    sound_saveLoad(s);
    ad_saveLoad(s);                          /* last: a load starts the music again */
}

static void ser_header(Ser *s, uint32_t *magic, uint32_t *version, uint32_t *size)
{
    SER(s, *magic);
    SER(s, *version);
    SER(s, *size);
}

#define PACK_MAGIC 0x5A494D4AUL              /* "JMIZ" */

/* the state packed: each run of zeros (most of it) as 0, n */
static uint32_t pack(uint8_t *out, uint32_t outMax, const uint8_t *in, uint32_t n)
{
    uint32_t i = 0, o = 8, k;
    if (outMax < 8)
        return 0;
    while (i < n) {
        if (in[i] == 0) {
            for (k = 0; i + k < n && in[i + k] == 0 && k < 255; k++)
                ;
            if (o + 2 > outMax)
                return 0;
            out[o++] = 0;
            out[o++] = (uint8_t)k;
            i += k;
        } else {
            if (o + 1 > outMax)
                return 0;
            out[o++] = in[i++];
        }
    }
    for (k = 0; k < 4; k++) {
        out[k] = (uint8_t)(PACK_MAGIC >> (8 * k));
        out[4 + k] = (uint8_t)(n >> (8 * k));
    }
    return o;
}

static uint32_t unpack(uint8_t *out, uint32_t outMax, const uint8_t *in, uint32_t len)
{
    uint32_t magic = 0, n = 0, i = 8, o = 0, k;
    if (len < 8)
        return 0;
    for (k = 0; k < 4; k++) {
        magic |= (uint32_t)in[k] << (8 * k);
        n |= (uint32_t)in[4 + k] << (8 * k);
    }
    if (magic != PACK_MAGIC || n > outMax)
        return 0;
    while (i < len && o < n) {
        if (in[i] == 0) {
            if (i + 1 >= len)
                return 0;
            k = in[i + 1];
            if (o + k > n)
                return 0;
            memset(out + o, 0, k);
            o += k;
            i += 2;
        } else {
            out[o++] = in[i++];
        }
    }
    return o == n && i == len ? n : 0;
}

uint32_t scumm_save(uint8_t *out, uint32_t outMax, uint8_t *raw, uint32_t rawMax)
{
    Ser s;
    uint32_t magic = SAVE_MAGIC, version = SAVE_VERSION, size = 0;
    memset(&s, 0, sizeof(s));
    s.buf = raw;
    s.max = rawMax;
    ser_header(&s, &magic, &version, &size);
    ser_all(&s);
    if (s.err)
        return 0;
    size = s.pos;                            /* written into the header */
    s.pos = 8;
    SER(&s, size);
    return pack(out, outMax, raw, size);
}

/* ScummEngine::loadState, a v4 DOS game's part: the sounds stopped, the state read, then what
 * it rebuilds */
int scumm_load(const uint8_t *in, uint32_t len, uint8_t *raw, uint32_t rawMax)
{
    Ser s;
    uint32_t magic = 0, version = 0, size = 0;
    const uint32_t n = unpack(raw, rawMax, in, len);
    if (!n)
        return 0;
    memset(&s, 0, sizeof(s));
    s.buf = raw;
    s.max = n;
    s.loading = 1;
    ser_header(&s, &magic, &version, &size);
    if (s.err || magic != SAVE_MAGIC || version != SAVE_VERSION || size != n)
        return 0;
   
    sound_stop_all();                        /* _sound->stopAllSounds() */
   
    ser_all(&s);
   
    if (s.err)
        plat_error("load: the save is cut");
    updateCursor();                          /* saveLoadWithSerializer: the cursor, and */
    {                                        /* the mouse warped to where it was */
        int x, y;
        scumm_mousePos(&x, &y);
        plat_warpMouse(x, y);
    }
    setDirtyColors(0, 255);                  /* resetPalette, DOS VGA */
   
    world_afterLoad();                       /* setupRoomSubBlocks ... initBGBuffers */
   
    return 1;
}
