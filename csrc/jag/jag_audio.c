/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Samuele Voltan.  See LICENSE. */
/*
 * jag_audio.c - Monkey Island's AdLib driver on the Jaguar (the port's own).
 *
 * The driver is the original one, ScummVM's Player_AD (csrc/scumm/player_ad.c); the chip is a
 * YM3812 synthesised by the DSP (src/dsp_audio.s, JagSkyroads' and JagGOT's).  Between them, a
 * ring of 16-bit stream words in DRAM (AD_RING), JagGOT's stream grammar: reg << 8 | val a
 * write, $0001 the end of a tick, $FF00 back to the ring's start.  The DSP is the clock: it
 * plays 472 ticks a second counted in samples, stops where the ring has been written to
 * (MUS_WR) and counts the ticks played (MUS_PLAYED).
 *
 * Two ways of running the driver, as the two phases of jag_main.c:
 *  - the trace phase (flat out, checked): ticks as scumm_pc's advance_audio, VAR_TIMER_NEXT
 *    jiffies x 472 / 60 before each round, and every write folded into a running CRC of the
 *    lines scumm_pc --opl prints ("OPL tick reg val"), logged as "A lines crc" every 256
 *    (tools/trace_crc.py --opl).  Nothing goes to the DSP.
 *  - real time: the video interrupt (c_audio, once a frame) runs the driver until it is
 *    AD_LEAD ticks ahead of the DSP (~8 ticks a frame), each tick's writes then $0001 into the
 *    ring.  The main loop's own calls into the driver (a sound started by a script) write into
 *    the same tick buffer, under ScummVM's Player_AD mutex (plat_ad_lock: AD_LOCK); while it is
 *    held the interrupt leaves the driver alone, and catches up the frame after.
 */
#include <string.h>
#include "jag.h"

/* the writes of the tick being made (and of the main loop's calls since the last tick) */
#define TICK_MAX 1024
static uint16_t tickBuf[TICK_MAX];
static int tickN;            /* words in tickBuf */
static int tickEnd;          /* words up to and with the tick's $0001; 0 = no tick made yet */
static uint32_t tickLost;    /* writes that did not fit in tickBuf (never expected) */

static int mode;             /* 0 none, 1 the trace phase's check, 2 real time */
static uint32_t produced;    /* ticks written into the ring */
static uint32_t ringWr;      /* the ring's next word (an address); MUS_WR once a tick is whole */
static uint32_t ringFull;    /* frames the ring had no room (the DSP behind: never expected) */

/* the trace phase */
static uint32_t traceTick, tickAcc, oplCrc, oplLines;

static const char hexd[] = "0123456789ABCDEF";

static void crc_line(int reg, int val)
{
    char line[24], *o = line + sizeof(line);
    uint32_t t = traceTick, c;
    int n = 0;
    *--o = 0;
    *--o = hexd[val & 15];
    *--o = hexd[(val >> 4) & 15];
    *--o = ' ';
    *--o = hexd[reg & 15];
    *--o = hexd[(reg >> 4) & 15];
    *--o = ' ';
    do {
        *--o = (char)('0' + t % 10);
        t /= 10;
    } while (t);
    o -= 4;
    memcpy(o, "OPL ", 4);
    while (o[n])
        n++;
    c = jag_crc_more(~oplCrc, (const uint8_t *)o, (uint32_t)n);
    c = jag_crc_more(c, (const uint8_t *)"\n", 1);
    oplCrc = ~c;
    if (++oplLines % 256 == 0)
        jag_logf("A %u %x", (unsigned)oplLines, (unsigned)oplCrc);
}

void plat_opl_write(int reg, int val)
{
    if (mode == 1) {
        crc_line(reg, val);
        return;
    }
    if (mode != 2)
        return;
    if (tickN < TICK_MAX)
        tickBuf[tickN++] = (uint16_t)((reg & 0xFF) << 8 | (val & 0xFF));
    else
        tickLost++;
}

/* ScummVM's Player_AD _mutex (player_ad.c): the interrupt does not run the driver while the
 * main loop is inside it */
void plat_ad_lock(int on)
{
    HW32(AD_LOCK) = (uint32_t)on;
}

/* pauseEngine (a message banner): ScummVM's mixer stops, Player_AD with it.  In real time
 * the interrupt leaves the driver alone (AD_LOCK) and the DSP holds the music (MUS_HOLD:
 * silent, the stream where it was; JagGOT's hold keys the notes off, so a held note does not
 * sound again after the banner as ScummVM's does).  The trace phase: no round runs, so no
 * tick either. */
void plat_pauseEngine(int pause)
{
    if (mode != 2)
        return;
    if (pause) {
        HW32(AD_LOCK) = 1;
        HW32(MUS_HOLD) = 1;
    } else {
        HW32(MUS_HOLD) = 0;
        HW32(AD_LOCK) = 0;
    }
}

/* the trace phase: delta jiffies of driver ticks, as scumm_pc's advance_audio */
void audio_trace_round(int jiffies)
{
    if (mode != 1)
        return;
    tickAcc += (uint32_t)jiffies * AD_CALLBACK_FREQUENCY;
    while (tickAcc >= 60) {
        tickAcc -= 60;
        ad_onTimer();
        traceTick++;
    }
}

void audio_trace_end(void)
{
    if (mode == 1)
        jag_logf("A %u %x end", (unsigned)oplLines, (unsigned)oplCrc);
}

/* a phase starts (before scumm_init, whose sound_init runs ad_init): 1 the check, 2 real time.
 * Real time starts the ring from the place the DSP stands (it has played everything written
 * so far, or nothing). */
void audio_start(int m)
{
    mode = 0;
    traceTick = tickAcc = oplCrc = oplLines = 0;
    tickN = tickEnd = 0;
    if (m == 2 && g.soundcard) {
        ringWr = HW32(MUS_WR);
        produced = HW32(MUS_PLAYED);
        mode = 2;
    } else if (m == 1 && g.soundcard) {
        mode = 1;
    }
}

/* the DSP's counter, read until two reads agree (the 68000 reads a long in two halves) */
static uint32_t played(void)
{
    uint32_t a, b = HW32(MUS_PLAYED);
    do {
        a = b;
        b = HW32(MUS_PLAYED);
    } while (a != b);
    return a;
}

/* the tick (tickBuf[0..tickEnd)) into the ring, if there is room; the main loop's writes made
 * after it stay in tickBuf for the next tick */
static int ring_put(void)
{
    const uint32_t start = AD_RING, end = AD_RING + AD_RING_WORDS * 2;
    const uint32_t rd = HW32(MUS_RD);        /* high word constant: no tear */
    const uint32_t bytes = (uint32_t)tickEnd * 2;
    uint32_t wr = ringWr, used, need;
    used = wr >= rd ? wr - rd : wr + (end - start) - rd;
    need = bytes;
    if (wr + bytes + 2 > end)                 /* $FF00 here, the tick from the start */
        need += end - wr;
    if (used + need + 2 > end - start)        /* wr must never catch rd up: that reads empty */
        return 0;
    if (wr + bytes + 2 > end) {
        HW16(wr) = 0xFF00;
        wr = start;
    }
    memcpy((void *)wr, tickBuf, bytes);
    wr += bytes;
    ringWr = wr;
    HW32(MUS_WR) = wr;                        /* last: the DSP may read the tick now */
    tickN -= tickEnd;
    if (tickN)
        memmove(tickBuf, tickBuf + tickEnd, (size_t)tickN * 2);
    tickEnd = 0;
    produced++;
    return 1;
}

/* the video interrupt, every frame (src/main.s CAudio) */
void c_audio(void)
{
    if (mode != 2 || HW32(AD_LOCK))
        return;
    while ((int32_t)(produced - played()) < AD_LEAD) {
        if (!tickEnd) {
            ad_onTimer();
            if (tickN < TICK_MAX)
                tickBuf[tickN++] = 0x0001;
            else
                tickLost++;
            tickEnd = tickN;
        }
        if (!ring_put()) {
            ringFull++;
            break;
        }
    }
}

/* the time phase's report: the counters since the report before (the DSP publishes its own
 * every 64 samples) */
void audio_report(void)
{
    static uint32_t last[6];
    uint32_t now[6];
    int i;
    if (mode != 2)
        return;
    now[0] = played();
    now[1] = HW32(MUS_STALL);
    now[2] = HW32(SND_STARVE);
    now[3] = HW32(SND_IRQ);
    now[4] = HW32(SND_DONE);
    now[5] = HW32(SND_SPIN);
    jag_logf("audio: ticks %u, stalls %u, starved %u, irq %u, samples made %u, spins %u "
             "(ring full %u, writes lost %u)",
             (unsigned)(now[0] - last[0]), (unsigned)(now[1] - last[1]),
             (unsigned)(now[2] - last[2]), (unsigned)(now[3] - last[3]),
             (unsigned)(now[4] - last[4]), (unsigned)(now[5] - last[5]),
             (unsigned)ringFull, (unsigned)tickLost);
    for (i = 0; i < 6; i++)
        last[i] = now[i];
}
