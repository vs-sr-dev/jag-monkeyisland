/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Samuele Voltan.  See LICENSE. */
/*
 * jag_main.c - the interpreter on the Jaguar's 68000: the platform half of csrc/scumm/.
 *
 * M2a, the first hardware slice: the interpreter alone, without drawing, input or sound,
 * started as the oracle starts ScummVM (boot param BOOT_PARAM, no music, debug mode 1, random
 * seed 1234), in two phases:
 *
 *   1. TRACE: TRACE_ROUNDS loop rounds as fast as the 68000 goes, the opcode trace on.  Every
 *      line goes into a running CRC32 (the line and a '\n', as zlib.crc32); every
 *      TRACE_EVERY lines COM8 gets "T <lines> <crc>", which tools/trace_crc.py compares with
 *      the same sum over ScummVM's trace.  The whole trace on COM8 would cost ~2 ms a line.
 *   2. TIME: the interpreter started again, trace off, in real time: one loop round every
 *      VAR_TIMER_NEXT video frames, as ScummVM's main loop.  Every TIME_EVERY rounds COM8
 *      gets the average and the worst round in half-lines (NTSC: 525 a frame, ~31.8 us each),
 *      and the average of each section of scummLoop (PROF marks in loop.c) in tenths;
 *      TIME_REPORTS times, then back to the GameDrive's stub (back_to_stub), so that the
 *      next upload needs no power cycle.
 *
 * The game image is the cartridge (ROM_GAME, tools/mi2jag.py): image.c keeps pointers into
 * it, and the interpreter reads every resource in place.  The picture is jag_gfx.c's; this
 * file keeps the GPU's command queue both use, the log, the time and the entry points.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "jag.h"

#define REG_VC  0xF00006UL

/* TRACE_ROUNDS: mem_map.h (tools/memory_map.py, JMI_TRACE; 900 = 90 s of the game) */
#define TRACE_EVERY  256
#define TIME_EVERY   100
#define TIME_REPORTS 4              /* then Reset(2): back to the stub */

/* ---- libc, the part the interpreter uses -------------------------------------------- */

void *memset(void *d, int c, size_t n)
{
    uint8_t *p = d;
    while (n--)
        *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *p = d;
    const uint8_t *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *p = d;
    const uint8_t *q = s;
    if (p <= q || p >= q + n) {
        while (n--)
            *p++ = *q++;
    } else {
        p += n;
        q += n;
        while (n--)
            *--p = *--q;
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p < *q ? -1 : 1;
    return 0;
}

static char *put_u(char *o, char *end, unsigned long v, int base, int upper)
{
    char t[12];
    int n = 0;
    do {
        int d = (int)(v % (unsigned)base);
        t[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= (unsigned)base;
    } while (v);
    while (n && o < end)
        *o++ = t[--n];
    return o;
}

/* %d %u %x %X %s %c (and l), what the trace and the halts print */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    char *o = buf, *end = buf + size - 1;
    if (!size)
        return 0;
    while (*fmt && o < end) {
        if (*fmt != '%') {
            *o++ = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == 'l')
            fmt++;
        switch (*fmt) {
        case 'd': {
            long v = __builtin_va_arg(ap, int);
            if (v < 0) {
                *o++ = '-';
                v = -v;
            }
            o = put_u(o, end, (unsigned long)v, 10, 0);
            break;
        }
        case 'u': o = put_u(o, end, __builtin_va_arg(ap, unsigned), 10, 0); break;
        case 'x': o = put_u(o, end, __builtin_va_arg(ap, unsigned), 16, 0); break;
        case 'X': o = put_u(o, end, __builtin_va_arg(ap, unsigned), 16, 1); break;
        case 'c': *o++ = (char)__builtin_va_arg(ap, int); break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            while (*s && o < end)
                *o++ = *s++;
            break;
        }
        default: *o++ = *fmt; break;
        }
        if (*fmt)
            fmt++;
    }
    *o = 0;
    return (int)(o - buf);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    int n;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    int n;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    n = vsnprintf(buf, 256, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

/* ---- COM8 ---------------------------------------------------------------------------- */

/* COM8, or without a GameDrive (BigPEmu) the EMU_LOG region, which tools/emu_log.py saves
 * once EMU_END says the run is over: the same lines either way. */
static void log_line(const char *s)
{
    uint32_t fn = HW32(GD_SEND_FN);
    if (fn)
        ((void (*)(const char *))fn)(s);
    else {
        uint32_t n = HW32(EMU_LOG_N);
        uint8_t *p = (uint8_t *)EMU_LOG;
        while (*s && n < EMU_LOG_SIZE - 1)
            p[n++] = (uint8_t)*s++;
        if (n < EMU_LOG_SIZE)
            p[n++] = '\n';
        HW32(EMU_LOG_N) = n;
    }
}

void jag_logf(const char *fmt, ...)
{
    char buf[128];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    log_line(buf);
}

/* ---- time: half-lines since boot (src/main.s TimeNow, in C) --------------------------- */

uint32_t jag_time(void)
{
    uint32_t t0, t1, hl = HW16(VID_NTSC) ? 525 : 625;
    int vc;
    do {
        t0 = HW32(IRQ_TICK);
        vc = HW16(REG_VC) & 0x7FF;
        t1 = HW32(IRQ_TICK);
    } while (t0 != t1);
    vc -= HW16(VI_VAL);
    if (vc < 0)
        vc += (int)hl;
    {
        /* Between VC reaching VI and the ISR counting IRQ_TICK the phase has restarted but
         * the tick has not: a reading there falls a frame behind.  Monotonic, then: a step
         * back of less than a frame is that, and returns the last time. */
        static uint32_t last;
        uint32_t t = t0 * hl + (uint32_t)vc;
        if (t < last && last - t < hl)
            return last;
        last = t;
        return t;
    }
}

/* ---- the trace's running CRC32 (zlib's) ----------------------------------------------- */

static uint32_t crcTable[256];
static uint32_t traceCrc, traceLines;

static void crc_init(void)
{
    uint32_t c, n, k;
    for (n = 0; n < 256; n++) {
        c = n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
        crcTable[n] = c;
    }
}

uint32_t jag_crc_more(uint32_t c, const uint8_t *p, uint32_t n)
{
    while (n--)
        c = crcTable[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c;
}

uint32_t jag_crc(const uint8_t *p, uint32_t n)
{
    return ~jag_crc_more(0xFFFFFFFFUL, p, n);
}

static void crc_add(const char *s)
{
    uint32_t c = ~traceCrc;
    while (*s)
        c = crcTable[(c ^ (uint8_t)*s++) & 0xFF] ^ (c >> 8);
    c = crcTable[(c ^ '\n') & 0xFF] ^ (c >> 8);
    traceCrc = ~c;
}

/* ---- the platform hooks -------------------------------------------------------------- */

void plat_trace(const char *line)
{
    crc_add(line);
    traceLines++;
    if (traceLines <= 4)                     /* the format, by eye */
        jag_logf("L %s", line);
    if (traceLines % TRACE_EVERY == 0)
        jag_logf("T %u %x", (unsigned)traceLines, (unsigned)traceCrc);
}

/* scummLoop's sections (PROF in loop.c): time from mark n to the next mark goes to n */
#define PROF_N 18
static uint32_t profSum[PROF_N], profAt;
static int profLast = -1;

void plat_prof(int section)
{
    uint32_t t = jag_time();
    if (profLast >= 0)
        profSum[profLast] += t - profAt;
    profAt = t;
    profLast = section == 11 ? -1 : section;
}

static void prof_report(uint32_t n)
{
    char buf[128], *o;
    int i, half;
    for (half = 0; half < 2; half++) {          /* two lines: COM8 lines stay short */
        o = buf;
        o += snprintf(o, sizeof(buf), "prof%d x0.1hl:", half);
        for (i = half * 11; i < PROF_N && i < half * 11 + 11; i++) {
            o += snprintf(o, (size_t)(buf + sizeof(buf) - o), " %u",
                          (unsigned)(profSum[i] * 10 / n));
            profSum[i] = 0;
        }
        log_line(buf);
    }
}

/* The GD BIOS's Reset(2): the Jaguar goes back to the GameDrive's debug stub (the RetroHQ
 * logo), from which the next USB reboot and upload always go through.  A ROM that stays in
 * its own loop leaves the GameDrive deaf to them until a power cycle (JaguarDocs
 * metodi/gamedrive_usb.md 3.3 and 6.10).  No BIOS: park. */
static void gpu_stop(void);

static void back_to_stub(void)
{
    uint32_t base = HW32(GD_BIOS);
    gpu_stop();
    HW32(EMU_END) = EMU_END_MAGIC;
    if (base)
        __asm__ volatile("move.w #0x2700,%%sr\n\t"
                         "move.w #2,%%d0\n\t"
                         "movea.l %0,%%a6\n\t"
                         "jsr 96(%%a6)"
                         : : "a"(base) : "d0", "d1", "a0", "a1", "a6", "memory", "cc");
    for (;;)
        ;
}

void plat_error(const char *msg)
{
    jag_logf("FATAL: %s", msg);
    back_to_stub();
}


/* ---- waiting: the 68000 waits only when it must (the queue full, a colour table to rewrite,
 * a hand-over not taken yet), off the bus, with a ceiling.  The time it waits is counted for
 * the time phase's report. ---- */

static uint32_t gpuWait;                    /* half-lines waited since the last report */

/* A pause that stays off the bus: the 68000 polling DRAM in a tight loop slows the GPU's DRAM
 * loads ~3x (JaguarDocs hw/01 s.6.1).  JagSkyroads, JagGOT. */
static inline void bus_pause(void)
{
    uint32_t a = 0, b = 0xFFFFu;
    __asm__ volatile("mulu.w %1,%0\n\tmulu.w %1,%0\n\tmulu.w %1,%0" : "+d"(a) : "d"(b));
}

void jag_wait_until(int (*done)(void), const char *what)
{
    uint32_t t0, guard = 0;
    if (done())
        return;
    t0 = jag_time();
    while (!done()) {
        bus_pause();
        if (++guard > 150000UL) {           /* ~3 s */
            HW32(GPU_FAIL) = HW32(GPU_FAIL) + 1;
            plat_error(what);
        }
    }
    gpuWait += jag_time() - t0;
}

/* ---- the GPU's command queue (src/gpu_room.s): GQ_N commands of 8 longs in GQ_RING.  The
 * 68000 writes a command, then GQ_HEAD + 1; the GPU does it, then GQ_TAIL + 1.  Both count
 * modulo 65536 in the low word of their cell (the high word stays 0: no torn reads). ---- */

#define G_CTRL_R  0xF02114UL

static uint32_t gqHead;                     /* GQ_HEAD's value: only this file writes it */

uint32_t gq_head(void)
{
    return gqHead;
}

static uint32_t gq_tail(void)
{
    return HW32(GQ_TAIL) & 0xFFFFu;
}

/* whether the GPU has done every command up to the count mark (a gq_head() value) */
int gq_passed(uint32_t mark)
{
    return ((gq_tail() - mark) & 0xFFFFu) < 0x8000u;
}

static int gq_has_room(void)
{
    return ((gqHead - gq_tail()) & 0xFFFFu) < GQ_N;
}

static int gq_empty(void)
{
    return gq_tail() == gqHead;
}

/* the next free command, waiting for one if the ring is full */
volatile uint32_t *gq_slot(void)
{
    jag_wait_until(gq_has_room, "GPU queue: the GPU does not move (full)");
    return (volatile uint32_t *)(GQ_RING + (gqHead & (GQ_N - 1)) * GQ_CMD);
}

void gq_commit(void)
{
    gqHead = (gqHead + 1) & 0xFFFFu;
    HW32(GQ_HEAD) = gqHead;
}

void gq_drain(void)
{
    jag_wait_until(gq_empty, "GPU queue: the GPU does not move (drain)");
}

/* Before the GameDrive's Reset(2): the GPU stops itself (GQ_STOP), or is stopped. */
static void gpu_stop(void)
{
    static int stopping;
    uint32_t i;
    if (stopping)
        return;
    stopping = 1;
    if (gq_has_room()) {
        volatile uint32_t *c = (volatile uint32_t *)(GQ_RING + (gqHead & (GQ_N - 1)) * GQ_CMD);
        c[0] = GQ_STOP;
        gq_commit();
        for (i = 0; i < 100000UL && !HW32(GPU_DONE); i++)
            bus_pause();
    }
    HW32(G_CTRL_R) = 0;
    (void)HW32(G_CTRL_R);
}

/* ---- the game image, from the cartridge ----------------------------------------------- */

static GameImage img;

static const uint8_t *game_file(int i, uint32_t *size)
{
    const uint8_t *g0 = (const uint8_t *)ROM_GAME;
    const uint8_t *e = g0 + 8 + 8 * i;
    uint32_t ofs = ((uint32_t)e[0] << 24) | ((uint32_t)e[1] << 16) | ((uint32_t)e[2] << 8) | e[3];
    uint32_t n = ((uint32_t)e[4] << 24) | ((uint32_t)e[5] << 16) | ((uint32_t)e[6] << 8) | e[7];
    *size = n;
    return n ? g0 + ofs : 0;
}

static int load_image(void)
{
    const uint8_t *g0 = (const uint8_t *)ROM_GAME;
    uint32_t size;
    const uint8_t *p;
    int d;
    if (g0[0] != 'M' || g0[1] != 'I' || g0[2] != 'G' || g0[3] != 'M')
        return 0;
    p = game_file(0, &size);
    image_index(&img, p, size);
    for (d = 1; d <= 4; d++) {
        p = game_file(d, &size);
        image_disk(&img, d, p, size);
    }
    {   /* the charsets into DRAM: the GPU reads the glyphs a byte at a time (GQ_CHAR) */
        uint8_t *to = (uint8_t *)CHARSETS;
        for (d = 1; d <= 4; d++) {
            p = game_file(4 + d, &size);
            img.charset[d] = 0;
            if (!p)
                continue;
            if (to + size > (uint8_t *)CHARSETS + CHARSETS_SIZE)
                plat_error("the charsets do not fit CHARSETS");
            memcpy(to, p, size);
            img.charset[d] = to;
            to += (size + 3) & ~3UL;
        }
    }
    return 1;
}

static void mouse_cells(void);
#if PLAY
static uint32_t padA, padB, padC, padO;     /* the presses counted when last looked at */
static uint32_t keyqTail;                    /* the keypad presses taken (KEYQ_HEAD: the ISR's) */
static int padADown, padBDown, padX = -1, padY = -1;
#endif

static void start(int trace)
{
    g.trace = trace;
    g.soundcard = (int)SOUNDCARD;            /* 0: the oracle's music_driver=null */
    g.debugMode = (int)DEBUG_MODE;           /* 1: as ScummVM with a debug level */
    g.copyProtection = (int)COPY_PROT;       /* 1: the Dial-A-Pirate wheel */
    g.bootParam = (int)BOOT_PARAM;
    g.randSeed = 1234;                       /* --random-seed=1234 */
    gfx_start(trace);
    audio_start(trace ? 1 : 2);              /* before scumm_init: its sound_init writes */
    scumm_init(&img);
#if PLAY
    {   /* the pad starts where the game puts the mouse */
        int x, y;
        scumm_mousePos(&x, &y);
        padX = x;
        padY = y;
        HW32(MOUSE_X) = (uint32_t)x;          /* before the interrupt moves it: no race, */
        HW32(MOUSE_Y) = (uint32_t)y;          /* MovePad only adds to it */
        padA = HW32(PAD_ADOWN);
        padB = HW32(PAD_BDOWN);
        padC = HW32(PAD_CDOWN);
        padO = HW32(PAD_ODOWN);
        keyqTail = HW32(KEYQ_HEAD);
    }
#else
    mouse_cells();
#endif
}

/* ---- the input the ROM replays (tools/input_table.py, JMI_INPUT): the events of the wait
 * before round n, applied before it, as scumm_pc --input does (pc_main.c apply_events) ---- */
#include "input_table.h"
static unsigned inputNext;

/* the mouse where the cursor is drawn (MOUSE_X/Y): the main loop's cells unless PLAY */
static void mouse_cells(void)
{
#if !PLAY
    int x, y;
    scumm_mousePos(&x, &y);
    HW32(MOUSE_X) = (uint32_t)x;
    HW32(MOUSE_Y) = (uint32_t)y;
#endif
}

static void apply_events(uint32_t round)
{
    while (inputEvents[inputNext].round <= round) {     /* the last entry's round: 0xFFFF */
        if (inputEvents[inputNext].round == round) {
            switch (inputEvents[inputNext].kind) {
            case 0:
                scumm_mouseMove(inputEvents[inputNext].a, inputEvents[inputNext].b);
                mouse_cells();
                break;
            case 1: scumm_mouseDown(inputEvents[inputNext].a); break;
            case 2: scumm_mouseUp(inputEvents[inputNext].a); break;
            case 3: scumm_keyPress(inputEvents[inputNext].a); break;
            case 4: {
                const int ascii = scumm_keypadKey(inputEvents[inputNext].a);
                if (ascii)
                    scumm_keyPress(ascii);
                break;
            }
            }
        }
        inputNext++;
    }
}

/* PLAY: the pad, before each round, as ScummVM's parseEvents before it: the mouse where the
 * video interrupt has moved it (MovePad), A the left button, B the right one (presses counted
 * by the interrupt: a short one is not lost; released when the button is up), C the '.' key
 * (the line being said ends), OPTION the ESC key (the cutscene's override); '*' and '#'
 * together: back to the GameDrive's stub (the instruments are on the keypad). */
#if PLAY
static void back_to_stub(void);

/* Every input given to the game, on COM8 as "I " + a line of scumm_pc --input (the round it
 * is applied before; a banner's at the round after its own, as plat_waitBannerInput takes
 * it): a play-through that stops can be replayed on the PC (grep "^ *[0-9.]* I " LOG). */
static uint32_t inRound;
static uint32_t pauseDownAt;                /* IRQ_TICK when PAUSE went down (| 1), 0: up */
static const char keypadChar[12] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '*', '#' };

static void pad_input(void)
{
    const uint32_t a = HW32(PAD_ADOWN), b = HW32(PAD_BDOWN);
    const unsigned st = HW16(PAD_STATE), keys = HW16(PAD_KEYS);
    const int x = (int)HW32(MOUSE_X), y = (int)HW32(MOUSE_Y);
    if ((keys & (1u << KEY_B_STAR)) && (keys & (1u << KEY_B_HASH))) {
        jag_logf("PLAY: '*' + '#', back to the GameDrive's stub");
        back_to_stub();
    }
    if (x != padX || y != padY) {
        scumm_mouseMove(x, y);
        jag_logf("I %u move %d %d", (unsigned)inRound, x, y);
    }
    padX = x;
    padY = y;
    if (a != padA) {
        scumm_mouseDown(0);
        jag_logf("I %u down L", (unsigned)inRound);
        padADown = 1;
    } else if (padADown && !(st & (1u << PAD_B_A))) {
        scumm_mouseUp(0);
        jag_logf("I %u up L", (unsigned)inRound);
        padADown = 0;
    }
    if (b != padB) {
        scumm_mouseDown(1);
        jag_logf("I %u down R", (unsigned)inRound);
        padBDown = 1;
    } else if (padBDown && !(st & (1u << PAD_B_B))) {
        scumm_mouseUp(1);
        jag_logf("I %u up R", (unsigned)inRound);
        padBDown = 0;
    }
    padA = a;
    padB = b;
    /* the keys: C is '.', OPTION is ESC (one key a round, as ScummVM's _keyPressed) */
    if (HW32(PAD_CDOWN) != padC) {
        scumm_keyPress('.');
        jag_logf("I %u key 46", (unsigned)inRound);
    } else if (HW32(PAD_ODOWN) != padO) {
        scumm_keyPress(27);
        jag_logf("I %u key 27", (unsigned)inRound);
    } else if (HW32(KEYQ_HEAD) != keyqTail) {
        /* the keypad: the key of the verb at the key's place (keypad.c), as the game's own
         * keyboard shortcut; the presses wait in the queue, one a round */
        const int k = HW8(KEYQ + (keyqTail & 15));
        const int ascii = scumm_keypadKey(k);
        if ((st & (1u << PAD_B_PAUSE)) && (k == 1 || k == 2)) {
            /* the save's shortcut until the original's menu: PAUSE held + 1 saves to slot 0,
             * + 2 loads it (at this round's handleSaveLoad); the key is not the game's, and
             * PAUSE's release is no pause */
            keyqTail++;
            g.saveLoadFlag = k;
            pauseDownAt = (HW32(IRQ_TICK) - 1000) | 1;
            jag_logf("PLAY: %s slot 0 (round %u)", k == 1 ? "save to" : "load from",
                     (unsigned)inRound);
            return;
        }
        jag_logf("I %u keypad %c", (unsigned)inRound, keypadChar[k < 12 ? k : 0]);
        keyqTail++;
        if (ascii)
            scumm_keyPress(ascii);
    }
    padC = HW32(PAD_CDOWN);
    padO = HW32(PAD_ODOWN);
    /* PAUSE: let go within a second, the game's pause key (VAR_PAUSE_KEY: ' ' in MI1, the
     * "Game Paused" banner); held longer, the menu (to come) */
    if (st & (1u << PAD_B_PAUSE)) {
        if (!pauseDownAt)
            pauseDownAt = HW32(IRQ_TICK) | 1;
    } else if (pauseDownAt) {
        if (HW32(IRQ_TICK) - pauseDownAt < 60 && VAR(VAR_PAUSE_KEY)) {
            scumm_keyPress(VAR(VAR_PAUSE_KEY));
            jag_logf("I %u key %d", (unsigned)inRound, (int)VAR(VAR_PAUSE_KEY));
        }
        pauseDownAt = 0;
    }
}
#endif

/* waitForBannerInput(-1) (input.cpp): the banner shown, then the first key or click.  The
 * replayed input (not PLAY): the events after the round's own, as scumm_pc's
 * plat_waitBannerInput.  PLAY: the pad, frame by frame; the cursor animates as ScummVM's
 * banner loop does it (every 17 of its 4 ms waits: every 4 frames here) and follows the
 * D-pad; A or B is a click (0), C '.', OPTION ESC, a keypad key its ASCII (keypad.c). */
int plat_waitBannerInput(void)
{
    gfx_present();
#if PLAY
    {
        uint32_t tick = HW32(IRQ_TICK), frames = 0;
        for (;;) {
            const unsigned keys = HW16(PAD_KEYS);
            int x, y;
            while (HW32(IRQ_TICK) == tick)
                ;
            tick = HW32(IRQ_TICK);
            if ((keys & (1u << KEY_B_STAR)) && (keys & (1u << KEY_B_HASH))) {
                jag_logf("PLAY: '*' + '#', back to the GameDrive's stub");
                back_to_stub();
            }
            x = (int)HW32(MOUSE_X);
            y = (int)HW32(MOUSE_Y);
            if (x != padX || y != padY) {
                scumm_mouseMove(x, y);
                jag_logf("I %u move %d %d", (unsigned)inRound + 1, x, y);
            }
            padX = x;
            padY = y;
            if (HW32(PAD_ADOWN) != padA || HW32(PAD_BDOWN) != padB) {
                jag_logf("I %u down %c", (unsigned)inRound + 1, HW32(PAD_ADOWN) != padA ? 'L' : 'R');
                padADown |= HW32(PAD_ADOWN) != padA;      /* released later, as any click */
                padBDown |= HW32(PAD_BDOWN) != padB;
                padA = HW32(PAD_ADOWN);
                padB = HW32(PAD_BDOWN);
                return 0;
            }
            if (HW32(PAD_CDOWN) != padC) {
                padC = HW32(PAD_CDOWN);
                jag_logf("I %u key 46", (unsigned)inRound + 1);
                return '.';
            }
            if (HW32(PAD_ODOWN) != padO) {
                padO = HW32(PAD_ODOWN);
                jag_logf("I %u key 27", (unsigned)inRound + 1);
                return 27;
            }
            if (HW16(PAD_STATE) & (1u << PAD_B_PAUSE)) {   /* a key too: the banner goes */
                if (!pauseDownAt) {
                    pauseDownAt = (HW32(IRQ_TICK) - 1000) | 1;   /* used: its release no key */
                    jag_logf("I %u key 32", (unsigned)inRound + 1);
                    return 32;
                }
            } else {
                pauseDownAt = 0;
            }
            if (HW32(KEYQ_HEAD) != keyqTail) {
                const int k = HW8(KEYQ + (keyqTail & 15));
                const int ascii = scumm_keypadKey(k);
                jag_logf("I %u keypad %c", (unsigned)inRound + 1, keypadChar[k < 12 ? k : 0]);
                keyqTail++;
                return ascii;
            }
            if (++frames == 4) {
                frames = 0;
                animateCursor();
                gfx_present();
            }
        }
    }
#else
    for (;; inputNext++) {
        if (inputEvents[inputNext].round == 0xFFFF)
            return 0;                        /* none left: as a click */
        switch (inputEvents[inputNext].kind) {
        case 0:
            scumm_mouseMove(inputEvents[inputNext].a, inputEvents[inputNext].b);
            mouse_cells();
            continue;
        case 1:
            inputNext++;
            return 0;
        case 3:
            return inputEvents[inputNext++].a;
        case 4: {
            const int ascii = scumm_keypadKey(inputEvents[inputNext].a);
            if (!ascii)
                continue;
            inputNext++;
            return ascii;
        }
        default:
            continue;
        }
    }
#endif
}

/* _system->warpMouse (a load): the cursor where the game's mouse is */
void plat_warpMouse(int x, int y)
{
    HW32(MOUSE_X) = (uint32_t)x;
    HW32(MOUSE_Y) = (uint32_t)y;
#if PLAY
    padX = x;
    padY = y;
#endif
}

static long gd_call(long fn, long d0, long d1, void *a0);
enum { GD_FOPEN = 10, GD_FCLOSE = 11, GD_FSEEK = 12, GD_FREAD = 13, GD_FWRITE = 14,
       GD_FSIZE = 16 };

/* scummLoop_handleSaveLoad: the save as a file on the GameDrive's SD card (GD BIOS FOpen
 * $0A = write | create always, FWrite, FClose; FOpen $01, FSize, FRead), raw for now.  After
 * an FRead the GameDrive refuses the next USB upload until a power cycle (gamedrive_usb.md
 * 3.5): a development matter, not a player's. */
#define SAVE_NAME "/JMISAVE0.SAV"
static int saveLoadFile(int flag);

/* the music held while the 68000 talks to the SD card (as a banner holds it) */
int plat_saveLoad(int flag)
{
    int ok;
    plat_pauseEngine(1);
    ok = saveLoadFile(flag);
    plat_pauseEngine(0);
    return ok;
}

static int saveLoadFile(int flag)
{
    uint8_t *buf = (uint8_t *)SAVE_BUF;
    const uint32_t t = jag_time();
    uint32_t n;
    long h, r;
    if (flag == 1) {
        n = scumm_save(buf, SAVE_BUF_SIZE, (uint8_t *)SAVE_RAW, SAVE_RAW_SIZE);
        if (!n) {
            jag_logf("save: does not fit SAVE_BUF");
            return 0;
        }
        h = gd_call(GD_FOPEN, 0x0A, 0, (void *)SAVE_NAME);
        if ((int16_t)h < 0) {
            jag_logf("save: FOpen %d", (int)(int16_t)h);
            return 0;
        }
        r = gd_call(GD_FWRITE, h & 0xFFFF, (long)n, buf);
        gd_call(GD_FCLOSE, h & 0xFFFF, 0, 0);
        jag_logf("save: %u bytes, crc %x, FWrite %d, %u half-lines", (unsigned)n,
                 (unsigned)jag_crc(buf, n), (int)r, (unsigned)(jag_time() - t));
        if (r != 0)
            return 0;
    } else {
        h = gd_call(GD_FOPEN, 0x01, 0, (void *)SAVE_NAME);
        if ((int16_t)h < 0) {
            jag_logf("load: no save (FOpen %d)", (int)(int16_t)h);
            return 0;
        }
        n = (uint32_t)gd_call(GD_FSIZE, h & 0xFFFF, 0, 0);
        if (n == 0 || n > SAVE_BUF_SIZE) {
            gd_call(GD_FCLOSE, h & 0xFFFF, 0, 0);
            jag_logf("load: size %u", (unsigned)n);
            return 0;
        }
        r = gd_call(GD_FREAD, h & 0xFFFF, (long)n, buf);
        gd_call(GD_FCLOSE, h & 0xFFFF, 0, 0);
        if (r != 0 || !scumm_load(buf, n, (uint8_t *)SAVE_RAW, SAVE_RAW_SIZE)) {
            jag_logf("load: FRead %d, or not a save of this game", (int)r);
            return 0;
        }
        jag_logf("load: %u bytes, crc %x, %u half-lines", (unsigned)n, (unsigned)jag_crc(buf, n),
                 (unsigned)(jag_time() - t));
    }
    jag_logf("SAVELOAD");                    /* scumm_pc's trace marker; not in the CRC */
    return 1;
}

static int round_delta(void)
{
    int delta = VAR(VAR_TIMER_NEXT);
    return delta < 1 ? 1 : delta;
}

/* ---- the entry points ---------------------------------------------------------------- */

long c_init(void)
{
    crc_init();
    return (long)C_MAGIC;
}

/* ---- the GameDrive's SD card (GD BIOS functions 10-16, JaguarDocs metodi/gamedrive_usb.md
 * 3.3), through src/gd.s GdCallC ---- */
static long gd_call(long fn, long d0, long d1, void *a0)
{
    const uint32_t f = HW32(GD_CALL_FN);
    if (!f)
        return -1;
    return ((long (*)(long, long, long, void *))f)(fn, d0, d1, a0);
}

#if SD_TEST
/* FWrite never tried before (T-GD-09): a file of 64 KB known written, its size read back;
 * with SD_TEST 2 its bytes too (FRead).  Every step logged with its time. */
static void sd_test(void)
{
    static const char name[] = "/JMITEST.BIN";
    static const uint16_t modes[3] = { 0x0A, 0x12, 0x02 };   /* FatFs: WRITE | CREATE_ALWAYS,
                                                               WRITE | OPEN_ALWAYS, WRITE */
    uint8_t *buf = (uint8_t *)SAVE_BUF;
    const uint32_t n = 65536;
    uint32_t i, crc, t;
    long h = -1, r;
    int m;
    for (i = 0; i < n; i++)
        buf[i] = (uint8_t)((i * 7) ^ (i >> 8));
    crc = jag_crc(buf, n);
    jag_logf("SD: GD BIOS %x, call %x; the file %s, %u bytes, crc %x",
             (unsigned)HW32(GD_BIOS), (unsigned)HW32(GD_CALL_FN), name, (unsigned)n, (unsigned)crc);
    for (m = 0; m < 3; m++) {
        t = jag_time();
        h = gd_call(GD_FOPEN, modes[m], 0, (void *)name);
        jag_logf("SD: FOpen mode %x -> %d (%u hl)", (unsigned)modes[m], (int)(int16_t)h,
                 (unsigned)(jag_time() - t));
        if ((int16_t)h >= 0)
            break;
    }
    if ((int16_t)h < 0) {
        jag_logf("SD: no mode opened the file for writing");
        return;
    }
    t = jag_time();
    r = gd_call(GD_FWRITE, h & 0xFFFF, (long)n, buf);
    jag_logf("SD: FWrite %u bytes -> %d (%u hl)", (unsigned)n, (int)r, (unsigned)(jag_time() - t));
    t = jag_time();
    r = gd_call(GD_FCLOSE, h & 0xFFFF, 0, 0);
    jag_logf("SD: FClose -> %d (%u hl)", (int)r, (unsigned)(jag_time() - t));
    h = gd_call(GD_FOPEN, 0x01, 0, (void *)name);
    jag_logf("SD: FOpen read -> %d", (int)(int16_t)h);
    if ((int16_t)h < 0)
        return;
    r = gd_call(GD_FSIZE, h & 0xFFFF, 0, 0);
    jag_logf("SD: FSize -> %u (%s)", (unsigned)r, (uint32_t)r == n ? "as written" : "NOT as written");
    if (SD_TEST >= 2) {
        memset(buf, 0, n);
        t = jag_time();
        r = gd_call(GD_FREAD, h & 0xFFFF, (long)n, buf);
        jag_logf("SD: FRead -> %d (%u hl), crc %x (%s)", (int)r, (unsigned)(jag_time() - t),
                 (unsigned)jag_crc(buf, n), jag_crc(buf, n) == crc ? "as written" : "NOT as written");
    }
    gd_call(GD_FCLOSE, h & 0xFFFF, 0, 0);
    jag_logf("SD: done%s", SD_TEST >= 2 ? " (FRead used: power cycle before the next upload)" : "");
}
#endif

void c_main(void)
{
    uint32_t t0, t1, i;
#if SD_TEST
    sd_test();
    back_to_stub();
#endif
    if (!load_image())
        plat_error("no game image at ROM_GAME");
    jag_logf("M2a: interpreter on the 68000, boot param %d", (int)BOOT_PARAM);

    /* 1. the trace, flat out (not when PLAY: the game played) */
    if (PLAY)
        goto play;
    start(1);
    t0 = jag_time();
    inputNext = 0;
    for (i = 0; i < TRACE_ROUNDS && !g.halted && !g.quit; i++) {
        const int delta = round_delta();
        audio_trace_round(delta);            /* the driver's ticks of the wait, as scumm_pc */
        apply_events(i);
        if ((int)i == (int)SAVE_AT)           /* scumm_pc --save-at / --load-at */
            g.saveLoadFlag = 1;
        if ((int)i == (int)LOAD_AT)
            g.saveLoadFlag = 2;
        scummLoop(delta);
    }
    t1 = jag_time();
    audio_trace_end();
    jag_logf("T %u %x end", (unsigned)traceLines, (unsigned)traceCrc);
    jag_logf("trace: %u rounds in %u half-lines%s%s%s", (unsigned)i, (unsigned)(t1 - t0),
         g.halted ? ", HALT: " : "", g.halted ? g.haltMsg : "", g.quit ? ", QUIT" : "");

    /* 2. real time, trace off */
play:
    start(0);
    inputNext = 0;
    memset(profSum, 0, sizeof(profSum));
    gpuWait = 0;
    {
        uint32_t next = HW32(IRQ_TICK), sum = 0, worst = 0, late = 0, n = 0, rounds = 0;
        for (;;) {
            uint32_t a, b, d;
            int delta = round_delta();
            /* ScummVM's waitForTimer: wait until the round's due time; the next one is due
             * delta frames after this one's due time (a small lag is made up: an even pace),
             * unless the round before ran more than 50 ms past it (a room entered): then from
             * now, not made up (the rounds after it would run back to back: the animations
             * of a new room raced, seen by the user) */
            const uint32_t lagMax = HW16(VID_NTSC) ? 3 : 2;      /* 50 ms in frames */
            uint32_t now;
            while ((int32_t)(HW32(IRQ_TICK) - next) < 0)
                ;
            now = HW32(IRQ_TICK);
            if ((int32_t)(now - next) > 0)
                late++;                      /* the round before ran past its frames */
            if ((int32_t)(now - next) > (int32_t)lagMax)
                next = now;
            next += (uint32_t)delta;
#if PLAY
            inRound = rounds;
            pad_input();
#else
            apply_events(rounds);            /* the same input again, in real time */
#endif
            a = jag_time();
            scummLoop(delta);
            b = jag_time();
            d = b - a;
            sum += d;
            if (d > worst)
                worst = d;
            n++;
            rounds++;
            if (g.halted) {
                jag_logf("HALT: %s (round %u)", g.haltMsg, (unsigned)rounds);
                back_to_stub();
            }
            if (g.quit) {                    /* the game quit (o5_systemOps): as DOS's prompt */
                jag_logf("QUIT (round %u)", (unsigned)rounds);
                back_to_stub();
            }
            if (n == TIME_EVERY) {
                jag_logf("time: rounds %u avg %u worst %u half-lines, late %u, room %d, "
                     "gpu-wait %u, gpu fail %u stall %u",
                     (unsigned)rounds, (unsigned)(sum / n), (unsigned)worst, (unsigned)late,
                     (int)g.currentRoom, (unsigned)(gpuWait / n), (unsigned)HW32(GPU_FAIL),
                     (unsigned)HW32(GPU_STALL));
                jag_logf("spies: op-miss rows %u (most in a frame %u), isr late at most %u hl, "
                         "list armed at most %u hl after VI",
                         (unsigned)HW32(OP_MISS), (unsigned)HW16(OP_MISSMAX),
                         (unsigned)HW16(ISR_LATE), (unsigned)HW16(ISR_END));
                gpuWait = 0;
                audio_report();
                prof_report(n);
                gfx_report(n);
                sum = worst = n = 0;
                if (!PLAY && rounds >= TIME_EVERY * TIME_REPORTS) {
                    jag_logf("M2a: done, back to the GameDrive's stub");
                    back_to_stub();
                }
            }
        }
    }
}
