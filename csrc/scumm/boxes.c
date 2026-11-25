/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * boxes.c - the walk boxes of the current room: their geometry, flags and scale, the box
 * matrix (which box leads to which), and the path through a box boundary.
 *
 * ScummVM boxes.cpp (the v4 = GF_SMALL_HEADER paths): getNumBoxes, getBoxBaseAddr,
 * getBoxFlags/setBoxFlags, getBoxScale/setBoxScale, getScale, getScaleFromSlot,
 * checkXYInBoxBounds, getBoxCoordinates, closestPtOnLine, getClosestPtOnBox, getNextBox,
 * Actor::findPathTowards, areBoxesNeighbors, calcItineraryMatrix, createBoxMatrix; room.cpp
 * resetRoomSubBlocks (the BOXD = "BX" and SCAL = "SA" parts).
 *
 * The scripts may change a box's flags and scale, so the boxes are copied to RAM at each room
 * change (rtMatrix 2); the matrix (rtMatrix 1) is read in place from the game image until a
 * script asks for it to be rebuilt (o5_matrixOps 4, after hiding or showing boxes).
 *
 * createBoxMatrix is ScummVM's, not the original's: ScummVM finds the routes with Kleene's
 * algorithm where the original used "some kind of mangled Dijkstra" that, ScummVM's comment
 * says, did not always find the shortest paths.  ScummVM is the oracle, so its routes are kept.
 */
#include <string.h>
#include "scumm.h"

#define SIZEOF_BOX      20          /* v4: 8 x int16 corners, mask, flags, uint16 scale */
#define MAX_BOXES       64          /* MI1: 61 at most (measured) */

static uint8_t boxes[1 + MAX_BOXES * SIZEOF_BOX];   /* rtMatrix 2: count, then the boxes */
static int haveBoxes;
static const uint8_t *matrix;                       /* rtMatrix 1, in place or matrixRam */
static uint32_t matrixSize;
#define BOX_MATRIX_SIZE 2000
static uint8_t matrixRam[BOX_MATRIX_SIZE];          /* rtMatrix 1 made by createBoxMatrix */

static struct { int x1, y1, scale1, x2, y2, scale2; } scaleSlots[20];

/* resetRoomSubBlocks, the box and scale part (GF_SMALL_HEADER) */
void boxes_resetRoom(const uint8_t *room)
{
    const uint8_t *ptr = res_find_data(room, "BX");
    int i;

    haveBoxes = 0;
    matrix = 0;
    matrixSize = 0;
    if (ptr) {
        uint8_t numOfBoxes = ptr[0];
        uint32_t size = (uint32_t)numOfBoxes * SIZEOF_BOX + 1;
        uint32_t blockData = rd32(ptr - 6) - 6;
        if (numOfBoxes > MAX_BOXES)
            plat_error("boxes: more than MAX_BOXES");
        memcpy(boxes, ptr, size);
        haveBoxes = 1;
        if (blockData > size) {
            matrix = ptr + size;
            matrixSize = blockData - size;
        }
    }

    /* scale slots: rtScaleTable has 5 entries, 1..4 read from SA, 8 bytes each */
    ptr = res_find_data(room, "SA");
    if (ptr) {
        for (i = 1; i < 5; i++, ptr += 8) {
            int s1 = rd16(ptr), y1 = rd16(ptr + 2), s2 = rd16(ptr + 4), y2 = rd16(ptr + 6);
            if (s1 || y1 || s2 || y2) {
                scaleSlots[i - 1].x1 = 0;  scaleSlots[i - 1].y1 = y1;  scaleSlots[i - 1].scale1 = s1;
                scaleSlots[i - 1].x2 = 0;  scaleSlots[i - 1].y2 = y2;  scaleSlots[i - 1].scale2 = s2;
            }
        }
    }
}

int getNumBoxes(void)
{
    return haveBoxes ? boxes[0] : 0;
}

static uint8_t *getBoxBaseAddr(int box)
{
    if (!haveBoxes || box == INVALID_BOX)
        return 0;
    if (boxes[0] == box)                     /* v4 and older: the count means the last box */
        box--;
    if (box < 0 || box > boxes[0] - 1) {
        plat_error("getBoxBaseAddr: box out of range");
        return 0;
    }
    return boxes + box * SIZEOF_BOX + 1;
}

/* getMaskFromBox: the z-plane an actor in this box is masked by (v3+: the old box's mask) */
int getMaskFromBox(int box)
{
    uint8_t *p = getBoxBaseAddr(box);
    return p ? p[16] : 0;
}

int getBoxFlags(int box)
{
    uint8_t *p = getBoxBaseAddr(box);
    return p ? p[17] : 0;
}

void setBoxFlags(int box, int val)
{
    uint8_t *p = getBoxBaseAddr(box);
    if (p)
        p[17] = (uint8_t)val;
}

int getBoxScale(int box)
{
    uint8_t *p = getBoxBaseAddr(box);
    return p ? rd16(p + 18) : 255;
}

void setBoxScale(int box, int scale)
{
    uint8_t *p = getBoxBaseAddr(box);
    if (p) {
        p[18] = (uint8_t)scale;
        p[19] = (uint8_t)(scale >> 8);
    }
}

/* setScaleSlot: o5_roomOps SO_ROOM_SCALE */
void setScaleSlot(int slot, int x1, int y1, int scale1, int x2, int y2, int scale2)
{
    if (slot < 1 || slot > 20)
        plat_error("setScaleSlot: slot out of range");
    scaleSlots[slot - 1].x2 = x2;
    scaleSlots[slot - 1].y2 = y2;
    scaleSlots[slot - 1].scale2 = scale2;
    scaleSlots[slot - 1].x1 = x1;
    scaleSlots[slot - 1].y1 = y1;
    scaleSlots[slot - 1].scale1 = scale1;
}

int getScaleFromSlot(int slot, int x, int y)
{
    int scale, scaleX = 0, scaleY = 0;
    const int x1 = scaleSlots[slot - 1].x1, y1 = scaleSlots[slot - 1].y1,
              x2 = scaleSlots[slot - 1].x2, y2 = scaleSlots[slot - 1].y2,
              s1 = scaleSlots[slot - 1].scale1, s2 = scaleSlots[slot - 1].scale2;

    if (y1 == y2 && x1 == x2)
        plat_error("Invalid scale slot");
    if (y1 != y2) {
        if (y < 0)
            y = 0;
        scaleY = (s2 - s1) * (y - y1) / (y2 - y1) + s1;
    }
    if (x1 == x2) {
        scale = scaleY;
    } else {
        scaleX = (s2 - s1) * (x - x1) / (x2 - x1) + s1;
        scale = (y1 == y2) ? scaleX : (scaleX + scaleY) / 2;
    }
    if (scale < 1)
        scale = 1;
    else if (scale > 255)
        scale = 255;
    return scale;
}

int getScale(int box, int x, int y)
{
    uint8_t *p = getBoxBaseAddr(box);
    int slot = 0, scale;
    if (!p)
        return 255;
    scale = rd16(p + 18);
    if (scale & 0x8000)
        slot = (scale & 0x7FFF) + 1;
    if (slot)
        scale = getScaleFromSlot(slot, x, y);
    return scale;
}

BoxCoords getBoxCoordinates(int boxnum)
{
    BoxCoords b;
    const uint8_t *p = getBoxBaseAddr(boxnum);
    if (!p) {
        plat_error("getBoxCoordinates: no box");
        memset(&b, 0, sizeof(b));
        return b;
    }
    b.ul.x = (int16_t)rd16(p + 0);  b.ul.y = (int16_t)rd16(p + 2);
    b.ur.x = (int16_t)rd16(p + 4);  b.ur.y = (int16_t)rd16(p + 6);
    b.lr.x = (int16_t)rd16(p + 8);  b.lr.y = (int16_t)rd16(p + 10);
    b.ll.x = (int16_t)rd16(p + 12); b.ll.y = (int16_t)rd16(p + 14);
    return b;
}

static int compareSlope(Point p1, Point p2, Point p3)
{
    return (p2.y - p1.y) * (p3.x - p1.x) <= (p3.y - p1.y) * (p2.x - p1.x);
}

int checkXYInBoxBounds(int boxnum, int x, int y)
{
    BoxCoords box;
    Point p;
    if (boxnum < 0 || boxnum == INVALID_BOX)
        return 0;
    box = getBoxCoordinates(boxnum);
    p.x = (int16_t)x;
    p.y = (int16_t)y;
    if (x < box.ul.x && x < box.ur.x && x < box.lr.x && x < box.ll.x) return 0;
    if (x > box.ul.x && x > box.ur.x && x > box.lr.x && x > box.ll.x) return 0;
    if (y < box.ul.y && y < box.ur.y && y < box.lr.y && y < box.ll.y) return 0;
    if (y > box.ul.y && y > box.ur.y && y > box.lr.y && y > box.ll.y) return 0;
    if (!compareSlope(box.ul, box.ur, p)) return 0;
    if (!compareSlope(box.ur, box.lr, p)) return 0;
    if (!compareSlope(box.lr, box.ll, p)) return 0;
    if (!compareSlope(box.ll, box.ul, p)) return 0;
    return 1;
}

static int iabs(int v) { return v < 0 ? -v : v; }

static Point closestPtOnLine(Point lineStart, Point lineEnd, Point p)
{
    Point result;
    const int lxdiff = lineEnd.x - lineStart.x;
    const int lydiff = lineEnd.y - lineStart.y;

    if (lineEnd.x == lineStart.x) {
        result.x = lineStart.x;
        result.y = p.y;
    } else if (lineEnd.y == lineStart.y) {
        result.x = p.x;
        result.y = lineStart.y;
    } else {
        const int dist = lxdiff * lxdiff + lydiff * lydiff;
        int a, b, c;
        if (iabs(lxdiff) > iabs(lydiff)) {
            a = lineStart.x * lydiff / lxdiff;
            b = p.x * lxdiff / lydiff;
            c = (a + b - lineStart.y + p.y) * lydiff * lxdiff / dist;
            result.x = (int16_t)c;
            result.y = (int16_t)(c * lydiff / lxdiff - a + lineStart.y);
        } else {
            a = lineStart.y * lxdiff / lydiff;
            b = p.y * lydiff / lxdiff;
            c = (a + b - lineStart.x + p.x) * lydiff * lxdiff / dist;
            result.x = (int16_t)(c * lxdiff / lydiff - a + lineStart.x);
            result.y = (int16_t)c;
        }
    }

    if (iabs(lydiff) < iabs(lxdiff)) {
        if (lxdiff > 0) {
            if (result.x < lineStart.x) result = lineStart;
            else if (result.x > lineEnd.x) result = lineEnd;
        } else {
            if (result.x > lineStart.x) result = lineStart;
            else if (result.x < lineEnd.x) result = lineEnd;
        }
    } else {
        if (lydiff > 0) {
            if (result.y < lineStart.y) result = lineStart;
            else if (result.y > lineEnd.y) result = lineEnd;
        } else {
            if (result.y > lineStart.y) result = lineStart;
            else if (result.y < lineEnd.y) result = lineEnd;
        }
    }
    return result;
}

/* Common::Point::sqrDist (common/rect.h): far points saturate at 0xFFFFFF */
static unsigned sqrDist(Point a, Point b)
{
    int dx = iabs(a.x - b.x), dy;
    if (dx >= 0x1000)
        return 0xFFFFFF;
    dy = iabs(a.y - b.y);
    if (dy >= 0x1000)
        return 0xFFFFFF;
    return (unsigned)(dx * dx + dy * dy);
}

int getClosestPtOnBox(BoxCoords box, int x, int y, int16_t *outX, int16_t *outY)
{
    Point p, tmp;
    unsigned dist, bestdist = 0xFFFFFF;
    p.x = (int16_t)x;
    p.y = (int16_t)y;

#define TRY(A, B) \
    tmp = closestPtOnLine(box.A, box.B, p); \
    dist = sqrDist(p, tmp); \
    if (dist < bestdist) { bestdist = dist; *outX = tmp.x; *outY = tmp.y; }
    TRY(ul, ur)
    TRY(ur, lr)
    TRY(lr, ll)
    TRY(ll, ul)
#undef TRY
    return (int)bestdist;
}

/* the next box on the way from 'from' to 'to', -1 if there is none */
int getNextBox(int from, int to)
{
    const uint8_t *boxm, *end;
    int i, dest = -1;

    if (from == to)
        return to;
    if (to == INVALID_BOX)
        return -1;
    if (from == INVALID_BOX)
        return to;
    if (!matrix)
        return -1;

    boxm = matrix;
    if (*boxm == 0xFF)                       /* getBoxMatrixBaseAddr */
        boxm++;
    end = boxm + matrixSize;                 /* WORKAROUND #1, as ScummVM: from the base */

    for (i = 0; i < from && boxm < end; i++) {
        while (boxm < end && *boxm != 0xFF)
            boxm += 3;
        boxm++;
    }
    while (boxm < end && boxm[0] != 0xFF) {
        if (boxm[0] <= to && to <= boxm[1])
            dest = (int8_t)boxm[2];
        boxm += 3;
    }
    return dest;
}

#define SWAP16(a, b) do { int16_t t_ = (a); (a) = (b); (b) = t_; } while (0)

/* Actor::findPathTowards: the point through which actor a crosses from box1 into box2 on its
 * way to box3; 1 when the straight line to the destination already passes. */
int findPathTowards(const Actor *a, int box1nr, int box2nr, int box3nr, Point *foundPath)
{
    BoxCoords box1 = getBoxCoordinates(box1nr);
    BoxCoords box2 = getBoxCoordinates(box2nr);
    Point tmp;
    int i, j, flag, q, pos;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            if (box1.ul.x == box1.ur.x && box1.ul.x == box2.ul.x && box1.ul.x == box2.ur.x) {
                flag = 0;
                if (box1.ul.y > box1.ur.y) { SWAP16(box1.ul.y, box1.ur.y); flag |= 1; }
                if (box2.ul.y > box2.ur.y) { SWAP16(box2.ul.y, box2.ur.y); flag |= 2; }

                if (box1.ul.y > box2.ur.y || box2.ul.y > box1.ur.y ||
                    ((box1.ur.y == box2.ul.y || box2.ur.y == box1.ul.y) &&
                     box1.ul.y != box1.ur.y && box2.ul.y != box2.ur.y)) {
                    if (flag & 1) SWAP16(box1.ul.y, box1.ur.y);
                    if (flag & 2) SWAP16(box2.ul.y, box2.ur.y);
                } else {
                    pos = a->y;
                    if (box2nr == box3nr) {
                        int diffX = a->walkdata.dest.x - a->x;
                        int diffY = a->walkdata.dest.y - a->y;
                        int boxDiffX = box1.ul.x - a->x;
                        if (diffX != 0) {
                            int t;
                            diffY *= boxDiffX;
                            t = diffY / diffX;
                            if (t == 0 && (diffY <= 0 || diffX <= 0) && (diffY >= 0 || diffX >= 0))
                                t = -1;
                            pos = a->y + t;
                        }
                    }
                    q = pos;
                    if (q < box2.ul.y) q = box2.ul.y;
                    if (q > box2.ur.y) q = box2.ur.y;
                    if (q < box1.ul.y) q = box1.ul.y;
                    if (q > box1.ur.y) q = box1.ur.y;
                    if (q == pos && box2nr == box3nr)
                        return 1;
                    foundPath->y = (int16_t)q;
                    foundPath->x = box1.ul.x;
                    return 0;
                }
            }

            if (box1.ul.y == box1.ur.y && box1.ul.y == box2.ul.y && box1.ul.y == box2.ur.y) {
                flag = 0;
                if (box1.ul.x > box1.ur.x) { SWAP16(box1.ul.x, box1.ur.x); flag |= 1; }
                if (box2.ul.x > box2.ur.x) { SWAP16(box2.ul.x, box2.ur.x); flag |= 2; }

                if (box1.ul.x > box2.ur.x || box2.ul.x > box1.ur.x ||
                    ((box1.ur.x == box2.ul.x || box2.ur.x == box1.ul.x) &&
                     box1.ul.x != box1.ur.x && box2.ul.x != box2.ur.x)) {
                    if (flag & 1) SWAP16(box1.ul.x, box1.ur.x);
                    if (flag & 2) SWAP16(box2.ul.x, box2.ur.x);
                } else {
                    if (box2nr == box3nr) {
                        int diffX = a->walkdata.dest.x - a->x;
                        int diffY = a->walkdata.dest.y - a->y;
                        int boxDiffY = box1.ul.y - a->y;
                        pos = a->x;
                        if (diffY != 0)
                            pos += diffX * boxDiffY / diffY;
                    } else {
                        pos = a->x;
                    }
                    q = pos;
                    if (q < box2.ul.x) q = box2.ul.x;
                    if (q > box2.ur.x) q = box2.ur.x;
                    if (q < box1.ul.x) q = box1.ul.x;
                    if (q > box1.ur.x) q = box1.ur.x;
                    if (q == pos && box2nr == box3nr)
                        return 1;
                    foundPath->x = (int16_t)q;
                    foundPath->y = box1.ul.y;
                    return 0;
                }
            }
            tmp = box1.ul;
            box1.ul = box1.ur;
            box1.ur = box1.lr;
            box1.lr = box1.ll;
            box1.ll = tmp;
        }
        tmp = box2.ul;
        box2.ul = box2.ur;
        box2.ur = box2.lr;
        box2.lr = box2.ll;
        box2.ll = tmp;
    }
    return 0;
}

/* areBoxesNeighbors: do two sides of the boxes touch?  Only the "upper" sides are matched, the
 * coordinates rotated four times each (16 comparisons). */
static int areBoxesNeighbors(int box1nr, int box2nr)
{
    BoxCoords box, box2;
    Point tmp;
    int j, k;

    if ((getBoxFlags(box1nr) & kBoxInvisible) || (getBoxFlags(box2nr) & kBoxInvisible))
        return 0;

    box2 = getBoxCoordinates(box1nr);
    box = getBoxCoordinates(box2nr);

    for (j = 0; j < 4; j++) {
        for (k = 0; k < 4; k++) {
            /* the upper sides on one vertical line? */
            if (box2.ur.x == box2.ul.x && box.ul.x == box2.ul.x && box.ur.x == box2.ul.x) {
                int swappedBox2 = 0, swappedBox1 = 0;
                if (box2.ur.y < box2.ul.y) {
                    swappedBox2 = 1;
                    SWAP16(box2.ur.y, box2.ul.y);
                }
                if (box.ur.y < box.ul.y) {
                    swappedBox1 = 1;
                    SWAP16(box.ur.y, box.ul.y);
                }
                if (box.ur.y < box2.ul.y ||
                        box.ul.y > box2.ur.y ||
                        ((box.ul.y == box2.ur.y ||
                          box.ur.y == box2.ul.y) && box2.ur.y != box2.ul.y && box.ul.y != box.ur.y)) {
                } else {
                    return 1;
                }
                if (swappedBox2)
                    SWAP16(box2.ur.y, box2.ul.y);
                if (swappedBox1)
                    SWAP16(box.ur.y, box.ul.y);
            }

            /* the upper sides on one horizontal line? */
            if (box2.ur.y == box2.ul.y && box.ul.y == box2.ul.y && box.ur.y == box2.ul.y) {
                int swappedBox2 = 0, swappedBox1 = 0;
                if (box2.ur.x < box2.ul.x) {
                    swappedBox2 = 1;
                    SWAP16(box2.ur.x, box2.ul.x);
                }
                if (box.ur.x < box.ul.x) {
                    swappedBox1 = 1;
                    SWAP16(box.ur.x, box.ul.x);
                }
                if (box.ur.x < box2.ul.x ||
                        box.ul.x > box2.ur.x ||
                        ((box.ul.x == box2.ur.x ||
                          box.ur.x == box2.ul.x) && box2.ur.x != box2.ul.x && box.ul.x != box.ur.x)) {
                } else {
                    return 1;
                }
                if (swappedBox2)
                    SWAP16(box2.ur.x, box2.ul.x);
                if (swappedBox1)
                    SWAP16(box.ur.x, box.ul.x);
            }

            tmp = box2.ul;                  /* "rotate" box2 */
            box2.ul = box2.ur;
            box2.ur = box2.lr;
            box2.lr = box2.ll;
            box2.ll = tmp;
        }
        tmp = box.ul;                       /* "rotate" box */
        box.ul = box.ur;
        box.ur = box.lr;
        box.lr = box.ll;
        box.ll = tmp;
    }
    return 0;
}

/* calcItineraryMatrix: the shortest routes between the boxes (Kleene's algorithm); entry
 * [i][j] is the box to go to first on the way from i to j.  Rows of 64, as ScummVM. */
static uint8_t adjacentMatrix[MAX_BOXES * MAX_BOXES];
static uint8_t itineraryMatrix[MAX_BOXES * MAX_BOXES];

static void calcItineraryMatrix(int num)
{
    int i, j, k;

    for (i = 0; i < num; i++) {
        for (j = 0; j < num; j++) {
            if (i == j) {
                adjacentMatrix[i * MAX_BOXES + j] = 0;
                itineraryMatrix[i * MAX_BOXES + j] = (uint8_t)j;
            } else if (areBoxesNeighbors(i, j)) {
                adjacentMatrix[i * MAX_BOXES + j] = 1;
                itineraryMatrix[i * MAX_BOXES + j] = (uint8_t)j;
            } else {
                adjacentMatrix[i * MAX_BOXES + j] = 255;
                itineraryMatrix[i * MAX_BOXES + j] = INVALID_BOX;
            }
        }
    }
    for (k = 0; k < num; k++) {
        for (i = 0; i < num; i++) {
            for (j = 0; j < num; j++) {
                int distIK, distKJ;
                if (i == j)
                    continue;
                distIK = adjacentMatrix[MAX_BOXES * i + k];
                distKJ = adjacentMatrix[MAX_BOXES * k + j];
                if (adjacentMatrix[MAX_BOXES * i + j] > distIK + distKJ) {
                    adjacentMatrix[MAX_BOXES * i + j] = (uint8_t)(distIK + distKJ);
                    itineraryMatrix[MAX_BOXES * i + j] = itineraryMatrix[MAX_BOXES * i + k];
                }
            }
        }
    }
}

/* createBoxMatrix: the itinerary matrix compressed into the engine's format.  Per box a 0xFF,
 * then triples (first, last, via): to reach any box first..last, go to box via first. */
void createBoxMatrix(void)
{
    int num = getNumBoxes(), i, j;
    uint8_t *p = matrixRam;

    calcItineraryMatrix(num);
    memset(matrixRam, 0, sizeof matrixRam);    /* createResource: a fresh, zeroed block */
    for (i = 0; i < num; i++) {
        *p++ = 0xFF;
        for (j = 0; j < num; j++) {
            uint8_t itinerary = itineraryMatrix[MAX_BOXES * i + j];
            if (itinerary != INVALID_BOX) {
                *p++ = (uint8_t)j;
                while (j < num - 1 && itinerary == itineraryMatrix[MAX_BOXES * i + (j + 1)])
                    j++;
                *p++ = (uint8_t)j;
                *p++ = itinerary;
            }
            if (p > matrixRam + BOX_MATRIX_SIZE - 4)
                plat_error("createBoxMatrix: BOX_MATRIX_SIZE");
        }
    }
    *p++ = 0xFF;
    matrix = matrixRam;
    matrixSize = BOX_MATRIX_SIZE;
}

/* --- save and load: the boxes as the scripts left them (rtMatrix 2), the matrix made from
 * them (rtMatrix 1), the scale slots ------------------------------------------------------ */
void boxes_saveLoad(Ser *s)
{
    int i;
    ser_bytes(s, boxes, sizeof(boxes));
    SER(s, haveBoxes);
    SER(s, matrixSize);
    ser_bytes(s, matrixRam, sizeof(matrixRam));
    {   /* which matrix: none, createBoxMatrix's, or the room's own (in the image) */
        uint8_t kind = !matrix ? 0 : matrix == matrixRam ? 1 : 2;
        const uint8_t *p = kind == 2 ? matrix : 0;
        SER(s, kind);
        ser_imgptr(s, &p);
        if (s->loading)
            matrix = kind == 1 ? matrixRam : kind == 2 ? p : 0;
    }
    for (i = 0; i < 20; i++) {
        SER(s, scaleSlots[i].x1); SER(s, scaleSlots[i].y1); SER(s, scaleSlots[i].scale1);
        SER(s, scaleSlots[i].x2); SER(s, scaleSlots[i].y2); SER(s, scaleSlots[i].scale2);
    }
}
