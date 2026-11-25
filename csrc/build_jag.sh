#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
# JaguarMI - the C blob: the interpreter (csrc/scumm/) + its Jaguar platform (csrc/jag/).
# Runs inside the marsdev-32x Docker image (m68k-elf-gcc 15.2.0, libgcc built for the 68000):
#
#   docker run --rm -v "<the repository>:/work" -w /work/csrc marsdev-32x:latest "sh build_jag.sh"
#
# The command goes as ONE string: the image's entrypoint ignores anything else and exits 0
# printing nothing.  (From Git Bash, prefix MSYS_NO_PATHCONV=1.)  JagGOT's recipe, except that
# the interpreter is our own C99 with 32-bit int: no -mshort, so libgcc (32-bit multiply and
# divide for the 68000) links as it is.
#
# Output: build/engine.bin (packed in the cart, copied to DBLOB at boot), .elf, .sym, .lst.
set -e

BIN=/marsdev/mars/m68k-elf/bin
OUT=../build

# -ffreestanding: no libc; jag/include gives the two headers the interpreter includes, and
#   jag_main.c the functions behind them.  gcc's own <stdint.h> <stddef.h> <stdarg.h> stay.
# -fno-tree-loop-distribute-patterns: gcc would turn memset's own loop into a memset call.
CFLAGS="-DSCUMM_PROF -m68000 -O2 -std=c99 -fomit-frame-pointer -ffreestanding \
        -fno-tree-loop-distribute-patterns --param=min-pagesize=0 \
        -Wall -Wextra -Wno-unused-parameter -Ijag/include -Iscumm -I."

# every file of the interpreter (the PC build takes the same scumm/*.c), then the platform
SRC="$(ls scumm/*.c) jag/jag_main.c jag/jag_gfx.c jag/jag_audio.c"

OBJ=obj
rm -rf "$OBJ"
mkdir -p "$OBJ" "$OUT"

"$BIN/m68k-elf-as" -m68000 jag/crt0.s -o "$OBJ/crt0.o"
OBJS="$OBJ/crt0.o"
for f in $SRC; do
    o="$OBJ/$(basename "$f" .c).o"
    "$BIN/m68k-elf-gcc" $CFLAGS -c "$f" -o "$o"
    OBJS="$OBJS $o"
done

LIBGCC=$("$BIN/m68k-elf-gcc" -m68000 -print-libgcc-file-name)
"$BIN/m68k-elf-ld" --no-warn-rwx-segments -T ../include/jag_c.ld $OBJS "$LIBGCC" -o "$OUT/engine.elf"
"$BIN/m68k-elf-objcopy" -O binary -j .text -j .data "$OUT/engine.elf" "$OUT/engine.bin"
"$BIN/m68k-elf-nm" -n "$OUT/engine.elf" > "$OUT/engine.sym"
"$BIN/m68k-elf-objdump" -d "$OUT/engine.elf" > "$OUT/engine.lst"
rm -rf "$OBJ"

# The entry table MUST be the blob's first bytes: RMAC jumps to fixed offsets.
FIRST=$("$BIN/m68k-elf-nm" -n "$OUT/engine.elf" | grep -m1 ' [Tt] ' | awk '{print $3}')
if [ "$FIRST" != "entry_table" ]; then
    echo "ERROR: entry_table is not the blob's first symbol ($FIRST)"
    exit 1
fi
"$BIN/m68k-elf-size" "$OUT/engine.elf"
