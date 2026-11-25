#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""Every script of the game, one file each, and descumm's listing of it.

Usage:
    python tools/script_dump.py <game dir> <out dir> [descumm.exe]

<game dir> as the other tools (000.LFL + DISK01-04.LEC, flat or d1..d4).  Writes into
<out dir>:
    sc_NNN.bin        global script NNN (numbered from the index's 0S directory)
    rRRR_en.bin       room RRR's entry code, rRRR_ex.bin its exit code
    rRRR_lsNNN.bin    room RRR's local script NNN
    rRRR_oc_OOOO.bin  object OOOO's code in room RRR (its verb table and verb scripts)
each the whole block, header included (as descumm -4 wants it), and with descumm a .txt
listing beside each.  index.txt lists them all.

descumm comes from scummvm-tools (GPL; built from its sources with g++, DESCUMM in local.ini; see
docs/todo.md, session 10): a dev tool, never part of the ROM.  The scan of the listings is
tools/script_scan.py.
"""
import os
import struct
import subprocess
import sys

XOR_KEY = 0x69


def find(d, name, disk):
    for p in (os.path.join(d, name), os.path.join(d, f'd{disk}', name)):
        if os.path.exists(p):
            return p
    raise FileNotFoundError(f'{name} not found in {d} (flat or d{disk}/)')


def blocks(b, off, end):
    while off + 6 <= end:
        size = struct.unpack_from('<I', b, off)[0]
        if size < 6 or off + size > end:
            return
        yield off, size, b[off + 4:off + 6].decode('latin1')
        off += size


def index_dir(idx, tag):
    """A v4 directory (0S, 0R...): {number: (room, offset)}."""
    for o, size, t in blocks(idx, 0, len(idx)):
        if t == tag:
            n = struct.unpack_from('<H', idx, o + 6)[0]
            out = {}
            for i in range(n):
                room = idx[o + 8 + 5 * i]
                off = struct.unpack_from('<I', idx, o + 9 + 5 * i)[0]
                if room or off:
                    out[i] = (room, off)
            return out
    return {}


def scripts(d):
    """(name, bytes) of every script block, in file order."""
    idx = open(find(d, '000.LFL', 1), 'rb').read()
    sdir = index_dir(idx, '0S')
    by_place = {v: k for k, v in sdir.items()}
    out = []
    for disk in range(1, 5):
        b = bytes(x ^ XOR_KEY for x in open(find(d, f'DISK0{disk}.LEC', disk), 'rb').read())
        for _, lsize, _ in blocks(b, 0, len(b)):
            for lo, ls, lt in blocks(b, 6, lsize):
                if lt != 'LF':
                    continue
                room = struct.unpack_from('<H', b, lo + 6)[0]
                o, end, first = lo + 8, lo + ls, True
                while o + 6 <= end:
                    size = struct.unpack_from('<I', b, o)[0]
                    tag = b[o + 4:o + 6].decode('latin1')
                    if tag == 'SO':          # sound blocks nest: step in (lec_survey.py)
                        o += 6
                        continue
                    if tag == 'RO' and first:
                        first = False
                        for so, ssize, st in blocks(b, o + 6, o + size):
                            blk = b[so:so + ssize]
                            if st == 'EN':
                                out.append((f'r{room:03}_en', blk))
                            elif st == 'EX':
                                out.append((f'r{room:03}_ex', blk))
                            elif st == 'LS':
                                out.append((f'r{room:03}_ls{blk[6]:03}', blk))
                            elif st == 'OC':
                                obj = struct.unpack_from('<H', blk, 6)[0]
                                out.append((f'r{room:03}_oc_{obj:04}', blk))
                    elif tag == 'SC':
                        # the index's offset is from the LF's room number (ScummVM: v4
                        # resources start at the room's LF data, 8 bytes in)
                        num = None
                        for base in (lo, lo + 6, lo + 8):
                            num = by_place.get((room, o - base))
                            if num is not None:
                                break
                        if num is None:
                            raise SystemExit(f'SC at {o:#x} in room {room}: not in 0S')
                        out.append((f'sc_{num:03}', b[o:o + size]))
                    o += size
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    d, outd = sys.argv[1], sys.argv[2]
    descumm = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(outd, exist_ok=True)
    lst = scripts(d)
    with open(os.path.join(outd, 'index.txt'), 'w') as ix:
        for name, blk in lst:
            p = os.path.join(outd, name + '.bin')
            open(p, 'wb').write(blk)
            ix.write(f'{name} {len(blk)}\n')
            if descumm:
                r = subprocess.run([descumm, '-4', '-o', p], capture_output=True, text=True,
                                   encoding='latin1')
                open(os.path.join(outd, name + '.txt'), 'w', encoding='latin1').write(
                    r.stdout + (('\n#STDERR ' + r.stderr) if r.stderr else '') +
                    (f'\n#EXIT {r.returncode}\n' if r.returncode else ''))
    print(f'{len(lst)} scripts into {outd}')


if __name__ == '__main__':
    main()
