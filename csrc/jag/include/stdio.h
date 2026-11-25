/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Samuele Voltan.  See LICENSE. */
/* JaguarMI on the 68000: the part of <stdio.h> the interpreter uses (csrc/jag/jag_main.c). */
#ifndef JAG_STDIO_H
#define JAG_STDIO_H
#include <stddef.h>
#include <stdarg.h>
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
#endif
