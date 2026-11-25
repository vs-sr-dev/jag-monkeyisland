/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Samuele Voltan.  See LICENSE. */
/* The OPL2 model's samples for one scenario, for tools/opl2/dsp_check.py to compare with
 * the DSP binary run in the JRISC interpreter.  (JagGOT's, rewritten for JaguarMI's ring.)
 *
 *   opl2_dump ticks.bin N START LATE_FROM LATE_TO AT_HOLD AT_FREE out.bin sched.bin
 *
 * ticks.bin (tools/opl2/dsp_check.py, from scumm_pc --opl): u32 count, then per tick u16
 * words and the words, big-endian, the last one $0001.  The ring is AD_RING_WORDS words; the
 * stream starts at byte START of it (near its end: the first wrap comes soon).  Every frame
 * (FRAME samples, 60 Hz) the producer does what csrc/jag/jag_audio.c c_audio does: ticks
 * into the ring while it is fewer than LEAD ahead of the ticks played, if they fit (wr never
 * catches rd up; a tick that does not fit before the end goes to the start, $FF00 left at
 * the end); frames LATE_FROM..LATE_TO-1 produce nothing (the 68000 late: the DSP stalls).
 * MUS_HOLD is 1 from sample AT_HOLD to AT_FREE (-1: never).
 *
 * out.bin: N samples, int16 little-endian.  sched.bin: what the producer wrote, for the
 * DSP's DRAM: per frame that wrote, u32 sample, u32 count of (u32 offset, u16 word) pairs,
 * the pairs, u32 the new MUS_WR offset; at the end u32 0xFFFFFFFF, then the model's
 * played, stall, rd offset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opl2_model.h"

#define RING_WORDS 4096
#define RING (RING_WORDS * 2)
#define LEAD 24
#define FRAME 187

static uint8_t ring[RING];
static uint32_t pairs_off[RING], npairs;
static uint16_t pairs_w[RING];

static void put16(uint32_t off, uint16_t w)
{
    ring[off] = (uint8_t)(w >> 8);
    ring[off + 1] = (uint8_t)w;
    pairs_off[npairs] = off;
    pairs_w[npairs++] = w;
}

static void wr32(FILE *f, uint32_t v)
{
    fwrite(&v, 4, 1, f);
}

int main(int argc, char **argv)
{
    if (argc < 10) {
        fprintf(stderr, "usage: opl2_dump ticks.bin N START LATE_FROM LATE_TO AT_HOLD AT_FREE out.bin sched.bin\n");
        return 2;
    }
    FILE *tf = fopen(argv[1], "rb");
    if (!tf) { perror(argv[1]); return 2; }
    fseek(tf, 0, SEEK_END);
    long tn = ftell(tf);
    fseek(tf, 0, SEEK_SET);
    uint8_t *tb = malloc((size_t)tn);
    if (fread(tb, 1, (size_t)tn, tf) != (size_t)tn) return 2;
    fclose(tf);
    uint32_t nt = (uint32_t)tb[0] << 24 | tb[1] << 16 | tb[2] << 8 | tb[3];
    const uint8_t **tick = malloc(nt * sizeof(*tick));
    const uint8_t *p = tb + 4;
    for (uint32_t k = 0; k < nt; k++) {
        tick[k] = p;
        p += 2 + 2 * ((uint32_t)p[0] << 8 | p[1]);
    }
    long n = atol(argv[2]);
    uint32_t start = (uint32_t)atol(argv[3]);
    long late_from = atol(argv[4]), late_to = atol(argv[5]);
    long at_hold = atol(argv[6]), at_free = atol(argv[7]);
    static opl2 c;
    opl2_init(&c);
    opl2_song(&c, ring + start, ring);          /* InitDspAudio: MUS_ADR, MUS_LOOP */
    opl2_wr(&c, ring + start);                  /* MUS_WR: nothing written */
    c.rd = ring + start;                        /* MUS_RD */
    uint32_t wr = start, produced = 0, k = 0;
    FILE *f = fopen(argv[8], "wb"), *s = fopen(argv[9], "wb");
    for (long i = 0; i < n; i++) {
        long frame = i / FRAME;
        if (i % FRAME == 0 && !(frame >= late_from && frame < late_to)) {
            npairs = 0;
            while ((int32_t)(produced - c.played) < LEAD && k < nt) {
                uint32_t words = (uint32_t)tick[k][0] << 8 | tick[k][1];
                uint32_t bytes = words * 2, rd = (uint32_t)(c.rd - ring), used, need = bytes;
                used = wr >= rd ? wr - rd : wr + RING - rd;
                if (wr + bytes + 2 > RING)
                    need += RING - wr;
                if (used + need + 2 > RING)
                    break;
                if (wr + bytes + 2 > RING) {
                    put16(wr, 0xFF00);
                    wr = 0;
                }
                for (uint32_t j = 0; j < words; j++)
                    put16(wr + 2 * j, (uint16_t)(tick[k][2 + 2 * j] << 8 | tick[k][3 + 2 * j]));
                wr += bytes;
                opl2_wr(&c, ring + wr);
                produced++;
                k++;
            }
            if (npairs) {
                wr32(s, (uint32_t)i);
                wr32(s, npairs);
                for (uint32_t j = 0; j < npairs; j++) {
                    wr32(s, pairs_off[j]);
                    fwrite(&pairs_w[j], 2, 1, s);
                }
                wr32(s, wr);
            }
        }
        if (i == at_hold)
            opl2_hold(&c, 1);
        if (i == at_free)
            opl2_hold(&c, 0);
        int16_t v = (int16_t)opl2_sample(&c);
        fwrite(&v, 2, 1, f);
    }
    wr32(s, 0xFFFFFFFFu);
    wr32(s, c.played);
    wr32(s, c.stall);
    wr32(s, (uint32_t)(c.rd - ring));
    fclose(f);
    fclose(s);
    fprintf(stderr, "opl2_dump: %u of %u ticks written, %u played, %u stalls\n",
            (unsigned)k, (unsigned)nt, (unsigned)c.played, (unsigned)c.stall);
    return 0;
}
