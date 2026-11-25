#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""The Jaguar's CLUT hand-overs against the PC reference renderer's palette.

    python tools/pal_crc.py COM8_LOG [--bb | --z | --fb | --sc | --cur] -- SCUMM_PC_ARGS...

--bb: the room buffer instead.  The Jaguar logs "bb ROUND crc X" when the visible window of
the room buffer it hands over (320 x the main screen's height, at the camera) changed;
scumm_pc --bbcrc prints the same of its back buffer.

--z: the z-plane buffer.  The Jaguar logs "z ROUND crc X" when the mask buffer the GPU writes
(GQ_MASK; all g.numZBuffer x (height + 4) x 40 bytes) changed; scumm_pc --zcrc prints the same
of ScummVM's.

--fb: the front buffer's window (the main virtual screen, 320 x its height, at the camera);
--sc: the system screen (320 x 200 palette indices).  scumm_pc --fbcrc and --sccrc.  Until the
Jaguar draws the costumes and the text, pass scumm_pc --no-cels --no-chars after the "--".

In the trace phase the Jaguar logs "pal ROUND crc X" whenever plat_drawDirtyScreenParts hands
the video interrupt a palette updatePalette changed: X is the CRC32 of the 256 RGB colours it
was given (csrc/jag/jag_main.c).  scumm_pc --palcrc prints the same line after each round in
which its system palette changed.  Both lists must be equal, round for round, as far as the
Jaguar's trace phase went.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(os.path.dirname(HERE), 'build', 'pc', 'scumm_pc.exe')
PAL = re.compile(r'\bpal (\d+) crc ([0-9a-f]+)\s*$')
BB = re.compile(r'\bbb (\d+) crc ([0-9a-f]+)\s*$')
Z = re.compile(r'\bz (\d+) crc ([0-9a-f]+)\s*$')
FB = re.compile(r'\bfb (\d+) crc ([0-9a-f]+)\s*$')
SC = re.compile(r'\bsc (\d+) crc ([0-9a-f]+)\s*$')
CUR = re.compile(r'\bcur (\d+) crc ([0-9a-f]+)\s*$')


def main():
    argv = sys.argv[1:]
    pat, flag, what = PAL, '--palcrc', 'palette hand-overs'
    if '--bb' in argv:
        argv.remove('--bb')
        pat, flag, what = BB, '--bbcrc', 'room buffer windows'
    if '--z' in argv:
        argv.remove('--z')
        pat, flag, what = Z, '--zcrc', 'z-plane buffers'
    if '--fb' in argv:
        argv.remove('--fb')
        pat, flag, what = FB, '--fbcrc', 'front buffer windows'
    if '--cur' in argv:
        argv.remove('--cur')
        pat, flag, what = CUR, '--curcrc', 'cursor states'
    if '--sc' in argv:
        argv.remove('--sc')
        pat, flag, what = SC, '--sccrc', 'system screens'
    if '--' not in argv or argv.index('--') != 1:
        print(__doc__)
        return 1
    log, pcargs = argv[0], argv[2:]
    jag, rounds = [], None
    for ln in open(log, encoding='latin-1', errors='replace'):
        m = re.search(r'trace: (\d+) rounds', ln)
        if m:                                 # the end of the trace phase
            rounds = int(m.group(1))
            break
        m = pat.search(ln)
        if m:
            jag.append((int(m.group(1)), int(m.group(2), 16)))
    if not jag:
        print('no lines of', what, 'in', log)
        return 1
    r = subprocess.run([EXE] + pcargs + [flag], stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL, text=True, errors='replace')
    pc = [(int(m.group(1)), int(m.group(2), 16)) for m in map(pat.search, r.stdout.splitlines()) if m]
    last = rounds - 1 if rounds else jag[-1][0]
    pc = [p for p in pc if p[0] <= last]
    for k, (a, b) in enumerate(zip(jag, pc)):
        if a != b:
            print(f'{what} {k}: Jaguar round {a[0]} crc {a[1]:08x}, PC round {b[0]} crc {b[1]:08x}')
            return 1
    if len(jag) != len(pc):
        print(f'Jaguar {len(jag)} {what}, PC {len(pc)} up to round {last}')
        return 1
    print(f'all {len(jag)} {what} equal the PC\'s (rounds {jag[0][0]}..{last})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
