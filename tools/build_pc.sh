#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
# The PC build of the interpreter (the oracle harness), with MSYS2's gcc.
#
#   sh tools/build_pc.sh                 -> build/pc/scumm_pc.exe
#   build/pc/scumm_pc.exe MI_1 3000 --soundcard 0 --debugmode 1 > build/pc/trace.txt
#   python tools/trace_cmp.py <scummvm stdout> build/pc/trace.txt
#   build/pc/scumm_pc.exe MI_1 3000 --wav build/pc/mi1.wav     (the AdLib, for listening)
#
# --soundcard 0 --debugmode 1 match a ScummVM run that prints its trace (debug level 11,
# music_driver=null, --random-seed=1234; see docs/todo.md for the oracle's command line).
set -e
cd "$(dirname "$0")/.."
PATH=/c/msys64/ucrt64/bin:$PATH
python tools/gen_opcodes.py
mkdir -p build/pc
# Nuked OPL3 (LGPL, a dev tool for listening: never linked into the ROM), if present
NUKED=$(sed -n 's/^NUKED_OPL=//p' local.ini 2>/dev/null | tr -d '\r' | sed 's|^\([A-Za-z]\):|/\L\1|')
NUKED=${NUKED_OPL:-$NUKED}
EXTRA=
if [ -f $NUKED/opl3.c ]; then
    EXTRA="-DWITH_NUKED -I$NUKED $NUKED/opl3.c"
fi
gcc -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -o build/pc/scumm_pc.exe \
    csrc/pc/pc_main.c csrc/scumm/*.c csrc/ref/*.c $EXTRA
echo "build_pc: build/pc/scumm_pc.exe"
