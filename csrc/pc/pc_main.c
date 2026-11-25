/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Samuele Voltan.  See LICENSE. */
/*
 * pc_main.c - the PC build of the interpreter: the oracle harness.
 *
 *     scumm_pc GAME_DIR [STEPS] [--soundcard N] [--debugmode N]
 *
 * A ScummVM run that prints its trace has a debug level, hence _debugMode = 1, and the boot
 * script reads VAR_DEBUGMODE: compare with --debugmode 1 --soundcard 0 (music_driver=null).
 *
 * Loads the user's files (000.LFL, 90x.LFL, DISK01-04.LEC, flat or in d1..d4), runs the boot
 * script and STEPS main-loop rounds, each advancing VAR_TIMER_NEXT jiffies as ScummVM's go()
 * does, and prints the opcode trace in ScummVM's DEBUG_OPCODES format on stdout, so that
 * tools/trace_cmp.py can diff it against a ScummVM run.
 *
 *     --wav FILE      the AdLib output through Nuked OPL3 (a YM3812 at 49 716 Hz, writes
 *                     buffered 2 chip samples apart like the card's), mono, for listening
 *     --opl           print each OPL2 write in the trace ("OPL tick reg val")
 *     --bootparam N   runBootscript's argument (1: the game start, no intro)
 *     --copyprot N    1: the Dial-A-Pirate wheel runs (the port's default on the Jaguar);
 *                     0 (default): skipped, as ScummVM's copy_protection=false
 *     --input FILE    input events, one per line: "FRAME move X Y", "FRAME down L|R",
 *                     "FRAME up L|R", "FRAME key ASCII" (decimal: 108 is 'l'), "FRAME
 *                     keypad K" (the Jaguar keypad: 0-9, * or #, through keypad.c as the
 *                     ROM has it); FRAME is the loop round (0 = the first), the event
 *                     arriving during the wait before it, as ScummVM's parseEvents does
 *     --ev "FRAME ..."  one event on the command line (repeatable)
 *     --frames        print "FRAME n" before each loop round (tools/input_fit.py)
 *     --state         camera, message and talk delay after each round, on stderr
 *     --verbs         the active verbs' rectangles at the end (planning clicks)
 *     --shots FILE    the picture: after each loop round "N CRC" (CRC32 of the 320x200 RGB
 *                     frame, row-major R G B), and "N.k CRC" for each screen an effect shows
 *                     inside round N; tools/shot_cmp.py matches ScummVM's shots against it
 *     --ppm DIR --ppm-frames A-B   the frames of rounds A..B as DIR/fNNNNN.ppm
 *     --palcrc        "pal N CRC" after round N when the palette changed (CRC32 of the 768
 *                     RGB bytes), as the Jaguar logs its CLUT hand-overs (tools/pal_crc.py)
 *     --bbcrc         "bb N CRC" after round N when the back buffer's visible window (320 x
 *                     the main screen's height) changed, as the Jaguar logs its room buffer
 *     --zcrc          "z N CRC" after round N when the z-plane buffer changed (CRC32 of the
 *                     g.numZBuffer x (room height + 4) x 40 bytes initBGBuffers sized), as the
 *                     Jaguar logs its mask buffer
 *     --fbcrc         "fb N CRC" after round N when the front buffer's visible window (the
 *                     main virtual screen, 320 x its height) changed
 *     --sccrc         "sc N CRC" after round N when the system screen (320 x 200 palette
 *                     indices, without the cursor) changed
 *     --curcrc        "cur N CRC" after round N when the cursor changed (CRC32 of its image,
 *                     then width, height, hotspot x, y, shown: 1 byte each, then the
 *                     mouse x, y: 2 bytes each, big-endian)
 *     --no-cels, --no-chars   the costumes' or the glyphs' pixels not drawn (the logic is the
 *                     same): the Jaguar's checks before it draws them
 *
 * Time: each loop round advances VAR_TIMER_NEXT jiffies (1/60 s) before scummLoop runs, as
 * ScummVM's go() waits before it; the player's 472 Hz callback runs inside that wait.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../scumm/scumm.h"
#include "../ref/ref.h"
#ifdef WITH_NUKED
#include "opl3.h"
static opl3_chip opl;
#endif

static GameImage img;

/* input events, applied before the loop round they name */
#define MAX_EVENTS 4096
/* kind: 0 move, 1 down, 2 up, 3 key (a = ASCII), 4 keypad (a = 0-9, 10 '*', 11 '#') */
static struct { int frame, kind, a, b; } events[MAX_EVENTS];
static int numEvents;
static int setStateObj, setStateVal;
static const char *dumpG;                    /* --dump-g FILE: the interpreter's struct at the end */
/* --save-at N FILE: the game saved at round N's handleSaveLoad (the run goes on);
 * --load FILE N: loaded at round N's (rounds before it not run; FILE saved at N).  The trace
 * prints "SAVELOAD" there: what follows must be the same in both. */
static const char *saveFile, *loadFile;
static int saveAt = -1, loadFrom, loadAt = -1;  /* --load-at N FILE: loaded at round N of a
                                                   whole run (back in time; the ROM's test) */
static uint8_t saveBuf[256 * 1024], saveRaw[128 * 1024];

int plat_saveLoad(int flag)
{
    FILE *f;
    uint32_t n;
    if (flag == 1) {
        n = scumm_save(saveBuf, sizeof(saveBuf), saveRaw, sizeof(saveRaw));
        f = fopen(saveFile, "wb");
        if (!n || !f) {
            fprintf(stderr, "save: %s\n", n ? "cannot write the file" : "too big");
            return 0;
        }
        fwrite(saveBuf, 1, n, f);
        fclose(f);
        fprintf(stderr, "save: %u bytes into %s at round %d\n", (unsigned)n, saveFile, saveAt);
    } else {
        f = fopen(loadFile, "rb");
        if (!f) {
            fprintf(stderr, "load: cannot read %s\n", loadFile);
            return 0;
        }
        n = (uint32_t)fread(saveBuf, 1, sizeof(saveBuf), f);
        fclose(f);
        if (!scumm_load(saveBuf, n, saveRaw, sizeof(saveRaw))) {
            fprintf(stderr, "load: %s is not a save of this game\n", loadFile);
            return 0;
        }
    }
    if (g.trace)
        plat_trace("SAVELOAD");
    return 1;
}

void plat_clearTextSurface(void)
{
    ref_clearTextSurface();
}

void plat_warpMouse(int x, int y)
{
    ref_setMouse(x, y);
}
static int verbLog;                          /* --verbs: the verbs' rectangles at the end */
static int stateLog;                         /* --state: camera and message, per frame */
static int frameMarks;                       /* --frames: "FRAME n" before each loop round */
static int egoLog;                           /* --ego: the ego actor after each round, on stderr */

static void add_event(const char *line)
{
    int frame, a = 0, b = 0;
    char what[16], btn[8];
    if (numEvents >= MAX_EVENTS)
        return;
    if (sscanf(line, "%d move %d %d", &frame, &a, &b) == 3) {
        events[numEvents].kind = 0;
    } else if (sscanf(line, "%d %15s %7s", &frame, what, btn) == 3 &&
               (!strcmp(what, "down") || !strcmp(what, "up"))) {
        events[numEvents].kind = !strcmp(what, "down") ? 1 : 2;
        a = (btn[0] == 'R' || btn[0] == 'r');
    } else if (sscanf(line, "%d key %d", &frame, &a) == 2) {
        events[numEvents].kind = 3;
    } else if (sscanf(line, "%d keypad %7s", &frame, btn) == 2 &&
               ((btn[0] >= '0' && btn[0] <= '9') || btn[0] == '*' || btn[0] == '#') && !btn[1]) {
        events[numEvents].kind = 4;
        a = btn[0] == '*' ? 10 : btn[0] == '#' ? 11 : btn[0] - '0';
    } else {
        if (line[0] && line[0] != '#' && line[0] != '\n')
            fprintf(stderr, "input: cannot read '%s'\n", line);
        return;
    }
    events[numEvents].frame = frame;
    events[numEvents].a = a;
    events[numEvents].b = b;
    numEvents++;
}

static void present_move(void);

/* The events of the wait before round `frame`.  The scripts see a move when the round starts
 * (processInput), but ScummVM's backend draws the cursor where the mouse is at once: the
 * screen of the round before is shown again with the cursor moved (present_move). */
static void apply_events(int frame)
{
    int i;
    for (i = 0; i < numEvents; i++) {
        if (events[i].frame != frame)
            continue;
        switch (events[i].kind) {
        case 0:
            scumm_mouseMove(events[i].a, events[i].b);
            ref_setMouse(events[i].a, events[i].b);
            present_move();
            break;
        case 1: scumm_mouseDown(events[i].a); break;
        case 2: scumm_mouseUp(events[i].a); break;
        case 3: scumm_keyPress(events[i].a); break;
        case 4: {
            const int ascii = scumm_keypadKey(events[i].a);
            if (ascii)
                scumm_keyPress(ascii);
            break;
        }
        }
    }
}
static FILE *wav;
static uint32_t wavSamples;
static int oplTrace;
static uint32_t adTicks;

void plat_opl_write(int reg, int val)
{
    if (oplTrace) {
        char line[48];
        snprintf(line, sizeof(line), "OPL %u %02X %02X", adTicks, reg, val);
        plat_trace(line);
    }
#ifdef WITH_NUKED
    if (wav)
        OPL3_WriteRegBuffered(&opl, (uint16_t)reg, (uint8_t)val);
#endif
}

/* Player_AD's mutex: the PC runs the driver in the same thread as the game */
void plat_ad_lock(int on)
{
    (void)on;
}

#define OPL_RATE 49716

static void wav_header(uint32_t n)
{
    uint8_t h[44];
    uint32_t data = n * 2, v;
    memcpy(h, "RIFF", 4); v = 36 + data; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8); v = 16; memcpy(h + 16, &v, 4);
    h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;
    v = OPL_RATE; memcpy(h + 24, &v, 4); v = OPL_RATE * 2; memcpy(h + 28, &v, 4);
    h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
}

/* delta jiffies of audio time: the 472 Hz callback, and the chip's samples between calls */
static void advance_audio(int jiffies)
{
    static uint32_t tickAcc, sampAcc;
    tickAcc += (uint32_t)jiffies * AD_CALLBACK_FREQUENCY;
    while (tickAcc >= 60) {
        tickAcc -= 60;
        ad_onTimer();
        adTicks++;
#ifdef WITH_NUKED
        if (wav) {
            uint32_t n;
            sampAcc += OPL_RATE;
            n = sampAcc / AD_CALLBACK_FREQUENCY;
            sampAcc %= AD_CALLBACK_FREQUENCY;
            while (n--) {
                int16_t lr[2];
                OPL3_GenerateResampled(&opl, lr);
                fwrite(&lr[0], 2, 1, wav);
                wavSamples++;
            }
        }
#endif
    }
}

/* the room: the reference renderer draws it strip by strip (redrawBGStrip) */
void plat_room(void)
{
}

/* --- the picture: frames out of the reference renderer (csrc/ref/) ----------------------- */
static FILE *shotsFile;
static int palCrc, bbCrc, zCrc, fbCrc, scCrc, curCrc;
static uint32_t bbLast, zLast, fbLast, scLast, curLast;
static int zRoom;
static const char *ppmDir;
static int ppmFirst = -1, ppmLast = -1;
static int curFrame, subFrame;

static uint32_t crcTable[256];

static void crc_init_table(void)
{
    int i, k;
    if (crcTable[1])
        return;
    for (i = 0; i < 256; i++) {
        uint32_t v = (uint32_t)i;
        for (k = 0; k < 8; k++)
            v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
        crcTable[i] = v;
    }
}

static uint32_t crc32_bytes(const uint8_t *p, int n)
{
    uint32_t c = 0xFFFFFFFFu;
    crc_init_table();
    while (n--)
        c = crcTable[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static uint32_t crc32_rgb(void)
{
    const uint8_t *scr = ref_frame(), *pal = ref_palette();
    uint32_t c = 0xFFFFFFFFu;
    int i, k;
    if (!crcTable[1])
        for (i = 0; i < 256; i++) {
            uint32_t v = (uint32_t)i;
            for (k = 0; k < 8; k++)
                v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crcTable[i] = v;
        }
    for (i = 0; i < 320 * 200; i++)
        for (k = 0; k < 3; k++)
            c = crcTable[(c ^ pal[scr[i] * 3 + k]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void write_ppm(const char *name)
{
    char path[512];
    const uint8_t *scr = ref_frame(), *pal = ref_palette();
    FILE *f;
    int i;
    snprintf(path, sizeof(path), "%s/%s.ppm", ppmDir, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n320 200\n255\n");
    for (i = 0; i < 320 * 200; i++)
        fwrite(pal + scr[i] * 3, 1, 3, f);
    fclose(f);
}

/* a screen shown: after a loop round (sub = 0), or inside one by an effect */
static void present(int sub)
{
    char name[32];
    if (shotsFile) {
        if (sub)
            fprintf(shotsFile, "%d.%d %08x\n", curFrame, sub, crc32_rgb());
        else
            fprintf(shotsFile, "%d %08x\n", curFrame, crc32_rgb());
    }
    if (ppmDir && curFrame >= ppmFirst && curFrame <= ppmLast) {
        if (sub)
            snprintf(name, sizeof(name), "f%05d_%d", curFrame, sub);
        else
            snprintf(name, sizeof(name), "f%05d", curFrame);
        write_ppm(name);
    }
}

static void present_effect(void)
{
    present(++subFrame);
}

static void present_move(void)
{
    present(++subFrame);
}

/* pauseEngine: the music advances with the rounds here, and none runs while paused */
void plat_pauseEngine(int pause)
{
    (void)pause;
}

/* waitForBannerInput(-1): the screen shown (a shot "N.k"), then the first input event after
 * the round's own: its moves applied on the way, a key or a keypad key gives its ASCII, a
 * button press 0 (its release, later, goes to the game as any).  An input file's event that
 * ends a banner is written at the round after the banner's (tools/input_fit.py finds it so).
 * No event left: 0, as a click. */
int plat_waitBannerInput(void)
{
    int i;
    present(++subFrame);
    if (scCrc)                               /* the Jaguar logs the same at its banner */
        printf("banner %d crc %x\n", curFrame, crc32_bytes(ref_screen(), 320 * 200));
    for (i = 0; i < numEvents; i++) {
        if (events[i].frame <= curFrame)
            continue;
        events[i].frame = -1;                /* taken: apply_events never sees it */
        switch (events[i].kind) {
        case 0:
            scumm_mouseMove(events[i].a, events[i].b);
            ref_setMouse(events[i].a, events[i].b);
            continue;
        case 3:
            return events[i].a;
        case 4: {
            const int ascii = scumm_keypadKey(events[i].a);
            if (!ascii) {
                continue;                    /* a key the keypad does not send */
            }
            return ascii;
        }
        case 1:
            return 0;
        default:
            continue;
        }
    }
    return 0;
}

void plat_trace(const char *line)
{
    puts(line);
}

void plat_error(const char *msg)
{
    fprintf(stdout, "FATAL: %s\n", msg);
    fflush(stdout);
    exit(2);
}

static uint8_t *load(const char *dir, const char *name, int disk, uint32_t *size)
{
    char path[512];
    FILE *f;
    uint8_t *buf;
    long n;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) {
        snprintf(path, sizeof(path), "%s/d%d/%s", dir, disk, name);
        f = fopen(path, "rb");
    }
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return 0;
    }
    fclose(f);
    if (size)
        *size = (uint32_t)n;
    return buf;
}

/* the game's files, decrypted, handed to image.c (which keeps pointers into them) */
static void load_game(const char *dir)
{
    uint32_t size;
    uint8_t *idx = load(dir, "000.LFL", 1, &size);
    int d, i;

    if (!idx) {
        fprintf(stderr, "000.LFL not found in %s\n", dir);
        exit(1);
    }
    image_index(&img, idx, size);
    for (d = 1; d <= 4; d++) {
        char name[16];
        uint8_t *buf;
        snprintf(name, sizeof(name), "DISK%02d.LEC", d);
        buf = load(dir, name, d, &size);
        if (!buf) {
            fprintf(stderr, "%s not found\n", name);
            exit(1);
        }
        for (i = 0; i < (int)size; i++)
            buf[i] ^= 0x69;
        image_disk(&img, d, buf, size);
    }
    for (i = 1; i <= 4; i++) {
        char name[16];
        snprintf(name, sizeof(name), "90%d.LFL", i);
        img.charset[i] = load(dir, name, 1, 0);
    }
}

int main(int argc, char **argv)
{
    int steps = 400, i, delta;
    if (argc < 2) {
        fprintf(stderr, "usage: scumm_pc GAME_DIR [STEPS] [--soundcard N]\n");
        return 1;
    }
    if (argc > 2 && argv[2][0] != '-')
        steps = atoi(argv[2]);
    g.soundcard = 3;
    g.randSeed = 1234;
    for (i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--soundcard") && i + 1 < argc)
            g.soundcard = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--debugmode") && i + 1 < argc)
            g.debugMode = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            g.randSeed = (uint32_t)strtoul(argv[i + 1], 0, 10);
        else if (!strcmp(argv[i], "--bootparam") && i + 1 < argc)
            g.bootParam = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--save-at") && i + 2 < argc) {
            saveAt = atoi(argv[i + 1]);
            saveFile = argv[i + 2];
        } else if (!strcmp(argv[i], "--load-at") && i + 2 < argc) {
            loadAt = atoi(argv[i + 1]);
            loadFile = argv[i + 2];
        } else if (!strcmp(argv[i], "--load") && i + 2 < argc) {
            loadFile = argv[i + 1];
            loadFrom = atoi(argv[i + 2]);
        }
        else if (!strcmp(argv[i], "--dump-g") && i + 1 < argc)
            dumpG = argv[i + 1];
        else if (!strcmp(argv[i], "--state-of") && i + 1 < argc)
            sscanf(argv[i + 1], "%d:%d", &setStateObj, &setStateVal);
        else if (!strcmp(argv[i], "--copyprot") && i + 1 < argc)
            g.copyProtection = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--opl"))
            oplTrace = 1;
        else if (!strcmp(argv[i], "--verbs"))
            verbLog = 1;
        else if (!strcmp(argv[i], "--state"))
            stateLog = 1;
        else if (!strcmp(argv[i], "--frames"))
            frameMarks = 1;
        else if (!strcmp(argv[i], "--ego"))
            egoLog = 1;
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) {
            shotsFile = fopen(argv[i + 1], "w");
            if (!shotsFile) {
                fprintf(stderr, "cannot write %s\n", argv[i + 1]);
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--palcrc"))
            palCrc = 1;
        else if (!strcmp(argv[i], "--bbcrc")) {
            bbCrc = 1;
            crc_init_table();
        }
        else if (!strcmp(argv[i], "--zcrc"))
            zCrc = 1;
        else if (!strcmp(argv[i], "--fbcrc"))
            fbCrc = 1;
        else if (!strcmp(argv[i], "--sccrc"))
            scCrc = 1;
        else if (!strcmp(argv[i], "--curcrc"))
            curCrc = 1;
        else if (!strcmp(argv[i], "--no-cels"))
            ref_noCels = 1;
        else if (!strcmp(argv[i], "--no-chars"))
            ref_noChars = 1;
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc)
            ppmDir = argv[i + 1];
        else if (!strcmp(argv[i], "--ppm-frames") && i + 1 < argc)
            sscanf(argv[i + 1], "%d-%d", &ppmFirst, &ppmLast);
        else if (!strcmp(argv[i], "--ev") && i + 1 < argc)
            add_event(argv[i + 1]);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
            char line[128];
            FILE *f = fopen(argv[i + 1], "r");
            if (!f) {
                fprintf(stderr, "cannot open %s\n", argv[i + 1]);
                return 1;
            }
            while (fgets(line, sizeof(line), f))
                add_event(line);
            fclose(f);
        }
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
#ifdef WITH_NUKED
            wav = fopen(argv[i + 1], "wb");
            OPL3_Reset(&opl, OPL_RATE);
            wav_header(0);
#else
            fprintf(stderr, "--wav: built without Nuked OPL3\n");
            return 1;
#endif
        }

    load_game(argv[1]);
    g.trace = 1;
    ref_init();
    ref_setPresentHook(present_effect);
    curFrame = -1;                           /* the boot script's effects: before round 0 */
    scumm_init(&img);
    if (setStateObj > 0)                     /* --state-of OBJ:STATE, a test's shortcut */
        putState(setStateObj, setStateVal);
    for (i = loadFile && loadAt < 0 ? loadFrom : 0; i < steps && !g.halted && !g.quit; i++) {
        delta = VAR(VAR_TIMER_NEXT);
        if (delta < 1)
            delta = 1;
        if (g.soundcard)
            advance_audio(delta);
        curFrame = i - 1;                    /* the wait after round i - 1 */
        apply_events(i);
        curFrame = i;
        if (saveFile && i == saveAt)
            g.saveLoadFlag = 1;              /* saved in this round's handleSaveLoad */
        if (loadFile && loadAt < 0 && i == loadFrom)
            g.saveLoadFlag = 2;              /* loaded there: the run goes on from round N */
        if (loadFile && i == loadAt)
            g.saveLoadFlag = 2;
        subFrame = 0;
        if (frameMarks)
            printf("FRAME %d\n", i);
        scummLoop(delta);
        present(0);
        if (bbCrc && g.currentRoom && g.roomImage) {
            uint32_t c = 0xFFFFFFFFu;
            int y, x, rows = scumm_mainScreenHeight();
            for (y = 0; y < rows; y++) {
                const uint8_t *p = ref_backRow(y);
                for (x = 0; x < 320; x++)
                    c = crcTable[(c ^ *p++) & 0xFF] ^ (c >> 8);
            }
            c = ~c;
            if (c != bbLast)
                printf("bb %d crc %x\n", i, c);
            bbLast = c;
        }
        if (zCrc && g.currentRoom && g.roomImage) {
            int n;
            const uint8_t *m = ref_maskArea(&n);
            uint32_t c = crc32_bytes(m, n);
            if (c != zLast)
                printf("z %d crc %x\n", i, c);
            if (g.currentRoom != zRoom)              /* which rooms to check: on stderr */
                fprintf(stderr, "z room %d planes %d\n", g.currentRoom, g.numZBuffer - 1);
            zRoom = g.currentRoom;
            zLast = c;
        }
        if (fbCrc) {
            uint32_t c = 0xFFFFFFFFu;
            int y, x, rows = scumm_mainScreenHeight();
            crc_init_table();
            for (y = 0; y < rows; y++) {
                const uint8_t *p = ref_frontRow(y);
                for (x = 0; x < 320; x++)
                    c = crcTable[(c ^ *p++) & 0xFF] ^ (c >> 8);
            }
            c = ~c;
            if (c != fbLast)
                printf("fb %d crc %x\n", i, c);
            fbLast = c;
        }
        if (scCrc) {
            uint32_t c = crc32_bytes(ref_screen(), 320 * 200);
            if (c != scLast)
                printf("sc %d crc %x\n", i, c);
            scLast = c;
        }
        if (curCrc) {
            const uint8_t *im;
            int w, h, hx, hy, shown = ref_cursor(&im, &w, &h, &hx, &hy);
            uint8_t t[9];
            uint32_t c = 0xFFFFFFFFu;
            int k, mx, my;
            crc_init_table();
            for (k = 0; k < w * h; k++)
                c = crcTable[(c ^ im[k]) & 0xFF] ^ (c >> 8);
            t[0] = (uint8_t)w; t[1] = (uint8_t)h; t[2] = (uint8_t)hx; t[3] = (uint8_t)hy;
            t[4] = (uint8_t)(shown != 0);
            scumm_mousePos(&mx, &my);
            t[5] = (uint8_t)(mx >> 8); t[6] = (uint8_t)mx;
            t[7] = (uint8_t)(my >> 8); t[8] = (uint8_t)my;
            for (k = 0; k < 9; k++)
                c = crcTable[(c ^ t[k]) & 0xFF] ^ (c >> 8);
            c = ~c;
            if (c != curLast)
                printf("cur %d crc %x\n", i, c);
            curLast = c;
        }
        if (ref_paletteChanged() && palCrc)
            printf("pal %d crc %x\n", i, crc32_bytes(ref_palette(), 768));
        if (stateLog)
            fprintf(stderr, "frame %d cam %d msg %d talk %d mask %d\n", i, g.camera.cur_x,
                    g.haveMsg, g.talkDelay, text_hasMask());
        if (egoLog) {
            int k;
            for (k = 1; k < NUM_ACTORS; k++) {
                const Actor *a = &g.actors[k];
                if (a->room != g.currentRoom || !g.currentRoom)
                    continue;
                fprintf(stderr, "frame %d actor %d %d,%d moving %d box %d scale %d facing %d "
                        "dest %d,%d/%d speed %d,%d width %d\n",
                        i, k, a->x, a->y, a->moving, a->walkbox, a->scalex, a->facing,
                        a->walkdata.dest.x, a->walkdata.dest.y, a->walkdata.destbox,
                        a->speedx, a->speedy, a->width);
            }
        }
    }
    if (shotsFile)
        fclose(shotsFile);
    if (wav) {
        wav_header(wavSamples);
        fclose(wav);
    }
    if (verbLog) {                           /* --verbs: where to click, in game pixels */
        int k;
        for (k = 1; k < NUM_VERBS; k++) {
            const VerbSlot *v = &g.verbs[k];
            if (!v->verbid || v->curmode != 1)
                continue;
            fprintf(stderr, "verb %d id %d '%s' rect %d,%d-%d,%d key %d save %d\n", k, v->verbid,
                    (const char *)v->name, v->x, v->y, v->right, v->bottom, v->key, v->saveid);
        }
        for (k = 1; k < NUM_LOCAL_OBJECTS; k++) {
            const ObjectData *od = &g.objs[k];
            const uint8_t *cd;
            if (!od->obj_nr)
                continue;
            cd = getOBCDFromObject(od->obj_nr);
            fprintf(stderr, "object %d rect %d,%d %dx%d state %d walk %d,%d '%s'\n", od->obj_nr,
                    od->x_pos, od->y_pos, od->width, od->height, od->state, od->walk_x,
                    od->walk_y, cd ? (const char *)(cd + cd[18]) : "?");
        }
        for (k = 1; k < NUM_ACTORS; k++) {
            const Actor *a = &g.actors[k];
            if (a->room == g.currentRoom && a->costume)
                fprintf(stderr, "actor %d at %d,%d top %d bottom %d '%s'\n", k, a->x, a->y,
                        (int)a->top, (int)a->bottom, a->name);
        }
    }
    if (dumpG) {
        FILE *f = fopen(dumpG, "wb");
        if (f) {
            fwrite(&g, sizeof(g), 1, f);
            fclose(f);
        }
    }
    if (g.halted)
        printf("HALT: %s (step %d)\n", g.haltMsg, i);
    if (g.quit)
        printf("QUIT (step %d)\n", i - 1);
    return 0;
}
