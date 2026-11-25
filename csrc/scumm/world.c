/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * world.c - rooms, room objects, global object tables, actors (the script-visible part).
 * Transcribed from ScummVM room.cpp (startScene, setupRoomSubBlocks) and object.cpp (the v4
 * path: object tables, positions, findObject, the inventory).  Actors and the camera live in
 * actor.c, the walk boxes in boxes.c.
 */
#include <stdio.h>
#include <string.h>
#include "scumm.h"

/* --- global object tables ------------------------------------------------------ */

static int smallHeaderClass(int cls)
{
    cls &= 0x7F;
    switch (cls) {               /* GF_SMALL_HEADER remap (object.cpp getClass/putClass) */
    case 32: return 24;          /* kObjectClassUntouchable */
    case 31: return 23;          /* kObjectClassPlayer */
    case 30: return 19;          /* kObjectClassXFlip */
    case 29: return 18;          /* kObjectClassYFlip */
    default: return cls;
    }
}

int getClass(int obj, int cls)
{
    cls = smallHeaderClass(cls);
    return (g.classData[obj] & (1u << (cls - 1))) != 0;
}

void putClass(int obj, int cls, int set)
{
    cls = smallHeaderClass(cls);
    if (set)
        g.classData[obj] |= (1u << (cls - 1));
    else
        g.classData[obj] &= ~(1u << (cls - 1));
    if (obj >= 1 && obj < NUM_ACTORS) {          /* Actor::classChanged */
        Actor *a = &g.actors[obj];
        if (cls == 21) a->forceClip = (uint8_t)set;     /* kObjectClassAlwaysClip */
        if (cls == 22) a->ignoreBoxes = (uint8_t)set;   /* kObjectClassIgnoreBoxes */
    }
}

int getOwner(int obj)               { return g.objectOwnerTable[obj]; }
void putOwner(int obj, int owner)   { g.objectOwnerTable[obj] = (uint8_t)owner; }
int getState(int obj)               { return g.objectStateTable[obj]; }
void putState(int obj, int state)   { g.objectStateTable[obj] = (uint8_t)state; }

/* --- room objects -------------------------------------------------------------- */

int getObjectIndex(int object)
{
    int i;
    if (object < 1)
        return -1;
    for (i = NUM_LOCAL_OBJECTS - 1; i > 0; i--)
        if (g.objs[i].obj_nr == object)
            return i;
    return -1;
}

int whereIsObject(int object)
{
    int i;
    if (object >= NUM_GLOBAL_OBJECTS || object < 1)
        return WIO_NOT_FOUND;
    if (g.objectOwnerTable[object] != OF_OWNER_ROOM) {
        for (i = 0; i < NUM_INVENTORY; i++)
            if (g.inventory[i] == object)
                return WIO_INVENTORY;
        return WIO_NOT_FOUND;
    }
    for (i = NUM_LOCAL_OBJECTS - 1; i > 0; i--)
        if (g.objs[i].obj_nr == object)
            return g.objs[i].fl_object_index ? WIO_FLOBJECT : WIO_ROOM;
    return WIO_NOT_FOUND;
}

uint32_t getOBCDOffs(int object)
{
    int i;
    if (g.objectOwnerTable[object] != OF_OWNER_ROOM)
        return 0;
    for (i = NUM_LOCAL_OBJECTS - 1; i > 0; i--)
        if (g.objs[i].obj_nr == object)
            return g.objs[i].fl_object_index ? 8 : g.objs[i].OBCDoffset;
    return 0;
}

const uint8_t *getOBCDFromObject(int obj)
{
    int i;
    if (g.objectOwnerTable[obj] != OF_OWNER_ROOM) {
        for (i = 0; i < NUM_INVENTORY; i++)
            if (g.inventory[i] == obj)
                return g.inventoryPtr[i];
        return 0;
    }
    for (i = NUM_LOCAL_OBJECTS - 1; i > 0; --i)
        if (g.objs[i].obj_nr == obj)
            return g.roomPtr + g.objs[i].OBCDoffset;
    return 0;
}

static int findLocalObjectSlot(void)
{
    int i;
    for (i = 1; i < NUM_LOCAL_OBJECTS; i++) {
        if (!g.objs[i].obj_nr) {
            memset(&g.objs[i], 0, sizeof(g.objs[i]));
            return i;
        }
    }
    return -1;
}

/* the blocks of 'room' with tag t, in order (ResourceIterator over a small-header room) */
static const uint8_t *nextBlock(const uint8_t *room, uint32_t *pos, char t0, char t1)
{
    uint32_t size = rd32(room);
    while (*pos + 6 <= size) {
        const uint8_t *p = room + *pos;
        uint32_t s = rd32(p);
        if (s < 6)
            break;
        *pos += s;
        if (p[4] == (uint8_t)t0 && p[5] == (uint8_t)t1)
            return p;
    }
    return 0;
}

/* ScummEngine_v4::resetRoomObjects + resetRoomObject */
static void resetRoomObjects(void)
{
    const uint8_t *room = g.roomPtr, *ptr;
    uint32_t pos;
    int i, j;

    if (g.numObjectsInRoom == 0)
        return;
    pos = 6;
    for (i = 0; i < g.numObjectsInRoom; i++) {
        ObjectData *od = &g.objs[findLocalObjectSlot()];
        ptr = nextBlock(room, &pos, 'O', 'C');
        if (!ptr)
            plat_error("Room missing object code block(s)");
        od->OBCDoffset = (uint32_t)(ptr - room);
        od->obj_nr = rd16(ptr + 6);
    }
    pos = 6;
    for (i = 0; i < g.numObjectsInRoom; i++) {
        uint16_t id;
        ptr = nextBlock(room, &pos, 'O', 'I');
        if (!ptr)
            break;
        id = rd16(ptr + 6);
        for (j = 1; j < NUM_LOCAL_OBJECTS; j++)
            if (g.objs[j].obj_nr == id)
                g.objs[j].OBIMoffset = (uint32_t)(ptr - room);
    }
    for (i = 1; i < NUM_LOCAL_OBJECTS; i++) {
        ObjectData *od = &g.objs[i];
        if (od->obj_nr && !od->fl_object_index) {
            ptr = room + od->OBCDoffset;
            od->obj_nr = rd16(ptr + 6);
            od->x_pos = (int16_t)(ptr[9] * 8);
            od->y_pos = (int16_t)((ptr[10] & 0x7F) * 8);
            od->parentstate = (ptr[10] & 0x80) ? 1 : 0;
            od->width = (uint16_t)(ptr[11] * 8);
            od->parent = ptr[12];
            od->walk_x = (int16_t)rd16(ptr + 13);
            od->walk_y = (int16_t)rd16(ptr + 15);
            od->actordir = ptr[17] & 7;
            od->height = ptr[17] & 0xF8;
        }
    }
}

/* ScummEngine::setupRoomSubBlocks, GF_SMALL_HEADER */
static void setupRoomSubBlocks(void)
{
    const uint8_t *room = g.roomPtr, *ptr;
    uint32_t pos;

    g.ENCD_offs = g.EXCD_offs = 0;
    ptr = res_find_data(room, "HD");
    g.roomWidth = rd16(ptr);
    g.roomHeight = rd16(ptr + 2);
    g.numObjectsInRoom = (uint8_t)rd16(ptr + 4);
    g.roomImage = res_find_data(room, "BM");     /* _IM00_offs: IM00 is BM with small headers */
    if (!g.roomImage)
        plat_error("setupRoomSubBlocks: the room has no image");
    /* Rooms 88 and 94 have a BM block of 2 bytes (height 0): rooms without a picture.
     * ScummVM reads an LE32 there anyway and walks out of the resource (in 94 it dies, in 88
     * it happens to survive).  Here such a room has no image: nothing to draw, one plane. */
    if (rd32(g.roomImage - 6) < 6 + 4)
        g.roomImage = 0;

    ptr = res_find_data(room, "EX");
    if (ptr)
        g.EXCD_offs = (uint32_t)(ptr - room);
    ptr = res_find_data(room, "EN");
    if (ptr)
        g.ENCD_offs = (uint32_t)(ptr - room);

    memset(g.localScriptOffsets, 0, sizeof(g.localScriptOffsets));
    pos = 6;
    while ((ptr = nextBlock(room, &pos, 'L', 'S')) != 0) {
        int id;
        ptr += 6;
        id = ptr[0];
        g.localScriptOffsets[id - NUM_GLOBAL_SCRIPTS] = (uint32_t)(ptr + 1 - room);
    }
}

/* initBGBuffers, the count (GF_SMALL_HEADER): the z-planes are chained after the image by
 * their LE16 sizes, at most 4 counted.  The buffers are the platform's. */
static void initBGBuffers(void)
{
    const uint8_t *ptr = g.roomImage;
    unsigned off;
    if (!ptr) {                                  /* a room without a picture (88, 94) */
        g.numZBuffer = 1;
        return;
    }
    off = rd32(ptr);
    g.numZBuffer = 0;
    while (off && g.numZBuffer < 4) {
        g.numZBuffer++;
        ptr += off;
        off = rd16(ptr);
    }
    if (g.numZBuffer < 1 || g.numZBuffer > 8)
        plat_error("initBGBuffers: numZBuffer");
}

/* ScummEngine::startScene (v4 path; a == NULL and objectNr == 0 for v4) */
void startScene(int room)
{
    int i;

    if (g.trace) {
        char tbuf[32];
        sprintf(tbuf, "Loading room %d", room);
        plat_trace(tbuf);
    }
    stopTalk();
    fadeOut(g.switchRoomEffect2);
    g.newEffect = g.switchRoomEffect;

    if (g.currentScript != 0xFF) {
        ScriptSlot *ss = &g.slot[g.currentScript];
        if (ss->where == WIO_ROOM || ss->where == WIO_FLOBJECT || ss->where == WIO_LOCAL)
            g.currentScript = 0xFF;
    }

    runExitScript();
    killScriptsAndResources();
    stopCycle(0);
    clearDrawObjectQueue();                      /* clearDrawQueues */

    for (i = 1; i < NUM_ACTORS; i++)
        actor_hide(&g.actors[i]);
    for (i = 0; i < 256; i++) {
        g.roomPalette[i] = (uint8_t)i;
        g.shadowPalette[i] = (uint8_t)i;
    }
    g.roomPaletteGen++;
    setDirtyColors(0, 255);                      /* GF_SMALL_HEADER */

    VAR(VAR_ROOM) = room;
    g.fullRedraw = 1;
    g.currentRoom = room;
    VAR(VAR_ROOM) = room;

    if (room >= 0x80)
        g.roomResource = g.resourceMapper[room & 0x7F];
    else
        g.roomResource = room;
    VAR(VAR_ROOM_RESOURCE) = g.roomResource;

    g.roomPtr = room ? res_room(g.roomResource) : 0;
    if (room && !g.roomPtr)
        plat_error("startScene: the room is not in the game (ScummVM asks for disk 0)");
    plat_room();                                 /* the platform shows the new room */

    for (i = 0; i < NUM_LOCAL_OBJECTS; i++)      /* clearRoomObjects */
        g.objs[i].obj_nr = 0;

    if (g.currentRoom == 0) {
        g.ENCD_offs = g.EXCD_offs = 0;
        g.numObjectsInRoom = 0;
        return;
    }

    setupRoomSubBlocks();
    boxes_resetRoom(g.roomPtr);                  /* resetRoomSubBlocks: boxes, scale slots */
    palette_resetRoom();                         /* resetRoomSubBlocks: the CLUT, the cycles */
    initBGBuffers();
    plat_initBGBuffers(g.roomHeight);
    resetRoomObjects();

    VAR(VAR_CAMERA_MIN_X) = g.screenWidth / 2;
    VAR(VAR_CAMERA_MAX_X) = g.roomWidth - (g.screenWidth / 2);
    g.camera.mode = 1;                           /* kNormalCameraMode */
    g.camera.cur_x = g.camera.dest_x = g.screenWidth / 2;
    g.camera.cur_y = g.camera.dest_y = g.screenHeight / 2;

    if (g.roomResource == 0)
        return;

    gfx_clearUsageBits();

    showActors();
    g.egoPositioned = 0;
    runEntryScript();
    g.doEffect = 1;
}

/* --- object state, positions, findObject (object.cpp) ------------------------------- */

/* updateObjectStates: the room objects' copy of their global state, once per loop */
void updateObjectStates(void)
{
    int i;
    for (i = 1; i < NUM_LOCAL_OBJECTS; i++)
        if (g.objs[i].obj_nr > 0)
            g.objs[i].state = (uint8_t)getState(g.objs[i].obj_nr);
}

static int objIsActor(int obj) { return obj < NUM_ACTORS; }

/* getObjectXYPos (v3-v5): the walk-to point and the direction to face */
void getObjectXYPos(int object, int *x, int *y, int *dir)
{
    int idx = getObjectIndex(object);
    if (idx < 0) {
        plat_error("getObjectXYPos: object not in the room");
        return;
    }
    *x = g.objs[idx].walk_x;
    *y = g.objs[idx].walk_y;
    if (dir)
        *dir = oldDirToNewDir(g.objs[idx].actordir & 3);
}

int getObjectOrActorXY(int object, int *x, int *y)
{
    Actor *act;
    if (objIsActor(object)) {
        if (!isValidActor(object))
            return -1;                           /* derefActorSafe: NULL */
        act = &g.actors[object];
        if (actor_inCurrentRoom(act)) {
            *x = act->x;
            *y = act->y;
            return 0;
        }
        return -1;
    }
    switch (whereIsObject(object)) {
    case WIO_NOT_FOUND:
        return -1;
    case WIO_INVENTORY:
        if (objIsActor(g.objectOwnerTable[object])) {
            act = derefActor(g.objectOwnerTable[object]);
            if (actor_inCurrentRoom(act)) {
                *x = act->x;
                *y = act->y;
                return 0;
            }
        }
        return -1;
    default:
        break;
    }
    getObjectXYPos(object, x, y, 0);
    return 0;
}

int getObjectOrActorWidth(int object, int *width)
{
    Actor *act;
    if (objIsActor(object)) {
        if (!isValidActor(object))
            return -1;
        act = &g.actors[object];
        if (actor_inCurrentRoom(act)) {
            *width = act->width;
            return 0;
        }
        return -1;
    }
    switch (whereIsObject(object)) {
    case WIO_NOT_FOUND:
        return -1;
    case WIO_INVENTORY:
        if (objIsActor(g.objectOwnerTable[object])) {
            act = derefActor(g.objectOwnerTable[object]);
            if (actor_inCurrentRoom(act)) {
                *width = act->width;
                return 0;
            }
        }
        return -1;
    default:
        break;
    }
    {   /* getObjectWidth */
        int idx = getObjectIndex(object);
        *width = idx >= 0 ? g.objs[idx].width : 0;
    }
    return 0;
}

int getObjX(int obj)
{
    int x, y;
    if (obj < 1)
        return 0;
    if (objIsActor(obj))
        return derefActor(obj)->x;
    if (whereIsObject(obj) == WIO_NOT_FOUND)
        return -1;
    if (getObjectOrActorXY(obj, &x, &y) == -1)
        return -1;
    return x;
}

int getObjY(int obj)
{
    int x, y;
    if (obj < 1)
        return 0;
    if (objIsActor(obj))
        return derefActor(obj)->y;
    if (whereIsObject(obj) == WIO_NOT_FOUND)
        return -1;
    if (getObjectOrActorXY(obj, &x, &y) == -1)
        return -1;
    return y;
}

/* findObject: the first touchable object whose rectangle holds (x, y), its parents in the
 * states that show it */
int findObject(int x, int y)
{
    int i, b;
    uint8_t a;
    for (i = 1; i < NUM_LOCAL_OBJECTS; i++) {
        if (g.objs[i].obj_nr < 1 || getClass(g.objs[i].obj_nr, 32))     /* Untouchable */
            continue;
        b = i;
        do {
            a = g.objs[b].parentstate;
            b = g.objs[b].parent;
            if (b == 0) {
                if (g.objs[i].x_pos <= x && g.objs[i].width + g.objs[i].x_pos > x &&
                    g.objs[i].y_pos <= y && g.objs[i].height + g.objs[i].y_pos > y)
                    return g.objs[i].obj_nr;
                break;
            }
        } while ((g.objs[b].state & 0xF) == a);
    }
    return 0;
}

/* --- new object names (object.cpp setObjectName, getObjOrActorName) -------------------------
 * ScummVM copies the new name, inline in the script, into an rtObjectName resource.  Here the
 * slot keeps a pointer to it: scripts live in the game image, read-only and permanent. */

#define NUM_NEW_NAMES 50                /* resource_v4.cpp: _numNewNames */
static uint16_t newNames[NUM_NEW_NAMES];
static const uint8_t *newNamePtr[NUM_NEW_NAMES];

void setObjectName(int obj, const uint8_t *name)
{
    int i;

    if (obj < NUM_ACTORS)
        plat_error("Can't set actor name with new-name-of");

    for (i = 0; i < NUM_NEW_NAMES; i++) {
        if (newNames[i] == obj) {
            newNamePtr[i] = 0;                  /* nukeResource(rtObjectName, i) */
            newNames[i] = 0;
            break;
        }
    }
    for (i = 0; i < NUM_NEW_NAMES; i++) {
        if (newNames[i] == 0) {
            newNamePtr[i] = name;               /* loadPtrToResource(rtObjectName, i, nullptr) */
            newNames[i] = (uint16_t)obj;
            runInventoryScript(0);
            return;
        }
    }
    plat_error("New name overflows name table");
}

const uint8_t *getNewName(int obj)
{
    int i;
    for (i = 0; i < NUM_NEW_NAMES; i++)
        if (newNames[i] == obj)
            return newNamePtr[i];
    return 0;
}

/* killScriptsAndResources, "nuke local object names": a name whose object nobody owns any
 * more, or a room owns, goes. */
void nukeLocalObjectNames(void)
{
    int i;
    for (i = 0; i < NUM_NEW_NAMES; i++) {
        int obj = newNames[i];
        if (obj) {
            int owner = getOwner(obj);
            if (owner == 0 || owner == OF_OWNER_ROOM) {
                newNames[i] = 0;
                newNamePtr[i] = 0;
            }
        }
    }
}

void resetNewNames(void)
{
    int i;
    for (i = 0; i < NUM_NEW_NAMES; i++) {
        newNames[i] = 0;
        newNamePtr[i] = 0;
    }
}

/* --- the inventory (object.cpp) -------------------------------------------------------- */

void runInventoryScript(int i)
{
    if (VAR(VAR_INVENTORY_SCRIPT)) {
        int32_t args[NUM_SCRIPT_LOCAL];
        memset(args, 0, sizeof(args));
        args[0] = i;
        runScript(VAR(VAR_INVENTORY_SCRIPT), 0, 0, args);
    }
}

int findInventory(int owner, int idx)
{
    int count = 1, i, obj;
    for (i = 0; i < NUM_INVENTORY; i++) {
        obj = g.inventory[i];
        if (obj && getOwner(obj) == owner && count++ == idx)
            return obj;
    }
    return 0;
}

int getInventoryCount(int owner)
{
    int i, obj, count = 0;
    for (i = 0; i < NUM_INVENTORY; i++) {
        obj = g.inventory[i];
        if (obj && getOwner(obj) == owner)
            count++;
    }
    return count;
}

/* addObjectToInventory: the inventory keeps the object's OC block.  The game image is
 * read-only and lives as long as the game, so the block is referenced, not copied. */
void addObjectToInventory(int obj, int room)
{
    int i, slot = -1, idx;
    (void)room;                                  /* findObjectInRoom: the current room */
    for (i = 0; i < NUM_INVENTORY; i++)
        if (g.inventory[i] == 0) {
            slot = i;
            break;
        }
    if (slot < 0) {
        plat_error("Inventory full");
        return;
    }
    idx = getObjectIndex(obj);
    if (idx < 0 || g.objs[idx].fl_object_index) {
        plat_error("addObjectToInventory: flobjects not yet implemented");
        return;
    }
    g.inventory[slot] = (uint16_t)obj;
    g.inventoryPtr[slot] = g.roomPtr + g.objs[idx].OBCDoffset;
}

static void clearOwnerOf(int obj)
{
    int i;
    stopObjectScript(obj);
    if (getOwner(obj) == OF_OWNER_ROOM) {
        for (i = 0; i < NUM_LOCAL_OBJECTS; i++)
            if (g.objs[i].obj_nr == obj && g.objs[i].fl_object_index) {
                g.objs[i].obj_nr = 0;
                g.objs[i].fl_object_index = 0;
            }
    } else {
        for (i = 0; i < NUM_INVENTORY; i++) {
            if (g.inventory[i] == obj) {
                g.inventory[i] = 0;
                g.inventoryPtr[i] = 0;
                for (i = 0; i < NUM_INVENTORY - 1; i++)      /* v >= 4: close the gap */
                    if (!g.inventory[i] && g.inventory[i + 1]) {
                        g.inventory[i] = g.inventory[i + 1];
                        g.inventoryPtr[i] = g.inventoryPtr[i + 1];
                        g.inventory[i + 1] = 0;
                        g.inventoryPtr[i + 1] = 0;
                    }
                break;
            }
        }
    }
}

void setOwnerOf(int obj, int owner)
{
    if (obj <= 0) {
        plat_error("setOwnerOf: bad object");
        return;
    }
    if (owner == 0) {
        clearOwnerOf(obj);
        if (g.currentScript != 0xFF && g.slot[g.currentScript].where == WIO_INVENTORY) {
            ScriptSlot *ss = &g.slot[g.currentScript];
            if ((ss->number < NUM_INVENTORY && g.inventory[ss->number] == obj) || ss->number == obj)
                plat_error("setOwnerOf: odd case (ScummVM errors here too)");
        }
    }
    putOwner(obj, owner);
    runInventoryScript(0);                       /* v < 6: argument 0 */
}

/* --- save and load (saveload.c) ------------------------------------------------------- */
void world_saveLoad(Ser *s)
{
    int i;
    SER_ARR(s, newNames);
    for (i = 0; i < NUM_NEW_NAMES; i++)
        ser_imgptr(s, &newNamePtr[i]);
}

/* loadState after the state is read: the room's static data from the image (not its boxes
 * nor its palette: the save has them, as ScummVM's), the screens, a complete redraw, the
 * text surface cleared, the camera, the buffers.  No entry script runs. */
void world_afterLoad(void)
{
    g.roomPtr = g.currentRoom ? res_room(g.roomResource) : 0;
    if (g.currentRoom && !g.roomPtr)
        plat_error("load: the room is not in the game");
    g.roomPaletteGen++;
    plat_room();
    if (g.currentRoom)
        setupRoomSubBlocks();
    g.camera.last_x = g.camera.cur_x;
    loop_afterLoad();                        /* initScreens(_screenB, _screenH) */
    g.completeScreenRedraw = 1;
    text_afterLoad();                        /* _charset->_hasMask = false, clearTextSurface */
    gfx_afterLoad();                         /* _drawObjectQueNr = 0 */
    g.verbMouseOver = 0;
    actor_cameraMoved();
    if (g.currentRoom) {
        initBGBuffers();
        plat_initBGBuffers(g.roomHeight);
    }
}
