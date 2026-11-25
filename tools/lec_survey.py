#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""Survey of Monkey Island 1 DOS VGA floppy data (SCUMM v4): the numbers in docs/feasibility.md §8.

Usage:
    python tools/lec_survey.py <game dir>              survey one copy
    python tools/lec_survey.py <game dir> <other dir>  ...and compare the two resource by resource

A game dir holds 000.LFL, 90x.LFL and DISK01-04.LEC, either flat or as d1..d4 subfolders
(the layout 7-Zip gives when extracting the four floppy images one per folder).

Format notes (v4, "small header"):
- every block is LE32 size (header included) + 2-byte tag;
- DISKnn.LEC is XORed with 0x69; 000.LFL is not;
- LE > FO + LF; LF = room number (LE16) + RO (the room) + its SC/SO/CO resources;
- sound blocks nest without consistent sizes: like ScummVM, an SO header is stepped into
  (6 bytes) instead of skipped, and the LF is walked flat;
- BM = LE32 length up to the z-planes, then one LE32 offset per 8-pixel strip (relative to the
  length field), the strips (first byte = codec), then z-planes chained by their LE16 sizes.
"""
import collections
import os
import struct
import sys


def find(d, name, disk):
    for p in (os.path.join(d, name), os.path.join(d, f'd{disk}', name)):
        if os.path.exists(p):
            return p
    raise FileNotFoundError(f'{name} not found in {d}')


def blocks(b, off, end):
    while off + 6 <= end:
        size = struct.unpack_from('<I', b, off)[0]
        if size < 6 or off + size > end:
            return
        yield off, size, b[off + 4:off + 6].decode('latin1')
        off += size


def strips(b, sp, n):
    """Codec byte and size of the n strips of an SMAP starting at sp."""
    smaplen = struct.unpack_from('<I', b, sp)[0]
    offs = [struct.unpack_from('<I', b, sp + 4 + 4 * i)[0] for i in range(n)] + [smaplen]
    return smaplen, [(b[sp + offs[i]], offs[i + 1] - offs[i]) for i in range(n)]


def survey(d):
    s = {'tag': collections.Counter(), 'cnt': collections.Counter(), 'snd': collections.Counter(),
         'sndn': collections.Counter(), 'codec': collections.Counter(), 'codecb': collections.Counter(),
         'oicodec': collections.Counter(), 'zp': collections.Counter(), 'rooms': [], 'scaled': [],
         'items': {}, 'zpbytes': 0, 'files': 0}
    for disk in range(1, 5):
        p = find(d, f'DISK0{disk}.LEC', disk)
        s['files'] += os.path.getsize(p)
        b = bytes(x ^ 0x69 for x in open(p, 'rb').read())
        for _, lsize, _ in blocks(b, 0, len(b)):
            for lo, ls, lt in blocks(b, 6, lsize):
                if lt != 'LF':
                    continue
                room = struct.unpack_from('<H', b, lo + 6)[0]
                o, end, first, k = lo + 8, lo + ls, True, collections.Counter()
                while o + 6 <= end:
                    size = struct.unpack_from('<I', b, o)[0]
                    tag = b[o + 4:o + 6].decode('latin1')
                    if tag == 'SO':
                        o += 6
                        continue
                    if tag == 'RO' and first:
                        first = False
                        room_block(s, b, o, size, room)  # its sub-blocks go into items
                        o += size
                        continue
                    if tag in ('WA', 'AD', 'RO'):
                        s['snd'][tag] += size
                        s['sndn'][tag] += 1
                    else:
                        s['tag'][tag] += size
                        s['cnt'][tag] += 1
                    k[tag] += 1
                    s['items'][(room, tag, k[tag])] = b[o:o + size]
                    o += size
    s['charsets'] = sum(os.path.getsize(find(d, f'90{i}.LFL', 1)) for i in range(1, 5))
    s['index'] = os.path.getsize(find(d, '000.LFL', 1))
    idx = open(find(d, '000.LFL', 1), 'rb').read()
    for o, size, tag in blocks(idx, 0, len(idx)):
        if tag == '0O':
            s['objects'] = struct.unpack_from('<H', idx, o + 6)[0]
    return s


def room_block(s, b, ro, rsize, room):
    s['tag']['RO'] += rsize
    s['cnt']['RO'] += 1
    info = {'room': room}
    k = collections.Counter()
    for o, size, tag in blocks(b, ro + 6, ro + rsize):
        k[tag] += 1
        s['items'][(room, 'RO.' + tag, k[tag])] = b[o:o + size]
        s['tag']['RO.' + tag] += size
        if tag == 'HD':
            info['w'], info['h'], info['nobj'] = struct.unpack_from('<HHH', b, o + 6)
        elif tag == 'SA' and any(b[o + 6:o + size]):
            s['scaled'].append(room)
        elif tag == 'BM' and info.get('h'):
            smaplen, st = strips(b, o + 6, info['w'] // 8)
            for c, n in st:
                s['codec'][c] += 1
                s['codecb'][c] += n
            zp, nz = o + 6 + smaplen, 0
            while zp + 2 <= o + size and struct.unpack_from('<H', b, zp)[0]:
                nz += 1
                zp += struct.unpack_from('<H', b, zp)[0]
            s['zp'][nz] += 1
            s['zpbytes'] += o + size - (o + 6 + smaplen)
        elif tag == 'OI' and size > 8:
            first = struct.unpack_from('<I', b, o + 12)[0]
            _, st = strips(b, o + 8, (first - 4) // 4)
            for c, _ in st:
                s['oicodec'][c] += 1
    s['rooms'].append(info)


def report(d, s):
    print(f'== {d}')
    total = s['files'] + s['charsets'] + s['index']
    drop = s['snd']['RO'] + s['snd']['WA']
    print(f'data: LEC {s["files"]:,} + charsets {s["charsets"]:,} + index {s["index"]:,} = {total:,} bytes')
    print(f'droppable: MT-32 RO {s["snd"]["RO"]:,} ({s["sndn"]["RO"]}) + speaker WA {s["snd"]["WA"]:,} '
          f'({s["sndn"]["WA"]}) -> kept {total - drop:,} bytes ({(total - drop) / 2**20:.2f} MiB)')
    print(f'AdLib AD: {s["snd"]["AD"]:,} bytes in {s["sndn"]["AD"]} chunks')
    for t in ('RO', 'RO.BM', 'RO.OI', 'CO', 'SC', 'RO.LS', 'RO.OC', 'RO.PA'):
        print(f'  {t:6} {s["tag"][t]:>10,}  {s["cnt"].get(t, "")}')
    real = [r for r in s['rooms'] if r.get('h')]
    print(f'rooms: {len(s["rooms"])} ({len(s["rooms"]) - len(real)} without an image)')
    print('  widths:', sorted(collections.Counter(r['w'] for r in real).items()))
    print('  heights:', sorted(collections.Counter(r['h'] for r in real).items()))
    big = max(real, key=lambda r: r['w'] * r['h'])
    print(f'  largest: room {big["room"]} {big["w"]}x{big["h"]} = {big["w"] * big["h"]:,} bytes at 8 bpp')
    print(f'  all rooms at 8 bpp: {sum(r["w"] * r["h"] for r in real):,} bytes '
          f'(compressed BM {s["tag"]["RO.BM"]:,})')
    print('room strip codecs (count, bytes):', sorted((c, n, s['codecb'][c]) for c, n in s['codec'].items()))
    print('object image strip codecs:', sorted(s['oicodec'].items()))
    print('z-planes per room:', sorted(s['zp'].items()), f'({s["zpbytes"]:,} bytes)')
    print(f'rooms with scale slots: {len(s["scaled"])}')
    print(f'global objects (index 0O): {s.get("objects")}')


def compare(a, b):
    same = diff = 0
    by = collections.Counter()
    for k, v in a['items'].items():
        w = b['items'].get(k)
        if w is None:
            continue
        if v == w:
            same += len(v)
        else:
            diff += len(w)
            by[k[1]] += len(w)
    only = sum(len(v) for k, v in b['items'].items() if k not in a['items'])
    print(f'== compare: identical {same:,}, differing {diff:,} (second copy), only in second {only:,}')
    print('  differing by tag:', by.most_common())


if __name__ == '__main__':
    res = []
    for d in sys.argv[1:3]:
        res.append(survey(d))
        report(d, res[-1])
    if len(res) == 2:
        compare(*res)
