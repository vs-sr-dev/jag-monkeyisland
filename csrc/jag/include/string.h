/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Samuele Voltan.  See LICENSE. */
/* JaguarMI on the 68000: the part of <string.h> the interpreter uses (csrc/jag/jag_main.c). */
#ifndef JAG_STRING_H
#define JAG_STRING_H
#include <stddef.h>
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
#endif
