#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""The expected CRC32 of a decoded room, to check the Jaguar's room lines on COM8.

    python tools/room_crc.py [ROOM ...] [--log COM8_LOG] [--game MI_1]

The Jaguar (csrc/jag/jag_main.c plat_room) logs "room N WxH ... crc X" after the GPU has
decoded room N from the game image: the CRC32 (zlib's) of the w x h buffer, 8 bpp, row-major.
Here the same room goes through ScummVM's decoder (tools/scumm_v4.py decode_room, which
tools/m0_ref.py checks pixel for pixel against ScummVM's algorithm).  With --log every room
line of the log is checked; otherwise the CRCs of the rooms given are printed.
"""
import os
import re
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scumm_v4 as sv4

LINE = re.compile(r'\broom (\d+) (\d+)x(\d+) .*crc ([0-9a-f]+)')


def main():
    argv = sys.argv[1:]
    game = os.path.join(os.path.dirname(HERE), 'MI_1')
    log = None
    if '--game' in argv:
        i = argv.index('--game'); game = argv[i + 1]; del argv[i:i + 2]
    if '--log' in argv:
        i = argv.index('--log'); log = argv[i + 1]; del argv[i:i + 2]
    g = sv4.Game(game)
    if log:
        bad = 0
        for ln in open(log, encoding='latin-1', errors='replace'):
            m = LINE.search(ln)
            if not m:
                continue
            n, w, h, crc = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4), 16)
            room = g.rooms[n]
            want = zlib.crc32(sv4.decode_room(room))
            ok = (w, h) == (room.w, room.h) and crc == want
            bad += not ok
            print(f'room {n:2} {w}x{h}: {"OK" if ok else "DIFFERS"} (Jaguar {crc:08x}, '
                  f'ScummVM {want:08x})')
        sys.exit(1 if bad else 0)
    for a in argv:
        room = g.rooms[int(a)]
        print(f'room {room.num} {room.w}x{room.h} crc {zlib.crc32(sv4.decode_room(room)):08x}')


if __name__ == '__main__':
    main()
