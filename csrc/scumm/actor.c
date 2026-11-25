/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * actor.c - the actors: placement, walking along the boxes, turning, the animation state the
 * scripts can see; and the camera that follows them.
 *
 * ScummVM actor.cpp, the base Actor class (what v4 uses: ScummEngine::resetScumm creates
 * Actor_v3 for v3 only): initActor, setActorWalkSpeed, calcMovementFactor, actorWalkStep,
 * startWalkActor, startWalkAnim, walkActor, remapDirection, updateActorDirection,
 * setDirection, faceToObject, turnToDirection, putActor, adjustXYToBeInBox, adjustActorPos,
 * hideActor, showActor, startAnimActor, animateActor, setActorCostume, walkActors, showActors.
 * camera.cpp, the v < 7 paths: setCameraAt, setCameraFollows, moveCamera, cameraMoved,
 * actorFollowCamera.
 *
 * The costume (_cost: costumeDecodeData, animateCostume, drawActorCostume's geometry) is in
 * costume.c; processActors, with the strips the actors cover, in gfx.c.
 */
#include <string.h>
#include "scumm.h"

static int iabs(int v) { return v < 0 ? -v : v; }

int newDirToOldDir(int dir)
{
    if (dir >= 71 && dir <= 109)
        return 1;
    if (dir >= 109 && dir <= 251)
        return 2;
    if (dir >= 251 && dir <= 289)
        return 0;
    return 3;
}

int oldDirToNewDir(int dir)
{
    static const int new_dir_table[4] = { 270, 90, 180, 0 };
    return new_dir_table[dir & 3];
}

int isValidActor(int id)
{
    return id >= 0 && id < NUM_ACTORS;
}

/* actor 0 exists and is valid, as in ScummVM (it only logs a debug line for it) */
Actor *derefActor(int id)
{
    if (!isValidActor(id)) {
        plat_error("derefActor: invalid actor");
        return &g.actors[0];
    }
    return &g.actors[id];
}

int actor_inCurrentRoom(const Actor *a)
{
    return a->room == g.currentRoom;
}

static int isPlayer(const Actor *a)
{
    return getClass(a->number, 31);          /* kObjectClassPlayer */
}

static int isInClass(const Actor *a, int cls)
{
    return getClass(a->number, cls);
}

/* --- set-up --------------------------------------------------------------------------- */

void actor_stopMoving(Actor *a)
{
    if (a->walkScript)
        stopScript(a->walkScript);
    a->moving = 0;
}

static int calcMovementFactor(Actor *a, Point next);

void actor_setWalkSpeed(Actor *a, int sx, int sy)
{
    if (sx == a->speedx && sy == a->speedy)
        return;
    a->speedx = (uint8_t)sx;
    a->speedy = (uint8_t)sy;
    if (a->moving)
        calcMovementFactor(a, a->walkdata.next);
}

void actor_init(Actor *a, int mode)
{
    if (mode == -1) {
        a->top = a->bottom = 0;
        a->visible = 0;
        a->needRedraw = 0;
        a->needBgReset = 0;
        a->speedx = 8;
        a->speedy = 2;
        a->frame = 0;
        a->walkbox = 0;
        a->animProgress = 0;
        memset(a->palette, 0, sizeof(a->palette));
        a->costumeNeedsInit = 0;
        costume_reset(&a->cost);
        memset(&a->walkdata, 0, sizeof(a->walkdata));
        a->walkdata.point3.x = 32000;
        a->walkScript = 0;
    }
    if (mode == 1 || mode == -1) {
        a->costume = 0;
        a->room = 0;
        a->x = a->y = 0;
        a->facing = 180;
    } else if (mode == 2) {
        a->facing = 180;
    }
    a->elevation = 0;
    a->width = 24;
    a->talkColor = 15;
    a->talkPosX = 0;
    a->talkPosY = -80;
    a->boxscale = a->scaley = a->scalex = 0xFF;
    a->sound = 0;
    a->targetFacing = a->facing;
    a->lastValidX = 0;
    a->lastValidY = 0;
    a->shadowMode = 0;

    actor_stopMoving(a);
    actor_setWalkSpeed(a, 8, 2);

    a->animSpeed = 0;
    a->ignoreBoxes = 0;
    a->forceClip = 0;
    a->ignoreTurns = 0;
    a->initFrame = 1;
    a->walkFrame = 2;
    a->standFrame = 3;
    a->talkStartFrame = 4;
    a->talkStopFrame = 5;
    a->walkScript = 0;
    a->talkScript = 0;

    g.classData[a->number] = 0;
}

void actor_setupScale(Actor *a)
{
    unsigned scale;
    if (a->ignoreBoxes)
        return;
    a->boxscale = (uint16_t)getBoxScale(a->walkbox);
    scale = (unsigned)getScale(a->walkbox, a->x, a->y);
    a->scalex = a->scaley = (uint8_t)scale;
}

static void setBox(Actor *a, int box)
{
    a->walkbox = (uint8_t)box;
    actor_setupScale(a);
}

/* --- animation state (the costume decoding itself comes with the drawing slice) --------- */

void actor_startAnim(Actor *a, int f)
{
    switch (f) {                             /* CHORE_REDIRECT_* */
    case 0x38: f = a->initFrame; break;
    case 0x39: f = a->walkFrame; break;
    case 0x3A: f = a->standFrame; break;
    case 0x3B: f = a->talkStartFrame; break;
    case 0x3C: f = a->talkStopFrame; break;
    default: break;
    }
    if (actor_inCurrentRoom(a) && a->costume != 0) {
        a->animProgress = 0;
        a->needRedraw = 1;
        a->cost.animCounter = 0;
        if (f == a->initFrame)
            costume_reset(&a->cost);
        costumeDecodeData(a, f, (unsigned)-1);
        a->frame = (uint8_t)f;
    }
}

void actor_setDirection(Actor *a, int direction)
{
    direction = (direction + 360) % 360;
    if (a->facing == direction)
        return;
    a->facing = (int16_t)direction;
    if (a->costume == 0)
        return;
    if (!actor_inCurrentRoom(a))
        return;
    /* the limbs re-decoded for the new direction (v1-v6: the direction kept in frame[]) */
    {
        unsigned aMask = 0x8000;
        int i;
        for (i = 0; i < 16; i++, aMask >>= 1) {
            unsigned vald = a->cost.frame[i];
            if (vald == 0xFFFF)
                continue;
            if ((int)(vald & 3) == newDirToOldDir(a->facing))
                continue;
            vald >>= 2;
            costumeDecodeData(a, (int)vald, aMask);
        }
    }
    a->needRedraw = 1;
}

void actor_turnToDirection(Actor *a, int newdir)
{
    if (newdir == -1 || a->ignoreTurns)
        return;
    a->targetFacing = (int16_t)newdir;
    a->moving = MF_TURN;
}

static int remapDirection(Actor *a, int dir, int is_walking)
{
    int flags, flipX, flipY;

    /* v < 5: the boxes count even when the actor ignores them; _extraBoxFlags are v7+ */
    flags = getBoxFlags(a->walkbox);

    flipX = (a->walkdata.deltaXFactor > 0);
    flipY = (a->walkdata.deltaYFactor > 0);

    if ((flags & kBoxXFlip) || isInClass(a, 30)) {      /* kObjectClassXFlip */
        dir = 360 - dir;
        flipX = !flipX;
    }
    if ((flags & kBoxYFlip) || isInClass(a, 29)) {      /* kObjectClassYFlip */
        dir = 180 - dir;
        flipY = !flipY;
    }

    switch (flags & 7) {
    case 1:
        if (is_walking)
            return flipX ? 90 : 270;
        return (dir == 90) ? 90 : 270;
    case 2:
        if (is_walking)
            return flipY ? 180 : 0;
        return (dir == 0) ? 0 : 180;
    case 3: return 270;
    case 4: return 90;
    case 5: return 0;
    case 6: return 180;
    default: break;
    }

    dir = (dir + 360) % 360;
    return dir | 0x400;
}

static int updateActorDirection(Actor *a, int is_walking)
{
    static const uint8_t actorTurnInterpolateTable[] =
        { 0, 2, 2, 3, 2, 1, 2, 3, 0, 1, 2, 1, 0, 1, 0, 3 };
    int dir = remapDirection(a, a->targetFacing, is_walking);
    if (dir & 0x400)
        dir = oldDirToNewDir(actorTurnInterpolateTable[newDirToOldDir(dir & 0x3FF) |
                                                       (newDirToOldDir(a->facing) << 2)]);
    return dir;
}

static void startWalkAnim(Actor *a, int cmd, int angle)
{
    if (angle == -1)
        angle = a->facing;
    if (a->walkScript) {
        int32_t args[NUM_SCRIPT_LOCAL];
        memset(args, 0, sizeof(args));
        args[0] = a->number;
        args[1] = cmd;
        args[2] = angle;
        runScript(a->walkScript, 1, 0, args);
    } else {
        if (cmd == 3)
            actor_turnToDirection(a, angle);
        else
            actor_setDirection(a, angle);
        if (cmd == 1)
            actor_startAnim(a, a->walkFrame);
        else if (cmd == 3)
            actor_startAnim(a, a->standFrame);
    }
}

/* --- walking ------------------------------------------------------------------------- */

static int actorWalkStep(Actor *a)
{
    int nextFacing, distX, distY, tmpX, tmpY;

    a->needRedraw = 1;

    nextFacing = updateActorDirection(a, 1);
    if ((a->walkFrame != a->frame && !(a->moving & MF_IN_LEG)) || a->facing != nextFacing)
        startWalkAnim(a, 1, nextFacing);
    a->moving |= MF_IN_LEG;

    if (a->walkbox != a->walkdata.curbox && checkXYInBoxBounds(a->walkdata.curbox, a->x, a->y))
        setBox(a, a->walkdata.curbox);

    distX = iabs(a->walkdata.next.x - a->walkdata.cur.x);
    distY = iabs(a->walkdata.next.y - a->walkdata.cur.y);

    if (iabs(a->x - a->walkdata.cur.x) >= distX && iabs(a->y - a->walkdata.cur.y) >= distY) {
        a->moving &= ~MF_IN_LEG;
        return 0;
    }

    tmpX = a->x * 0x10000 + a->walkdata.xfrac + (a->walkdata.deltaXFactor >> 8) * a->scalex;
    a->walkdata.xfrac = (uint16_t)tmpX;
    a->x = (int16_t)(tmpX >> 16);

    tmpY = a->y * 0x10000 + a->walkdata.yfrac + (a->walkdata.deltaYFactor >> 8) * a->scaley;
    a->walkdata.yfrac = (uint16_t)tmpY;
    a->y = (int16_t)(tmpY >> 16);

    if (iabs(a->x - a->walkdata.cur.x) > distX)
        a->x = a->walkdata.next.x;
    if (iabs(a->y - a->walkdata.cur.y) > distY)
        a->y = a->walkdata.next.y;

    if (a->x == a->walkdata.next.x && a->y == a->walkdata.next.y) {   /* v4-v6 */
        a->moving &= ~MF_IN_LEG;
        return 0;
    }
    return 1;
}

static int calcMovementFactor(Actor *a, Point next)
{
    int diffX, diffY;
    int32_t deltaXFactor, deltaYFactor;

    if (a->x == next.x && a->y == next.y)
        return 0;

    diffX = next.x - a->x;
    diffY = next.y - a->y;

    deltaYFactor = (int32_t)a->speedy << 16;
    if (diffY < 0)
        deltaYFactor = -deltaYFactor;

    deltaXFactor = deltaYFactor * diffX;
    if (diffY != 0)
        deltaXFactor /= diffY;
    else
        deltaYFactor = 0;

    if ((unsigned)iabs(deltaXFactor / 0x10000) > a->speedx) {
        deltaXFactor = (int32_t)a->speedx << 16;
        if (diffX < 0)
            deltaXFactor = -deltaXFactor;
        deltaYFactor = deltaXFactor * diffY;
        if (diffX != 0)
            deltaYFactor /= diffX;
        else
            deltaXFactor = 0;
    }

    a->walkdata.xfrac = 0;
    a->walkdata.yfrac = 0;
    a->walkdata.cur.x = a->x;
    a->walkdata.cur.y = a->y;
    a->walkdata.next = next;
    a->walkdata.deltaXFactor = deltaXFactor;
    a->walkdata.deltaYFactor = deltaYFactor;

    a->targetFacing = (int16_t)((iabs(diffY) * 3 > iabs(diffX)) ? (deltaYFactor > 0 ? 180 : 0)
                                                                 : (deltaXFactor > 0 ? 90 : 270));
    return actorWalkStep(a);
}

static int inBoxQuickReject(BoxCoords box, int x, int y, int threshold)
{
    int t;
    t = x - threshold;
    if (t > box.ul.x && t > box.ur.x && t > box.lr.x && t > box.ll.x) return 1;
    t = x + threshold;
    if (t < box.ul.x && t < box.ur.x && t < box.lr.x && t < box.ll.x) return 1;
    t = y - threshold;
    if (t > box.ul.y && t > box.ur.y && t > box.lr.y && t > box.ll.y) return 1;
    t = y + threshold;
    if (t < box.ul.y && t < box.ur.y && t < box.lr.y && t < box.ll.y) return 1;
    return 0;
}

typedef struct { int16_t x, y; uint8_t box; } AdjustBoxResult;

static AdjustBoxResult adjustXYToBeInBox(Actor *a, int dstX, int dstY)
{
    static const unsigned thresholdTable[] = { 30, 80, 0 };
    AdjustBoxResult abr;
    int16_t tmpX, tmpY;
    int tmpDist, bestDist, threshold, numBoxes, box, tIdx;
    uint8_t flags, bestBox;

    abr.x = a->lastValidX;                   /* the old system (v <= 4) */
    abr.y = a->lastValidY;
    abr.box = INVALID_BOX;

    if (a->ignoreBoxes) {
        abr.x = (int16_t)dstX;
        abr.y = (int16_t)dstY;
        return abr;
    }

    for (tIdx = 0; tIdx < 3; tIdx++) {
        threshold = (int)thresholdTable[tIdx];
        numBoxes = getNumBoxes() - 1;
        if (numBoxes < 0)                    /* firstValidBox = 0 for small headers */
            return abr;
        bestDist = 0xFFFF;
        bestBox = INVALID_BOX;

        for (box = numBoxes; box >= 0; box--) {
            flags = (uint8_t)getBoxFlags(box);
            if ((flags & kBoxInvisible) && !((flags & kBoxPlayerOnly) && !isPlayer(a)))
                continue;
            if (threshold > 0 && inBoxQuickReject(getBoxCoordinates(box), dstX, dstY, threshold))
                continue;
            if (checkXYInBoxBounds(box, dstX, dstY)) {
                a->lastValidX = (int16_t)dstX;
                a->lastValidY = (int16_t)dstY;
                abr.x = (int16_t)dstX;
                abr.y = (int16_t)dstY;
                abr.box = (uint8_t)box;
                return abr;
            }
            tmpDist = getClosestPtOnBox(getBoxCoordinates(box), dstX, dstY, &tmpX, &tmpY);
            if (tmpDist < bestDist) {
                a->lastValidX = tmpX;
                a->lastValidY = tmpY;
                abr.x = tmpX;
                abr.y = tmpY;
                if (tmpDist == 0) {
                    abr.box = (uint8_t)box;
                    return abr;
                }
                bestDist = tmpDist;
                bestBox = (uint8_t)box;
            }
        }
        if (threshold == 0 || threshold * threshold >= bestDist) {
            abr.box = bestBox;
            return abr;
        }
    }
    return abr;
}

void actor_adjustXYToBeInBox(Actor *a, int x, int y, int *ox, int *oy)
{
    AdjustBoxResult abr = adjustXYToBeInBox(a, x, y);
    *ox = abr.x;
    *oy = abr.y;
}

static void adjustActorPos(Actor *a)
{
    AdjustBoxResult abr = adjustXYToBeInBox(a, a->x, a->y);
    a->x = abr.x;
    a->y = abr.y;
    a->walkdata.destbox = abr.box;
    setBox(a, abr.box);
    a->walkdata.dest.x = -1;
    actor_stopMoving(a);
    a->cost.soundCounter = 0;
    if (a->walkbox != INVALID_BOX) {
        int flags = getBoxFlags(a->walkbox);
        if (flags & 7)
            actor_turnToDirection(a, a->facing);
    }
}

void actor_startWalk(Actor *a, int destX, int destY, int dir)
{
    AdjustBoxResult abr;

    abr.x = (int16_t)destX;                  /* v <= 4: no adjustment here */
    abr.y = (int16_t)destY;
    abr.box = INVALID_BOX;

    if (!actor_inCurrentRoom(a)) {
        a->x = abr.x;
        a->y = abr.y;
        if (!a->ignoreTurns && dir != -1)
            a->facing = (int16_t)dir;
        return;
    }

    if (a->ignoreBoxes) {
        abr.box = INVALID_BOX;
        a->walkbox = INVALID_BOX;
    } else {
        if (checkXYInBoxBounds(a->walkdata.destbox, abr.x, abr.y))
            abr.box = a->walkdata.destbox;
        else
            abr = adjustXYToBeInBox(a, abr.x, abr.y);
        if (a->moving && a->walkdata.destdir == dir && a->walkdata.dest.x == abr.x &&
            a->walkdata.dest.y == abr.y)
            return;
    }

    if (a->x == abr.x && a->y == abr.y) {
        if (dir != a->facing)
            actor_turnToDirection(a, dir);
        return;
    }

    a->walkdata.dest.x = abr.x;
    a->walkdata.dest.y = abr.y;
    a->walkdata.destbox = abr.box;
    a->walkdata.destdir = (int16_t)dir;
    a->walkdata.point3.x = 32000;
    a->walkdata.curbox = a->walkbox;
    a->moving = (uint8_t)((a->moving & MF_IN_LEG) | MF_NEW_LEG);
}

static void walkActor(Actor *a)
{
    int new_dir, next_box;
    Point foundPath;

    if (!a->moving)
        return;

    if (!(a->moving & MF_NEW_LEG)) {
        if ((a->moving & MF_IN_LEG) && actorWalkStep(a))
            return;

        if (a->moving & MF_LAST_LEG) {
            a->moving = 0;
            setBox(a, a->walkdata.destbox);
            actor_startAnim(a, a->standFrame);
            if (a->targetFacing != a->walkdata.destdir)
                actor_turnToDirection(a, a->walkdata.destdir);
            return;
        }

        if (a->moving & MF_TURN) {
            new_dir = updateActorDirection(a, 0);
            if (a->facing != new_dir)
                actor_setDirection(a, new_dir);
            else
                a->moving = 0;
            return;
        }

        setBox(a, a->walkdata.curbox);
        a->moving &= MF_IN_LEG;
    }

    a->moving &= ~MF_NEW_LEG;
    for (;;) {
        if (a->walkbox == INVALID_BOX) {
            setBox(a, a->walkdata.destbox);
            a->walkdata.curbox = a->walkdata.destbox;
            break;
        }
        if (a->walkbox == a->walkdata.destbox)
            break;
        next_box = getNextBox(a->walkbox, a->walkdata.destbox);
        if (next_box < 0) {
            a->walkdata.destbox = a->walkbox;
            a->moving |= MF_LAST_LEG;
            return;
        }
        a->walkdata.curbox = (uint8_t)next_box;
        if (findPathTowards(a, a->walkbox, next_box, a->walkdata.destbox, &foundPath))
            break;
        if (calcMovementFactor(a, foundPath))
            return;
        setBox(a, a->walkdata.curbox);
    }

    a->moving |= MF_LAST_LEG;
    calcMovementFactor(a, a->walkdata.dest);
}

void walkActors(void)
{
    int i;
    for (i = 1; i < NUM_ACTORS; ++i)
        if (actor_inCurrentRoom(&g.actors[i]))
            walkActor(&g.actors[i]);
}

void actor_faceToObject(Actor *a, int obj)
{
    int x2, y2, dir, width = 0;
    if (!actor_inCurrentRoom(a))
        return;
    if (getObjectOrActorXY(obj, &x2, &y2) == -1)
        return;
    getObjectOrActorWidth(obj, &width);      /* v <= 4 */
    dir = (a->x < x2) ? 1 : 0;
    if (iabs(a->x - x2) < width / 2)
        dir = (a->y > y2) ? 3 : 2;
    actor_turnToDirection(a, oldDirToNewDir(dir));
}

/* --- placement and visibility ---------------------------------------------------------- */

void actor_hide(Actor *a)
{
    if (!a->visible)
        return;
    if (a->moving) {
        actor_stopMoving(a);
        actor_startAnim(a, a->standFrame);
    }
    a->visible = 0;
    a->cost.soundCounter = 0;
    a->needRedraw = 0;
    a->needBgReset = 1;
}

void actor_show(Actor *a)
{
    if (g.currentRoom == 0 || a->visible)
        return;
    adjustActorPos(a);
    if (a->costumeNeedsInit) {
        actor_startAnim(a, a->initFrame);
        a->costumeNeedsInit = 0;
    }
    actor_stopMoving(a);
    a->visible = 1;
    a->needRedraw = 1;
}

void showActors(void)
{
    int i;
    for (i = 1; i < NUM_ACTORS; i++)
        if (actor_inCurrentRoom(&g.actors[i]))
            actor_show(&g.actors[i]);
}

/* Actor::putActor(x, y, room) */
void actor_put(Actor *a, int x, int y, int room)
{
    if (a->visible && g.currentRoom != room && text_isTalking(a->number))
        stopTalk();
    a->x = (int16_t)x;
    a->y = (int16_t)y;
    a->room = (int16_t)room;
    a->needRedraw = 1;
    if (VAR(VAR_EGO) == a->number)
        g.egoPositioned = 1;
    if (a->visible) {
        if (actor_inCurrentRoom(a)) {
            if (a->moving) {
                actor_stopMoving(a);
                actor_startAnim(a, a->standFrame);
            }
            adjustActorPos(a);
        } else {
            actor_hide(a);
        }
    } else {
        if (actor_inCurrentRoom(a))
            actor_show(a);
    }
}

void actor_setCostume(Actor *a, int c)
{
    int i;
    a->costumeNeedsInit = 1;
    if (a->visible) {                        /* old costumes */
        actor_hide(a);
        a->costume = (int16_t)c;
        actor_show(a);
    } else {
        a->costume = (int16_t)c;
    }
    for (i = 0; i < 32; i++)
        a->palette[i] = 0xFF;
}

void actor_animate(Actor *a, int anim)
{
    int chore = anim >> 2, dir = oldDirToNewDir(anim & 3);
    chore = 0x3F - chore + 2;
    switch (chore) {
    case 2:                                  /* stop walking */
        if (actor_inCurrentRoom(a)) {
            actor_startAnim(a, a->standFrame);
            actor_stopMoving(a);
        }
        break;
    case 3:                                  /* change direction immediately */
        if (actor_inCurrentRoom(a))
            a->moving &= ~MF_TURN;
        actor_setDirection(a, dir);
        break;
    case 4:                                  /* turn to the new direction */
        if (actor_inCurrentRoom(a))
            actor_turnToDirection(a, dir);
        break;
    default:
        actor_startAnim(a, anim);
    }
}

/* --- camera (camera.cpp, v < 7) -------------------------------------------------------- */

enum { kNormalCameraMode = 1, kFollowActorCameraMode = 2, kPanningCameraMode = 3 };

static void cameraMoved(void)
{
    if (g.camera.cur_x < g.screenWidth / 2)
        g.camera.cur_x = g.screenWidth / 2;
    else if (g.camera.cur_x > g.roomWidth - g.screenWidth / 2)
        g.camera.cur_x = g.roomWidth - g.screenWidth / 2;
    g.screenStartStrip = g.camera.cur_x / 8 - 40 / 2;       /* _gdi->_numStrips = 40 */
    g.screenEndStrip = g.screenStartStrip + 40 - 1;
    g.mainXStart = g.screenStartStrip * 8;
}

void setCameraAt(int pos_x, int pos_y)
{
    (void)pos_y;
    if (g.camera.mode != kFollowActorCameraMode || iabs(pos_x - g.camera.cur_x) > g.screenWidth / 2)
        g.camera.cur_x = pos_x;
    g.camera.dest_x = pos_x;
    if (g.camera.cur_x < VAR(VAR_CAMERA_MIN_X))
        g.camera.cur_x = (int16_t)VAR(VAR_CAMERA_MIN_X);
    if (g.camera.cur_x > VAR(VAR_CAMERA_MAX_X))
        g.camera.cur_x = (int16_t)VAR(VAR_CAMERA_MAX_X);
    if (VAR(VAR_SCROLL_SCRIPT)) {
        VAR(VAR_CAMERA_POS_X) = g.camera.cur_x;
        runScript(VAR(VAR_SCROLL_SCRIPT), 0, 0, 0);
    }
    if (g.camera.cur_x != g.camera.last_x && text_hasMask())
        stopTalk();
}

void setCameraFollows(Actor *a, int setCamera)
{
    int t;
    g.camera.mode = kFollowActorCameraMode;
    g.camera.follows = a->number;
    if (!actor_inCurrentRoom(a)) {
        startScene(a->room);
        g.camera.mode = kFollowActorCameraMode;
        g.camera.cur_x = a->x;
        setCameraAt(g.camera.cur_x, 0);
    }
    t = a->x / 8 - g.screenStartStrip;
    if (t < g.camera.leftTrigger || t > g.camera.rightTrigger || setCamera)
        setCameraAt(a->x, 0);
    runInventoryScript(0);                   /* runInventoryScriptEx(0) */
}

void actorFollowCamera(int act)
{
    int old = g.camera.follows;
    setCameraFollows(derefActor(act), 0);
    if (g.camera.follows != old)
        runInventoryScript(0);
    g.camera.movingToActor = 0;
}

void moveCamera(void)
{
    int pos = g.camera.cur_x, t, snapToX = VAR(VAR_CAMERA_FAST_X) != 0;
    Actor *a = 0;

    g.camera.cur_x &= 0xFFF8;

    if (g.camera.cur_x < VAR(VAR_CAMERA_MIN_X)) {
        if (snapToX)
            g.camera.cur_x = (int16_t)VAR(VAR_CAMERA_MIN_X);
        else
            g.camera.cur_x += 8;
        cameraMoved();
        return;
    }
    if (g.camera.cur_x > VAR(VAR_CAMERA_MAX_X)) {
        if (snapToX)
            g.camera.cur_x = (int16_t)VAR(VAR_CAMERA_MAX_X);
        else
            g.camera.cur_x -= 8;
        cameraMoved();
        return;
    }

    if (g.camera.mode == kFollowActorCameraMode) {
        int actorx;
        a = derefActor(g.camera.follows);
        actorx = a->x;
        t = actorx / 8 - g.screenStartStrip;
        if (t < g.camera.leftTrigger || t > g.camera.rightTrigger) {
            if (snapToX) {
                if (t > 40 - 5)
                    g.camera.dest_x = actorx + 80;
                if (t < 5)
                    g.camera.dest_x = actorx - 80;
            } else {
                g.camera.movingToActor = 1;
            }
        }
    }

    if (g.camera.movingToActor) {
        a = derefActor(g.camera.follows);
        g.camera.dest_x = a->x;
    }

    if (g.camera.dest_x < VAR(VAR_CAMERA_MIN_X))
        g.camera.dest_x = (int16_t)VAR(VAR_CAMERA_MIN_X);
    if (g.camera.dest_x > VAR(VAR_CAMERA_MAX_X))
        g.camera.dest_x = (int16_t)VAR(VAR_CAMERA_MAX_X);

    if (snapToX) {
        g.camera.cur_x = g.camera.dest_x;
    } else {
        if (g.camera.cur_x < g.camera.dest_x)
            g.camera.cur_x += 8;
        if (g.camera.cur_x > g.camera.dest_x)
            g.camera.cur_x -= 8;
    }

    if (g.camera.movingToActor && (g.camera.cur_x / 8) == (a->x / 8))
        g.camera.movingToActor = 0;

    cameraMoved();

    if (VAR(VAR_SCROLL_SCRIPT) && pos != g.camera.cur_x) {
        VAR(VAR_CAMERA_POS_X) = g.camera.cur_x;
        runScript(VAR(VAR_SCROLL_SCRIPT), 0, 0, 0);
    }
}

/* a load's cameraMoved (saveload.c: loadState) */
void actor_cameraMoved(void)
{
    cameraMoved();
}
