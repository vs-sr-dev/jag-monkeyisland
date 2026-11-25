/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * loop.c - start-up and the main loop.
 * ScummVM scumm.cpp: resetScumm, runBootscript, go, scummLoop, scummLoop_updateScummVars;
 * vars.cpp: resetScummVars (v4); input.cpp: processInput (the mouse); verbs.cpp: checkExecVerbs;
 * script.cpp: runInputScript; gfx.cpp: initScreens, findVirtScreen.
 */
#include <string.h>
#include "scumm.h"

static struct { int x, y; } mouse, virtualMouse;

/* the virtual screens (initScreens): 0 main, 1 text, 2 verbs; top line and height */
enum { kMainVirtScreen = 0, kTextVirtScreen = 1, kVerbVirtScreen = 2, kBannerVirtScreen = 3 };
static struct { int topline, h; } virtscr[4];

/* the buttons as ScummVM's event handler leaves them (input.cpp: msDown, msClicked) */
enum { msDown = 1, msClicked = 2 };
static int leftBtnPressed, rightBtnPressed, mouseAndKeyboardStat;
enum { MBS_LEFT_CLICK = 0x8000, MBS_RIGHT_CLICK = 0x4000, MBS_MOUSE_MASK = 0xC000,
       MBS_MAX_KEY = 0x0200 };
enum { kVerbClickArea = 1, kSceneClickArea = 2, kKeyClickArea = 4 };

/* --- the platform's input events (ScummEngine::parseEvent) ------------------------------ */
void scumm_mouseMove(int x, int y)
{
    mouse.x = x;
    mouse.y = y;
}

static int keyPressed;                       /* _keyPressed: the key of the next round */

void scumm_keyPress(int ascii)
{
    keyPressed = ascii;
}

/* where the platform draws the cursor: the mouse as the input left it */
void scumm_mousePos(int *x, int *y)
{
    *x = mouse.x;
    *y = mouse.y;
}

void scumm_mouseDown(int right)
{
    if (right)
        rightBtnPressed |= msClicked | msDown;
    else
        leftBtnPressed |= msClicked | msDown;
}

void scumm_mouseUp(int right)
{
    if (right)
        rightBtnPressed &= ~msDown;
    else
        leftBtnPressed &= ~msDown;
}

/* Common::RandomSource::getRandomNumber (ScummVM common/random.cpp), bit for bit */
unsigned scumm_random(unsigned max)
{
    g.randSeed ^= g.randSeed >> 13;
    g.randSeed ^= g.randSeed << 21;
    g.randSeed ^= g.randSeed >> 11;
    return (unsigned)((uint32_t)(g.randSeed * 0xDEADBF03u) % ((uint32_t)max + 1));
}

int scumm_mainScreenHeight(void)
{
    return virtscr[kMainVirtScreen].h;
}

int scumm_virtscrHeight(int virt)
{
    return virtscr[virt].h;
}

int scumm_virtscrTop(int virt)
{
    return virtscr[virt].topline;
}

/* initScreens: the logic keeps each virtual screen's top line and height; the screens
 * themselves are the platform's */
void scumm_initScreens(int b, int h)
{
    virtscr[kBannerVirtScreen].topline = 80;        /* v < 7, not FM-Towns */
    virtscr[kBannerVirtScreen].h = 12;
    virtscr[kMainVirtScreen].topline = b;
    virtscr[kMainVirtScreen].h = h - b;
    virtscr[kTextVirtScreen].topline = 0;
    virtscr[kTextVirtScreen].h = b;
    virtscr[kVerbVirtScreen].topline = h;
    virtscr[kVerbVirtScreen].h = g.screenHeight - h;
    plat_initScreens(b, h);
}

/* handleMouseOver (verbs.cpp): the verb under the mouse is drawn highlighted */
void handleMouseOver(void)
{
    if (g.cursor.state > 0)                  /* _completeScreenRedraw: the GUI's only */
        verbMouseOver(findVerbAtPos(mouse.x, mouse.y));
}

/* clearClickedStatus (input.cpp) */
void clearClickedStatus(void)
{
    keyPressed = 0;
    mouseAndKeyboardStat = 0;
    leftBtnPressed &= ~msClicked;
    rightBtnPressed &= ~msClicked;
}

/* The screen effects (gfx.cpp): 1-6 transitionEffect(effect - 1), 128 dissolveEffectSelector
 * (DOS: dissolveEffect(8, 4)), 129 none, 130-133 scrollEffect(133 - effect), 134
 * dissolveEffect(1, 1), 135 dissolveEffect(1, main height).  The drawing is the platform's;
 * which effect runs, and _screenEffectFlag, are the logic's. */
static void checkScreenEffect(int effect, const char *who)
{
    (void)who;
    if ((effect >= 1 && effect <= 6) || (effect >= 128 && effect <= 135) || effect == 0)
        return;
    plat_error("Unknown screen effect");
}

/* dissolveEffect (gfx.cpp), the order: a permutation of the blocks of the main screen drawn
 * from the GAME's random source (_rnd), so a dissolve moves every later random number: it
 * runs here, with ScummVM's calls; the platform draws the blocks in that order.  Offsets fit
 * 16 bits (y * 320 + x < 320 x 200). */
static void dissolveEffect(int width, int height)
{
    const int vw = 320, vh = virtscr[kMainVirtScreen].h;
    int w = vw / width, h = vh / height, i, x, y, n;
    uint16_t *offsets;

    if (vw % width)
        w++;
    if (vh % height)
        h++;
    n = w * h;
    offsets = plat_dissolveBuffer(n);
    if (width == 1 && height == 1) {
        for (i = 0; i < vw * vh; i++)
            offsets[i] = (uint16_t)i;
        for (i = 1; i < n; i++) {
            int j = (int)scumm_random((unsigned)(i - 1));
            offsets[i] = offsets[j];
            offsets[j] = (uint16_t)i;
        }
    } else {
        /* offsets2: the identity order, read as the permutation is built; offsets[j] is only
         * ever written at j < i, so offsets2[i] is the order's own entry i */
        for (i = 0, x = 0; x < vw; x += width)
            for (y = 0; y < vh; y += height)
                offsets[i++] = (uint16_t)(y * 320 + x);
        for (i = 1; i < n; i++) {
            int j = (int)scumm_random((unsigned)(i - 1));
            int xi = (i / h) * width, yi = (i % h) * height;     /* offsets2[i] */
            offsets[i] = offsets[j];
            offsets[j] = (uint16_t)(yi * 320 + xi);
        }
    }
    plat_dissolve(width, height, offsets, n);
}

static void screenDissolve(int effect)
{
    if (effect == 128)
        dissolveEffect(8, 4);                /* dissolveEffectSelector: DOS */
    else if (effect == 134)
        dissolveEffect(1, 1);
    else if (effect == 135)
        dissolveEffect(1, virtscr[kMainVirtScreen].h);
}

/* fadeIn: the palette first, then the effect */
void fadeIn(int effect)
{
    updatePalette();
    checkScreenEffect(effect, "fadeIn");
    plat_fadeIn(effect);
    screenDissolve(effect);
    g.screenEffectFlag = 1;
}

/* fadeOut: v < 7 sets the camera's last position to the current one (the new room starts
 * "not scrolled": a message printed in its first frame shows at once when the camera lands
 * where it was, room 74 from 38); the effect runs only after a fadeIn (_screenEffectFlag) */
void fadeOut(int effect)
{
    int run = g.screenEffectFlag && effect != 0;
    g.camera.last_x = g.camera.cur_x;
    if (run)
        checkScreenEffect(effect, "fadeOut");
    plat_fadeOut(effect, run);                /* the black screen first */
    if (run)
        screenDissolve(effect);
    g.screenEffectFlag = 0;
}

/* scummLoop_handleEffects */
static void handleEffects(void)
{
    cyclePalette();                          /* v >= 4 */
    palManipulate();
    if (g.doEffect) {
        g.doEffect = 0;
        fadeIn(g.newEffect);
        clearClickedStatus();
    }
}

int scumm_findVirtScreen(int y)
{
    int i;
    if (g.forceBannerVirtScreen)
        return kBannerVirtScreen;
    for (i = 0; i < 3; i++)
        if (y >= virtscr[i].topline && y < virtscr[i].topline + virtscr[i].h)
            return i;
    return -1;
}

/* processKeyboard (input.cpp), what MI1 DOS meets with the original interface: '.' ends the
 * line being said (VAR_TALKSTOP_KEY is not a v4 variable: always on), ESC jumps to the
 * cutscene's override (when VAR_CUTSCENEEXIT_KEY is set), any other key goes to the scripts;
 * Return and Tab are the left and right clicks while the cursor is on. */
static void processKeyboard(int key)
{
    if (VAR(VAR_PAUSE_KEY) != 0 && key == VAR(VAR_PAUSE_KEY)) {
        /* the original GUI: the cursor off, the banner until a key, the cursor back */
        const int oldCursorState = g.cursor.state;
        g.cursor.state = 0;
        plat_showMouse(0);
        text_pauseBanner();
        g.cursor.state = oldCursorState;
        return;
    }
    if (key == '.') {
        g.talkDelay = 0;                     /* (stopTalk only with a talkie) */
    } else if (key == 27 && VAR(VAR_CUTSCENEEXIT_KEY) != 0) {
        abortCutscene();
        mouseAndKeyboardStat = VAR(VAR_CUTSCENEEXIT_KEY);
    } else {
        mouseAndKeyboardStat = key;
    }
    if (mouseAndKeyboardStat == 13 && g.cursor.state > 0)
        mouseAndKeyboardStat = MBS_LEFT_CLICK;
    else if (mouseAndKeyboardStat == 9 && g.cursor.state > 0)
        mouseAndKeyboardStat = MBS_RIGHT_CLICK;
}

static void processInput(void)
{
    int lastKeyHit = keyPressed;
    keyPressed = 0;

    if (mouse.x < 0) mouse.x = 0;
    if (mouse.x > g.screenWidth - 1) mouse.x = g.screenWidth - 1;
    if (mouse.y < 0) mouse.y = 0;
    if (mouse.y > g.screenHeight - 1) mouse.y = g.screenHeight - 1;

    virtualMouse.x = mouse.x + g.mainXStart;
    virtualMouse.y = mouse.y - virtscr[kMainVirtScreen].topline;
    if (virtualMouse.y < 0)
        virtualMouse.y = -1;
    if (virtualMouse.y >= virtscr[kMainVirtScreen].h)
        virtualMouse.y = -1;

    mouseAndKeyboardStat = 0;
    if ((leftBtnPressed & msClicked) && (rightBtnPressed & msClicked)) {
        mouseAndKeyboardStat = 0;            /* both buttons: ESC, as the original (v4+) */
        lastKeyHit = 27;
    } else if (leftBtnPressed & msClicked)
        mouseAndKeyboardStat = MBS_LEFT_CLICK;
    else if (rightBtnPressed & msClicked)
        mouseAndKeyboardStat = MBS_RIGHT_CLICK;

    leftBtnPressed &= ~msClicked;
    rightBtnPressed &= ~msClicked;
    if (lastKeyHit)
        processKeyboard(lastKeyHit);
}

static void runInputScript(int clickArea, int val, int mode)
{
    int32_t args[NUM_SCRIPT_LOCAL];
    int verbScript = VAR(VAR_VERB_SCRIPT);
    memset(args, 0, sizeof(args));
    args[0] = clickArea;
    args[1] = val;
    args[2] = mode;
    if (verbScript)
        runScript(verbScript, 0, 0, args);
}

int findVerbAtPos(int x, int y)
{
    int i = NUM_VERBS - 1;
    for (; i > 0; i--) {
        const VerbSlot *vs = &g.verbs[i];
        if (vs->curmode != 1 || !vs->verbid || vs->saveid || y < vs->y || y >= vs->bottom)
            continue;
        if (vs->center) {
            if (x < -(vs->right - 2 * vs->x) || x >= vs->right)
                continue;
        } else {
            if (x < vs->x || x >= vs->right)
                continue;
        }
        return i;
    }
    return 0;
}

static void checkExecVerbs(void)
{
    int i, zone;
    if (g.userPut <= 0 || mouseAndKeyboardStat == 0)
        return;
    if (mouseAndKeyboardStat < MBS_MAX_KEY) {
        for (i = 1; i < NUM_VERBS; i++) {
            VerbSlot *vs = &g.verbs[i];
            if (vs->verbid && vs->saveid == 0 && vs->curmode == 1 && mouseAndKeyboardStat == vs->key) {
                runInputScript(kVerbClickArea, vs->verbid, 1);
                return;
            }
        }
        runInputScript(kKeyClickArea, mouseAndKeyboardStat, 1);
    } else if (mouseAndKeyboardStat & MBS_MOUSE_MASK) {
        const int code = (mouseAndKeyboardStat & MBS_LEFT_CLICK) ? 1 : 2;
        zone = scumm_findVirtScreen(mouse.y);
        if (zone < 0)
            return;
        i = findVerbAtPos(mouse.x, mouse.y);
        if (i != 0)
            runInputScript(kVerbClickArea, g.verbs[i].verbid, code);
        else
            runInputScript(zone == kMainVirtScreen ? kSceneClickArea : kVerbClickArea, 0, code);
    }
}

static void resetScummVars(void)
{
    VAR(VAR_SOUNDCARD) = g.soundcard;        /* setSoundCardVarToCurrentConfig */
    VAR(VAR_VIDEOMODE) = 19;                 /* VGA */
    VAR(VAR_HEAPSPACE) = 1400;
    VAR(VAR_FIXEDDISK) = 1;
    VAR(VAR_DEBUGMODE) = g.debugMode;
    VAR(VAR_CHARINC) = 4;
    VAR(VAR_TALK_ACTOR) = 0;                 /* setTalkingActor(0) */
    VAR(VAR_V5_TALK_STRING_Y) = -0x50;
    VAR(VAR_CURRENT_LIGHTS) = 1 | 2 | 8;     /* actor base palette | room lights on | actor colours */
}

void scumm_init(const GameImage *img)
{
    int i, trace = g.trace, soundcard = g.soundcard, debugMode = g.debugMode, bootParam = g.bootParam;
    int copyProtection = g.copyProtection;
    uint32_t seed = g.randSeed;
    memset(&g, 0, sizeof(g));
    g.copyProtection = copyProtection;
    g.trace = trace;
    g.soundcard = soundcard;
    g.debugMode = debugMode;
    g.bootParam = bootParam;
    g.randSeed = seed ? seed : 1;            /* RandomSource::setSeed: 0 becomes 1 */
    g.img = img;
    g.screenWidth = 320;
    g.screenHeight = 200;
    resetCursors();                          /* ScummEngine_v5's constructor */
    g.currentScript = 0xFF;
    resetNewNames();
    gfx_reset();

    /* the global object table (ScummEngine_v4::readGlobalObjects) */
    for (i = 0; i < NUM_GLOBAL_OBJECTS; i++) {
        const uint8_t *p = img->objects + 4 * i;
        g.classData[i] = p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16);
        g.objectOwnerTable[i] = p[3] & OF_OWNER_MASK;
        g.objectStateTable[i] = p[3] >> OF_STATE_SHL;
    }

    /* resetScumm */
    for (i = 0; i < 256; i++)
        g.roomPalette[i] = (uint8_t)i;
    g.roomPaletteGen++;
    for (i = 0; i < NUM_ACTORS; i++) {           /* after readIndexFile: clears classData[i] */
        g.actors[i].number = (uint8_t)i;
        actor_init(&g.actors[i], -1);
    }
    for (i = 0; i < NUM_VERBS; i++) {
        g.verbs[i].color = 2;
        g.verbs[i].charset = 1;
        g.verbs[i].right = (int16_t)(g.screenWidth - 1);
        g.verbs[i].oldLeft = -1;
    }
    g.camera.leftTrigger = 10;
    g.camera.rightTrigger = 30;
    g.camera.mode = 0;
    mouse.x = 104;
    mouse.y = 56;
    leftBtnPressed = rightBtnPressed = mouseAndKeyboardStat = 0;
    g.newEffect = 129;
    g.fullRedraw = 1;
    scumm_initScreens(16, 144);
    g.palManipCounter = 0;
    resetPalette();                          /* resetPalette(true): the roomPalette is the identity */
    g.cursor.animate = 1;

    text_init();                             /* the string slots' defaults */
    sound_init();                            /* setupMusic: the player exists before the boot */
    resetScummVars();

    /* runBootscript: script 1 with boot parameter 0 */
    {
        int32_t args[NUM_SCRIPT_LOCAL];
        memset(args, 0, sizeof(args));
        args[0] = g.bootParam;
        runScript(1, 0, 0, args);
    }
}

/* scummLoop, the v4 path. */
void scummLoop(int delta)
{
    PROF(0);
    VAR(VAR_TIMER) = delta;
    VAR(VAR_TIMER_TOTAL) += delta;
    VAR(VAR_TMR_1) += delta;
    VAR(VAR_TMR_2) += delta;
    VAR(VAR_TMR_3) += delta;

    if (delta > 15)
        delta = 15;
    decreaseScriptDelay(delta);

    g.talkDelay -= delta;
    if (g.talkDelay < 0)
        g.talkDelay = 0;

    PROF(1);
    processInput();

    /* scummLoop_updateScummVars */
    VAR(VAR_CAMERA_POS_X) = g.camera.cur_x;
    VAR(VAR_HAVE_MSG) = g.haveMsg;
    VAR(VAR_VIRT_MOUSE_X) = virtualMouse.x;
    VAR(VAR_VIRT_MOUSE_Y) = virtualMouse.y;
    VAR(VAR_MOUSE_X) = mouse.x;
    VAR(VAR_MOUSE_Y) = mouse.y;
    VAR(VAR_DEBUGMODE) = g.debugMode;

    sound_updateMusicTimer();

    /* scummLoop_handleSaveLoad (load_game:), then a load's _completeScreenRedraw */
    if (g.saveLoadFlag) {
        const int flag = g.saveLoadFlag;
        if (plat_saveLoad(flag) && flag != 1) {
            clearClickedStatus();
            if (g.completeScreenRedraw)      /* ScummEngine_v5: the text mask is clear */
                redrawVerbs();
        }
        g.saveLoadFlag = 0;
    }
    if (g.completeScreenRedraw) {
        handleMouseOver();
        g.completeScreenRedraw = 0;
        g.fullRedraw = 1;
    }

    PROF(2);
    runAllScripts();
    PROF(3);
    checkExecVerbs();
    checkAndRunSentenceScript();

    if (g.quit)                              /* shouldQuit(): o5_systemOps' quitGame */
        return;

    if (g.currentRoom != 0) {
        PROF(4);
        walkActors();
        moveCamera();
        PROF(5);
        updateObjectStates();
        PROF(6);
        displayDialog();
        /* scummLoop_handleDrawing -> redrawBGAreas: a scroll takes the text away */
        if (g.camera.cur_x != g.camera.last_x && text_hasMask())
            stopTalk();
        PROF(7);
        gfx_handleDrawing();
        PROF(8);
        gfx_handleActors();                  /* scummLoop_handleActors */
        g.fullRedraw = 0;
        handleEffects();
        handleMouseOver();                   /* oldEgo != VAR_EGO: unused before v7 */
        updatePalette();
        plat_drawDirtyScreenParts();
        PROF(9);
        playActorSounds();
    } else {
        displayDialog();                     /* v > 3 */
        plat_drawDirtyScreenParts();
    }
    PROF(10);
    sound_process();                         /* scummLoop_handleSound */
    g.camera.last_x = g.camera.cur_x;
    animateCursor();
    plat_showMouse(g.cursor.state > 0);
    PROF(11);
}

/* --- save and load: _screenB / _screenH (initScreens' arguments) ---------------------------- */
static int loadedB, loadedH;

void loop_saveLoad(Ser *s)
{
    int b = virtscr[kMainVirtScreen].topline, h = b + virtscr[kMainVirtScreen].h;
    SER(s, b);
    SER(s, h);
    SER(s, mouse.x);                         /* not restored by ScummVM's original GUI; the */
    SER(s, mouse.y);                         /* platform puts its cursor there, or moves it */
    if (s->loading) {
        loadedB = b;
        loadedH = h;
    }
}

void loop_afterLoad(void)
{
    scumm_initScreens(loadedB, loadedH);
}
