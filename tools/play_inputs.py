#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""The input of a play-through on the Jaguar, from its COM8 log, as a scumm_pc --input file.

    python tools/play_inputs.py LOG [OUT]          default OUT: LOG with .in

A PLAY ROM logs every input it gives the game as "I ROUND ..." (csrc/jag/jag_main.c
pad_input, plat_waitBannerInput): moves, A / B as the left / right button, C '.', OPTION
ESC, the keypad's keys as "keypad K" (keypad.c decides again on the PC).  Replay:

    build/pc/scumm_pc.exe MI_1 ROUNDS --bootparam N --debugmode D --soundcard 3 --input OUT

with the ROM's boot parameter, its debug mode (JMI_DEBUG) and the AdLib, as it played.
The music ran in real time on the Jaguar and per round on the PC: a script that waits on a
sound may see it end a round apart, and the replay part from the play there.
"""
import re
import sys


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    log = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else re.sub(r'\.log$', '', log) + '.in'
    n = 0
    with open(out, 'w') as f:
        f.write(f'# input of {log} (tools/play_inputs.py)\n')
        for ln in open(log, encoding='latin-1', errors='replace'):
            m = re.match(r'\s*[0-9.]+\s+I (\d+ .*)$', ln.rstrip())
            if m:
                f.write(m.group(1) + '\n')
                n += 1
    print(f'play_inputs: {n} events into {out}')


if __name__ == '__main__':
    main()
