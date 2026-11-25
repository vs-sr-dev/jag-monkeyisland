/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * JaguarMI - a SCUMM v4 interpreter for The Secret of Monkey Island (DOS VGA floppy).
 *
 * Written against ScummVM's SCUMM engine (engines/scumm/, GPL v2+, commit f0e3f05 of
 * 2026-11-24): every function carries the name of the ScummVM function it transcribes, so the
 * two can be read side by side.  Only the v4 path is kept, and only the ORIGINAL behaviour:
 * ScummVM's workarounds and "enhancements" are left out unless the original interpreter did the
 * same (the copy-protection skip of the later releases is the one kept, see o_startScript).
 *
 * Portable C99: no dynamic allocation, all state in one struct, resources read in place from
 * the game image the platform provides (the PC loads the user's files; the Jaguar reads its
 * cartridge).  This file is GPL v2+ like the code it derives from.
 */
#ifndef SCUMM_H
#define SCUMM_H

#include <stdint.h>

/* --- limits (ScummVM script.h, scumm.h, resource_v4.cpp, readMAXS defaults) -------- */
#define NUM_SCRIPT_SLOT     80
#define NUM_SCRIPT_LOCAL    25
#define MAX_NESTING         15
#define NUM_SENTENCE        6
#define MAX_CUTSCENE        5
#define NUM_VARIABLES       800
#define NUM_BITVARIABLES    2048
#define NUM_LOCAL_OBJECTS   200
#define NUM_GLOBAL_SCRIPTS  200
#define NUM_LOCAL_SCRIPTS   60
#define NUM_ACTORS          13
#define NUM_VERBS           100
#define NUM_INVENTORY       80
#define NUM_GLOBAL_OBJECTS  1000
#define NUM_ROOMS           100
#define NUM_STRINGS         50          /* _numArray: string resources */
#define STRING_MAX          256
#define NUM_CHARSETS        9

#define SCRIPT_NUM_EXCD     10001
#define SCRIPT_NUM_ENCD     10002

enum { SS_DEAD = 0, SS_PAUSED = 1, SS_RUNNING = 2 };
enum { WIO_INVENTORY = 0, WIO_ROOM = 1, WIO_GLOBAL = 2, WIO_LOCAL = 3, WIO_FLOBJECT = 4,
       WIO_NOT_FOUND = -1 };

#define OF_OWNER_MASK   0x0F
#define OF_STATE_SHL    4
#define OF_OWNER_ROOM   0x0F

/* --- variables (ScummVM vars.cpp: ScummEngine::setupScummVars, v4 and v5 parts) ----- */
enum {
    VAR_KEYPRESS = 0, VAR_EGO = 1, VAR_CAMERA_POS_X = 2, VAR_HAVE_MSG = 3, VAR_ROOM = 4,
    VAR_OVERRIDE = 5, VAR_MACHINE_SPEED = 6, VAR_ME = 7, VAR_NUM_ACTOR = 8,
    VAR_CURRENT_LIGHTS = 9, VAR_CURRENTDRIVE = 10, VAR_TMR_1 = 11, VAR_TMR_2 = 12,
    VAR_TMR_3 = 13, VAR_MUSIC_TIMER = 14, VAR_ACTOR_RANGE_MIN = 15, VAR_ACTOR_RANGE_MAX = 16,
    VAR_CAMERA_MIN_X = 17, VAR_CAMERA_MAX_X = 18, VAR_TIMER_NEXT = 19, VAR_VIRT_MOUSE_X = 20,
    VAR_VIRT_MOUSE_Y = 21, VAR_ROOM_RESOURCE = 22, VAR_LAST_SOUND = 23,
    VAR_CUTSCENEEXIT_KEY = 24, VAR_TALK_ACTOR = 25, VAR_CAMERA_FAST_X = 26,
    VAR_SCROLL_SCRIPT = 27, VAR_ENTRY_SCRIPT = 28, VAR_ENTRY_SCRIPT2 = 29,
    VAR_EXIT_SCRIPT = 30, VAR_EXIT_SCRIPT2 = 31, VAR_VERB_SCRIPT = 32,
    VAR_SENTENCE_SCRIPT = 33, VAR_INVENTORY_SCRIPT = 34, VAR_CUTSCENE_START_SCRIPT = 35,
    VAR_CUTSCENE_END_SCRIPT = 36, VAR_CHARINC = 37, VAR_WALKTO_OBJ = 38, VAR_DEBUGMODE = 39,
    VAR_HEAPSPACE = 40, VAR_RESTART_KEY = 42, VAR_PAUSE_KEY = 43, VAR_MOUSE_X = 44,
    VAR_MOUSE_Y = 45, VAR_TIMER = 46, VAR_TIMER_TOTAL = 47, VAR_SOUNDCARD = 48,
    VAR_VIDEOMODE = 49, VAR_MAINMENU_KEY = 50, VAR_FIXEDDISK = 51, VAR_CURSORSTATE = 52,
    VAR_USERPUT = 53, VAR_V5_TALK_STRING_Y = 54
};

/* --- script VM (ScummVM script.h) --------------------------------------------------- */
typedef struct {
    uint32_t offs;
    int32_t  delay;
    uint16_t number;
    uint16_t delayFrameCount;
    uint8_t  status, where, freezeResistant, recursive, freezeCount, didexec,
             cutsceneOverride, cycle;
} ScriptSlot;

typedef struct {
    uint16_t number;
    uint8_t  where, slot;
} NestedScript;

typedef struct {
    uint8_t verb, preposition, freezeCount;
    uint16_t objectA, objectB;
} SentenceTab;

/* --- actors (ScummVM actor.h: Actor, the v4 = base-class fields) ------------------- */
typedef struct { int16_t x, y; } Point;
typedef struct { Point ul, ur, lr, ll; } BoxCoords;

#define INVALID_BOX     255         /* kOldInvalidBox: Actor::kInvalidBox for GF_SMALL_HEADER */
enum { MF_NEW_LEG = 1, MF_IN_LEG = 2, MF_TURN = 4, MF_LAST_LEG = 8, MF_FROZEN = 0x80 };
enum { kBoxXFlip = 0x08, kBoxYFlip = 0x10, kBoxPlayerOnly = 0x20, kBoxLocked = 0x40,
       kBoxInvisible = 0x80 };

typedef struct {
    Point   dest;
    uint8_t destbox;
    int16_t destdir;
    Point   cur;
    uint8_t curbox;
    Point   next;
    Point   point3;
    int32_t deltaXFactor, deltaYFactor;
    uint16_t xfrac, yfrac;
} ActorWalkData;

/* ScummVM CostumeData (the classic costume's part) */
typedef struct {
    uint16_t animCounter;
    uint8_t  soundCounter;
    uint16_t stopped;
    uint16_t curpos[16], start[16], end[16], frame[16];
} CostumeData;

typedef struct {
    int16_t x, y;                 /* _pos */
    int16_t room;
    int16_t costume;
    int16_t elevation;
    int16_t facing, targetFacing;
    uint8_t number;
    uint8_t visible, moving, ignoreBoxes, forceClip, shadowMode, ignoreTurns;
    uint8_t costumeNeedsInit, needRedraw, needBgReset;
    uint8_t initFrame, walkFrame, standFrame, talkStartFrame, talkStopFrame, frame;
    uint8_t talkColor, width, scalex, scaley, animSpeed, animProgress;
    uint16_t boxscale;            /* a scale, or 0x8000 | scale slot */
    uint8_t walkbox;
    uint8_t speedx, speedy;
    int16_t lastValidX, lastValidY;
    int16_t talkPosX, talkPosY;
    uint8_t walkScript, talkScript;
    uint8_t sound;
    ActorWalkData walkdata;
    uint8_t palette[32];
    char    name[64];             /* MI1: 33 bytes at most (Italian, room 28) */
    int32_t top, bottom;          /* the drawn extent (drawActorCostume); 0x7FFFFFFF: nothing */
    CostumeData cost;
} Actor;

/* --- objects in the current room (ScummVM ObjectData, the v4 fields) --------------- */
typedef struct {
    uint32_t OBIMoffset, OBCDoffset;  /* from the room block */
    int16_t  walk_x, walk_y;
    uint16_t obj_nr;
    int16_t  x_pos, y_pos;
    uint16_t width, height;
    uint8_t  actordir, parent, parentstate, state, fl_object_index, flags;
} ObjectData;

/* ScummVM StringSlot: a text slot (0 talk, 1 text, 2 debug, 3 banner), with its default */
typedef struct {
    int16_t xpos, ypos, right, height;
    uint8_t color, charset, center, overhead, no_talk_anim;
} TextSlot;

typedef struct {
    int16_t x, y, right, bottom;         /* curRect: x, y set by the script, right/bottom by drawVerb */
    int16_t oldLeft, oldTop, oldRight, oldBottom;   /* oldRect: what drawVerb drew; left -1: none */
    uint8_t color, hicolor, dimcolor, bkcolor, type, charset, curmode, saveid, key, center,
            prep, hasName;               /* hasName: the rtVerb resource exists */
    uint16_t verbid, imgindex;
    uint8_t  name[STRING_MAX];   /* MI1: ~111 bytes (dialogue choices); was 64: cut them */
} VerbSlot;

/* ScummVM ColorCycle (gfx.h) */
typedef struct {
    uint16_t delay, counter, flags;
    uint8_t  start, end;
} ColorCycle;

/* --- the game image (provided by the platform) --------------------------------------- */
typedef struct {
    const uint8_t *disk[5];          /* DISKnn.LEC, decrypted; [1]..[4] */
    uint32_t disk_size[5];
    const uint8_t *charset[NUM_CHARSETS];   /* 90x.LFL, NULL if absent */
    uint8_t  room_disk[NUM_ROOMS];   /* index 0R */
    uint32_t room_lf[NUM_ROOMS];     /* offset of the room's LF block in its disk, 0 = none */
    uint8_t  script_room[NUM_GLOBAL_SCRIPTS];
    uint32_t script_offs[NUM_GLOBAL_SCRIPTS];
    uint8_t  sound_room[200];
    uint32_t sound_offs[200];
    uint8_t  costume_room[200];
    uint32_t costume_offs[200];
    const uint8_t *objects;          /* index 0O: NUM_GLOBAL_OBJECTS x {class 24 bit, owner|state} */
} GameImage;

/* --- the whole interpreter ----------------------------------------------------------- */
typedef struct {
    const GameImage *img;

    /* VM */
    ScriptSlot   slot[NUM_SCRIPT_SLOT];
    NestedScript nest[MAX_NESTING];
    int32_t      localvar[NUM_SCRIPT_SLOT][NUM_SCRIPT_LOCAL + 1];
    uint8_t      numNestedScripts;
    uint8_t      currentScript;
    uint8_t      opcode;
    const uint8_t *scriptOrgPointer, *scriptPointer;
    int32_t      resultVarNumber;
    int32_t      vmStack[150];
    int          stackPos;
    uint32_t     cutScenePtr[MAX_CUTSCENE];
    uint8_t      cutSceneScript[MAX_CUTSCENE];
    int16_t      cutSceneData[MAX_CUTSCENE];
    uint8_t      cutSceneScriptIndex, cutSceneStackPointer;
    SentenceTab  sentence[NUM_SENTENCE];
    uint8_t      sentenceNum;

    int32_t  vars[NUM_VARIABLES];
    uint8_t  bitVars[NUM_BITVARIABLES / 8];

    /* objects */
    uint32_t classData[NUM_GLOBAL_OBJECTS];
    uint8_t  objectOwnerTable[NUM_GLOBAL_OBJECTS];
    uint8_t  objectStateTable[NUM_GLOBAL_OBJECTS];
    ObjectData objs[NUM_LOCAL_OBJECTS];
    int      numObjectsInRoom;
    uint16_t inventory[NUM_INVENTORY];
    const uint8_t *inventoryPtr[NUM_INVENTORY];   /* rtInventory: the OC block, in place */

    /* room */
    int      currentRoom, roomResource;
    uint8_t  resourceMapper[128];    /* o5_pseudoRoom: room numbers >= 0x80 */
    const uint8_t *roomPtr;          /* the RO block, header included */
    const uint8_t *roomImage;        /* the room's BM data (_IM00_offs); 0: no picture */
    int      numZBuffer;             /* _gdi->_numZBuffer: the image and its z-planes */
    int      roomWidth, roomHeight;
    uint32_t localScriptOffsets[NUM_LOCAL_SCRIPTS];
    uint32_t ENCD_offs, EXCD_offs;
    struct { int cur_x, cur_y, dest_x, dest_y, mode, follows, last_x, movingToActor,
             leftTrigger, rightTrigger; } camera;
    int      screenStartStrip, screenEndStrip, mainXStart;   /* cameraMoved */
    int      screenWidth, screenHeight;
    uint8_t  roomPalette[256];
    uint32_t roomPaletteGen;     /* + 1 at every write of roomPalette: the platform's tables */

    /* actors, strings, verbs, cursor */
    Actor    actors[NUM_ACTORS];
    uint8_t  strings[NUM_STRINGS][STRING_MAX];
    TextSlot string[6], stringDefault[6];
    int      actorToPrintStrFor;
    VerbSlot verbs[NUM_VERBS];
    struct { int state, animate, animateIndex, width, height, hotspotX, hotspotY; } cursor;
    uint16_t cursorImages[4][16];        /* _cursorImages: one bit a pixel, bit 15 leftmost */
    uint8_t  cursorHotspots[8];
    uint8_t  grabbedCursor[16 * 16];     /* the image, 0xFF transparent */
    int      userPut, currentCursor;
    int      haveMsg, talkDelay;
    int      egoPositioned;
    int      verbMouseOver;              /* the verb drawn highlighted */
    uint8_t  charsetColorMap[16];        /* _charsetColorMap: [1] is the text colour */
    uint8_t  charsetData[NUM_CHARSETS][16];   /* the colour maps SO_CHARSET_COLOR sets */
    int      newEffect, switchRoomEffect, switchRoomEffect2;
    int      doEffect;                   /* startScene: fadeIn(newEffect) at the next handleEffects */
    int      screenEffectFlag;           /* a fadeIn since the last fadeOut */
    int      fullRedraw;

    /* palette (palette.c) */
    uint8_t  currentPalette[768];
    uint8_t  shadowPalette[256];
    int      palDirtyMin, palDirtyMax;
    int      curPalIndex;
    ColorCycle colorCycle[16];
    uint8_t  palManipPalette[768];
    uint16_t palManipIntermediatePal[768];
    int      palManipStart, palManipEnd, palManipCounter;

    /* set by the platform before scumm_init, kept by it */
    int      trace;                  /* 1: print ScummVM's DEBUG_OPCODES/DEBUG_SCRIPTS lines */
    int      soundcard;              /* VAR_SOUNDCARD: 3 AdLib (the port), 0 none (an oracle run) */
    int      debugMode;              /* VAR_DEBUGMODE: 0 (the port); 1 matches a ScummVM run with a debug level */
    int      bootParam;              /* runBootscript's args[0] (ScummVM --boot-param) */
    uint32_t randSeed;               /* Common::RandomSource; a ScummVM run with --random-seed=N starts at N */
    int      copyProtection;         /* 1: the Dial-A-Pirate wheel (script 152) runs (the port);
                                        0: skipped, as ScummVM's copy_protection=false (the oracle's) */
    int      quit;                   /* o5_systemOps SO_QUIT: the platform ends the game after the round */
    int      forceBannerVirtScreen;  /* findVirtScreen answers the banner's (the banner drawing) */
    int      postGUICharMask;        /* after a banner: the next restoreCharsetBg clears the text */
    int      drawingObject;          /* the object drawObject is drawing (a FATAL's message) */
    int      saveLoadFlag;           /* _saveLoadFlag: 1 save, 2 load, at this round's handleSaveLoad */
    int      completeScreenRedraw;   /* _completeScreenRedraw: a load sets it */
    int      halted;                 /* set by an unimplemented opcode */
    char     haltMsg[96];
} Scumm;

extern Scumm g;

/* --- platform: tracing and fatal errors ---------------------------------------------- */
void plat_trace(const char *line);
/* SCUMM_PROF (the Jaguar build): plat_prof(n) marks the start of section n of scummLoop;
 * the platform sums the time between marks.  Nothing at all elsewhere. */
#ifdef SCUMM_PROF
void plat_prof(int section);
#define PROF(n) plat_prof(n)
#else
#define PROF(n) ((void)0)
#endif
void plat_error(const char *msg);
void plat_drawBox(int x, int y, int x2, int y2, int color);   /* drawBox: the fill */
void plat_room(void);             /* startScene: g.roomPtr is the new room (0: none) */
void plat_setPalette(int first, int num, const uint8_t *rgb);  /* updatePalette: 8-bit RGB */

/* --- platform: the picture.  The ScummVM functions that touch pixels, called where ScummVM
 * calls them with ScummVM's arguments.  The PC's are ScummVM's own (csrc/ref/, the reference
 * renderer); the Jaguar's are the GPU, Blitter and OP, checked against the PC's. */
enum { DB_ALLOW_MASK_OR = 1, DB_DRAW_MASK_ON_ALL = 2, DB_OBJECT_MODE = 8 };  /* Gdi::DrawBitmapFlags */
#define USAGE_BIT_DIRTY     96                            /* gfx usage bits (usage_bits.h) */
#define USAGE_BIT_RESTORED  95
void plat_initScreens(int b, int h);                      /* initScreens */
void plat_initBGBuffers(int height);                      /* initBGBuffers: g.numZBuffer planes */
void plat_drawBitmap(const uint8_t *ptr, int x, int y, int width, int height,
                     int stripnr, int numstrip, int flag); /* Gdi::drawBitmap, main screen */
void plat_resetBackground(int top, int bottom, int strip);  /* Gdi::resetBackground */
void plat_markRectAsDirty(int virt, int left, int right, int top, int bottom); /* clipped */
void plat_drawDirtyScreenParts(void);
/* a costume cel: ScummVM's ByleRLEData at byleRLEDecode, and the renderer's state */
typedef struct {
    const uint8_t *src;          /* the cel's RLE data (after its 12-byte CostumeInfo) */
    int skipLines;               /* skipCelLines(num) first; 0: none */
    int x, y;                    /* compData.x, y: the first column drawn, the top line */
    int width, height;           /* the cel's, unscaled */
    int skipWidth;               /* the columns to decode */
    int scaleX, scaleY;          /* 255: not scaled */
    int scaleXIndex, scaleYIndex, scaleXStep;
    int colorMask, colorShr;     /* 16 colours: 15, 4; 32: 7, 3 */
    int boundsRight, boundsBottom;   /* boundsRect (left and top are 0) */
    int zbuf;                    /* the z-plane masking the actor */
    int actor;
    uint8_t palette[32];         /* the renderer's _palette (setPalette) */
} CelDraw;
extern const uint8_t smallCostumeScaleTable[256];
void plat_drawCel(const CelDraw *c);                     /* skipCelLines + byleRLEDecode */
/* restoreBackground's fill or copy: virt, the rectangle in its own coordinates, clipped */
void plat_restoreBackground(int virt, int left, int top, int right, int bottom, int backColor);
void plat_restoreCharsetBg(int virt);                     /* restoreCharsetBg's pixels */
/* CharsetRendererClassic::printCharIntern + drawBitsN: one glyph (after its 4-byte header),
 * bpp bits a pixel, through the colour map.  toText: into the text surface at screen (x, y);
 * else into virtual screen virt at (x, y) of its own (xstart applies to the main screen).
 * drawTop: the y the clipping counts from (ScummVM's drawTop). */
void plat_drawChar(int virt, int toText, int x, int y, int drawTop, const uint8_t *glyph,
                   int bpp, int width, int height, const uint8_t *cmap);
void plat_setCursor(const uint8_t *img, int w, int h, int hotX, int hotY); /* 0xFF: clear */
void plat_showMouse(int show);                            /* CursorMan.showMouse */
void plat_fadeIn(int effect);                             /* fadeIn's switch, the drawing */
void plat_fadeOut(int effect, int run);                   /* fadeOut: run = the effect runs */
/* dissolveEffect(width, height): the blocks of the main screen copied to the screen in the
 * order the logic drew from the game's random source: offsets[n] (y * 320 + x of each block,
 * pitch 320), width x height each.  The buffer: the platform's (plat_dissolveBuffer). */
uint16_t *plat_dissolveBuffer(int n);
void plat_dissolve(int width, int height, const uint16_t *offsets, int n);
#define VIRT_BANNER 3
/* The message banner (showOldStyleBannerAndPause, text.c), its pixels and its wait.  The
 * virtual screens' numbers: 0 main, 1 text, 2 verbs, 3 the banner (80, 320 x 12). */
void plat_saveSurfacesPreGUI(void);       /* the text, main and verb surfaces kept; the text
                                             stamped onto the main screen */
void plat_restoreSurfacesPostGUI(void);   /* them back, main and verbs dirty in full */
void plat_fillVirtScreen(int virt, int color);           /* memset of its w x h */
void plat_drawPixel(int virt, int x, int y, int color);  /* at (x, y) of its own */
void plat_setDirtyRange(int virt, int top, int bottom);
void plat_updateDirtyScreen(int virt);
/* drawDirtyScreenParts proper (the three screens into the system screen), without what
 * plat_drawDirtyScreenParts adds at a round's end (the Jaguar's hand-over, the checks) */
void plat_drawDirtyScreenPartsOnly(void);
void plat_pauseEngine(int pause);         /* pauseEngine: the sound stops where it is */
/* waitForBannerInput(-1): the screen shown (waitForTimer), then the first key or click; the
 * key's ASCII, 0 for a click (ScummVM's KEYCODE_INVALID) */
int  plat_waitBannerInput(void);
void plat_clearTextSurface(void);         /* clearTextSurface: all CHARSET_MASK_TRANSPARENCY */
/* scummLoop_handleSaveLoad: g.saveLoadFlag 1 save, 2 load; the platform keeps the bytes
 * (scumm_save / scumm_load).  Returns 1 on success. */
int  plat_saveLoad(int flag);

/* --- saveload.c: the game's state (ScummVM saveLoadWithSerializer, its v4 part) -------
 * Little-endian fields of fixed width, the same on the PC and the Jaguar; pointers into the
 * game image as (disk, offset).  Taken at scummLoop's handleSaveLoad, between the music
 * timer and runAllScripts, as ScummVM does. */
typedef struct {
    uint8_t *buf;
    uint32_t pos, max;
    int      loading, err;
} Ser;
void ser_int(Ser *s, void *p, int size, int isSigned);
void ser_bytes(Ser *s, void *p, uint32_t n);
void ser_imgptr(Ser *s, const uint8_t **p);
#define SER(s, x) ser_int((s), &(x), (int)sizeof(x), ((__typeof__(x))-1) < 0)
#define SER_ARR(s, a) do { unsigned i_; for (i_ = 0; i_ < sizeof(a) / sizeof((a)[0]); i_++) SER(s, (a)[i_]); } while (0)
/* The file: "JMIZ", LE32 the state's size, then the state with each run of zero bytes as
 * 0, n (n 1..255).  raw: a work buffer for the state itself (~86 KB). */
uint32_t scumm_save(uint8_t *out, uint32_t outMax, uint8_t *raw, uint32_t rawMax); /* 0: no fit */
int  scumm_load(const uint8_t *in, uint32_t len, uint8_t *raw, uint32_t rawMax); /* 0: not a save */
void boxes_saveLoad(Ser *s);
void gfx_saveLoad(Ser *s);
void text_saveLoad(Ser *s);
void sound_saveLoad(Ser *s);
void world_saveLoad(Ser *s);
void ad_saveLoad(Ser *s);
void loop_saveLoad(Ser *s);
void world_afterLoad(void);              /* the room's static data, the screens, the buffers */
void loop_afterLoad(void);
void gfx_afterLoad(void);
void text_afterLoad(void);
void actor_cameraMoved(void);
void redrawVerbs(void);
void text_pauseBanner(void);
void updateCursor(void);
void plat_warpMouse(int x, int y);       /* _system->warpMouse: the platform's cursor there */

/* --- res.c ------------------------------------------------------------------------- */
const uint8_t *res_room(int room);                 /* the RO block */
const uint8_t *res_room_offs(int room, uint32_t offs); /* a resource of the room's LF block */
const uint8_t *res_script(int n);                  /* the SC block */
const uint8_t *res_find_data(const uint8_t *block, const char tag[2]); /* data of a sub-block */
uint16_t rd16(const uint8_t *p);
uint32_t rd32(const uint8_t *p);

/* --- script.c ---------------------------------------------------------------------- */
void runScript(int script, int freezeResistant, int recursive, const int32_t *lvarptr);
void runObjectScript(int object, int entry, int freezeResistant, int recursive, const int32_t *vars);
void stopScript(int script);
void stopObjectScript(int script);
void stopObjectCode(void);
void runAllScripts(void);
void runExitScript(void);
void runEntryScript(void);
void killScriptsAndResources(void);
void freezeScripts(int flag);
void unfreezeScripts(void);
void decreaseScriptDelay(int amount);
int  isScriptInUse(int script);
int  isScriptRunning(int script);
int  isRoomScriptRunning(int script);
void doSentence(int verb, int objectA, int objectB);
void checkAndRunSentenceScript(void);
void beginCutscene(const int32_t *args);
void endCutscene(void);
void abortCutscene(void);
void beginOverride(void);
void endOverride(void);
void updateScriptPtr(void);
uint8_t  fetchScriptByte(void);
unsigned fetchScriptWord(void);
int      fetchScriptWordSigned(void);
int  readVar(unsigned var);
void writeVar(unsigned var, int value);
void push(int a);
int  pop(void);
int  resStrLen(const uint8_t *src);
void executeOpcode(uint8_t op);
#define VAR(n) (g.vars[(n)])

/* --- ops_v5.c --------------------------------------------------------------------- */
extern const char *const opcodeNames[256];
void opcodes_execute(uint8_t op);

/* --- boxes.c ----------------------------------------------------------------------- */
void boxes_resetRoom(const uint8_t *room);
int  getNumBoxes(void);
int  getBoxFlags(int box);
void setBoxFlags(int box, int val);
int  getBoxScale(int box);
void setBoxScale(int box, int scale);
int  getScale(int box, int x, int y);
int  getScaleFromSlot(int slot, int x, int y);
int  getMaskFromBox(int box);
void setScaleSlot(int slot, int x1, int y1, int scale1, int x2, int y2, int scale2);
BoxCoords getBoxCoordinates(int box);
int  checkXYInBoxBounds(int box, int x, int y);
int  getClosestPtOnBox(BoxCoords box, int x, int y, int16_t *outX, int16_t *outY);
int  getNextBox(int from, int to);
void createBoxMatrix(void);
int  findPathTowards(const Actor *a, int box1, int box2, int box3, Point *foundPath);

/* --- actor.c: Actor (walking, directions, animation state) and the camera ------------ */
void actor_init(Actor *a, int mode);
void actor_put(Actor *a, int x, int y, int room);
void actor_hide(Actor *a);
void actor_show(Actor *a);
void actor_animate(Actor *a, int anim);
void actor_startAnim(Actor *a, int f);
void actor_setCostume(Actor *a, int c);
void actor_setWalkSpeed(Actor *a, int sx, int sy);
void actor_startWalk(Actor *a, int destX, int destY, int dir);
void actor_setDirection(Actor *a, int direction);
void actor_turnToDirection(Actor *a, int newdir);
void actor_faceToObject(Actor *a, int obj);
void actor_stopMoving(Actor *a);
void actor_adjustXYToBeInBox(Actor *a, int x, int y, int *ox, int *oy);
int  actor_inCurrentRoom(const Actor *a);
void walkActors(void);
void actor_setupScale(Actor *a);

/* --- costume.c: the classic costumes, as far as the logic sees them ------------------ */
const uint8_t *res_costume(int n);
void costume_reset(CostumeData *c);
void costumeDecodeData(Actor *a, int frame, unsigned usemask);
void actor_animateCostume(Actor *a);
void actor_drawCostume(Actor *a);
int  getActorFromPos(int x, int y);
void playActorSounds(void);

/* --- gfx.c: the drawing bookkeeping (usage bits, draw queue, redraw flags) ----------- */
void gfx_reset(void);
void gfx_clearUsageBits(void);
int  gfx_testUsageBit(int strip, int bit);
int  gfx_testAnyUsageBits(int strip);
void gfx_markActorRect(int left, int right, int top, int bottom, int dirtybit);
void markRectAsDirty(int virt, int left, int right, int top, int bottom, int dirtybit);
void drawLine(int x1, int y1, int x2, int y2, int color);
void restoreBackground(int left, int top, int right, int bottom, int backColor);
void gfx_handleDrawing(void);
void gfx_handleActors(void);
int  gfx_bgNeedsRedraw(void);
void addObjectToDrawQue(int object);
void clearDrawObjectQueue(void);
void markObjectRectAsDirty(int obj);
void showActors(void);
int  newDirToOldDir(int dir);
int  oldDirToNewDir(int dir);
void setCameraAt(int x, int y);
void setCameraFollows(Actor *a, int setCamera);
void actorFollowCamera(int act);
void moveCamera(void);

/* --- world.c: rooms, objects, inventory, strings ------------------------------------ */
void startScene(int room);
int  getObjectIndex(int object);
int  whereIsObject(int object);
int  getOwner(int obj);
int  getVerbEntrypoint(int obj, int entry);
void setObjectName(int obj, const uint8_t *name);
const uint8_t *getNewName(int obj);
void nukeLocalObjectNames(void);
void resetNewNames(void);
void putOwner(int obj, int owner);
int  getState(int obj);
void putState(int obj, int state);
int  getClass(int obj, int cls);
void putClass(int obj, int cls, int set);
int  isValidActor(int id);
Actor *derefActor(int id);
void updateObjectStates(void);
int  findObject(int x, int y);
int  findInventory(int owner, int idx);
int  getInventoryCount(int owner);
void addObjectToInventory(int obj, int room);
void setOwnerOf(int obj, int owner);
void runInventoryScript(int i);
int  getObjectOrActorXY(int object, int *x, int *y);
int  getObjectOrActorWidth(int object, int *width);
void getObjectXYPos(int object, int *x, int *y, int *dir);
int  getObjX(int obj);
int  getObjY(int obj);
const uint8_t *getOBCDFromObject(int obj);
uint32_t getOBCDOffs(int object);

/* --- sound.c: ScummVM's Sound, the queue in front of the player ------------------- */
void sound_init(void);
void sound_queue(int sound);
void sound_stop(int sound);
void sound_stop_all(void);
int  sound_running(int sound);
void sound_process(void);
void sound_updateMusicTimer(void);
const uint8_t *res_sound_ad(int sound);            /* the AD chunk's data, or 0 */

/* --- player_ad.c: the original AdLib driver (ScummVM Player_AD) -------------------- */
#define AD_CALLBACK_FREQUENCY 472                  /* ad_onTimer calls per second */
void ad_init(void);
void ad_startSound(int sound, const uint8_t *res);
void ad_stopSound(int sound);
void ad_stopAllSounds(void);
int  ad_getMusicTimer(void);
int  ad_getSoundStatus(int sound);
void ad_onTimer(void);
void plat_opl_write(int reg, int val);             /* platform: one OPL2 register write */
void plat_ad_lock(int on);                         /* platform: Player_AD's mutex */

/* --- text.c: messages and the charset metrics ------------------------------------------ */
void text_init(void);
void initCharset(int charsetno);
void printString(int m, const uint8_t *msg);
void actorTalk(const uint8_t *msg);
void displayDialog(void);
void stopTalk(void);
int  text_hasMask(void);
int  text_isTalking(int act);
int  text_fontHeight(void);
int  text_getStringWidth(int arg, const uint8_t *text);
void drawVerb(int verb, int mode);
void verbMouseOver(int verb);
void handleMouseOver(void);
int  findVerbAtPos(int x, int y);

/* --- palette.c: the palette and its effects ------------------------------------------- */
void resetPalette(void);
void setDirtyColors(int min, int max);
void setPalColor(int idx, int r, int gr, int b);
void setCurrentPalette(int palindex);
void palette_resetRoom(void);
void initCycl(const uint8_t *ptr);
void stopCycle(int i);
void cyclePalette(void);
void palManipulateInit(int resID, int start, int end, int time);
void palManipulate(void);
void setShadowPalette(int redScale, int greenScale, int blueScale, int startColor, int endColor,
                      int start, int end);
void darkenPalette(int redScale, int greenScale, int blueScale, int startColor, int endColor);
void updatePalette(void);

/* --- cursor.c: the mouse cursor ------------------------------------------------------ */
void resetCursors(void);
void setBuiltinCursor(int idx);
void animateCursor(void);

/* --- loop.c ----------------------------------------------------------------------- */
/* image.c: the game image from the game's own (decrypted) files, read in place */
void image_index(GameImage *img, const uint8_t *idx, uint32_t size);
void image_disk(GameImage *img, int d, const uint8_t *data, uint32_t size);
void scumm_init(const GameImage *img);
void scumm_initScreens(int b, int h);
int  scumm_mainScreenHeight(void);
int  scumm_virtscrHeight(int virt);
int  scumm_virtscrTop(int virt);
void clearClickedStatus(void);
void fadeIn(int effect);
void fadeOut(int effect);
int  scumm_findVirtScreen(int y);                  /* 0 main, 1 text, 2 verbs, -1 none */
void scumm_mouseMove(int x, int y);                /* platform input: game screen coordinates */
void scumm_mousePos(int *x, int *y);               /* the mouse, for the platform's cursor */
/* a key, as ScummVM's _keyPressed (its ASCII; 27 ESC, 13 Return, 9 Tab), seen by the next
 * loop round's processInput */
void scumm_keyPress(int ascii);
/* the Jaguar keypad (0-9, 10 '*', 11 '#') as the key of the verb at its place (keypad.c);
 * 0 = no verb there */
int  scumm_keypadKey(int key);
void scumm_mouseDown(int right);
void scumm_mouseUp(int right);
unsigned scumm_random(unsigned max);
void scummLoop(int delta);

#endif
