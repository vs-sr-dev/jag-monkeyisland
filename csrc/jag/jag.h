/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Samuele Voltan.  See LICENSE. */
/*
 * jag.h - what the Jaguar platform's two halves share: jag_main.c (the libc part, the log, the
 * time, the GPU's command queue, the entry points) and jag_gfx.c (ScummVM's screens).
 */
#ifndef JAG_H
#define JAG_H
#include <stdint.h>
#include "scumm.h"
#include "mem_map.h"

#define HW32(a) (*(volatile uint32_t *)(a))
#define HW16(a) (*(volatile uint16_t *)(a))
#define HW8(a)  (*(volatile uint8_t *)(a))

/* jag_main.c */
void jag_logf(const char *fmt, ...);
uint32_t jag_time(void);                                 /* half-lines since boot */
uint32_t jag_crc(const uint8_t *p, uint32_t n);         /* zlib's CRC32 */
uint32_t jag_crc_more(uint32_t c, const uint8_t *p, uint32_t n);   /* ~ on both ends: start 0xFFFFFFFF */
void jag_wait_until(int (*done)(void), const char *what);   /* off the bus, with a ceiling */

/* the GPU's command queue (src/gpu_room.s): gq_slot gives the next command's 8 longs, waiting
 * for room if the ring is full; gq_commit hands it to the GPU; gq_drain waits until the GPU
 * has done every command */
volatile uint32_t *gq_slot(void);
void gq_commit(void);
void gq_drain(void);
uint32_t gq_head(void);
int gq_passed(uint32_t mark);        /* the GPU has done the commands up to mark */

/* jag_audio.c: the AdLib driver's output (1 the trace phase's check, 2 real time) */
void audio_start(int mode);
void audio_trace_round(int jiffies);
void audio_trace_end(void);
void audio_report(void);
void c_audio(void);                  /* the video interrupt, every frame */

/* jag_gfx.c */
void gfx_start(int trace);           /* scumm_init is next: the picture from scratch */
void gfx_report(uint32_t n);         /* the time phase: the blits of the last n rounds */
void gfx_present(void);              /* a hand-over now (a banner's waitForTimer) */

#endif
