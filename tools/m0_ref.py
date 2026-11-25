#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
# developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING.
"""M0's oracle check and expected frames.

    python tools/m0_ref.py [GAME_DIR] [ROOM ...]

1. Decodes every room of cart/rooms.bin with a model of src/gpu_room.s (the same loads, the
   same bit reader, the same walk) and compares it, pixel for pixel, with ScummVM's decoder run
   on the game's original bytes (tools/scumm_v4.py).  Any difference is a build error: the
   converter or the algorithm is wrong before the Jaguar is ever involved.
2. Writes docs/m0_expected_<room>.png: the room as the Jaguar shows it at camera 0 (320
   columns, the CLUT's RGB16 colours), for the rooms given (default: 38 lookout, 33 dock).
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import scumm_v4 as sv4


def L(b, o):
    return struct.unpack_from('>I', b, o)[0]


def S(b, o):
    return struct.unpack_from('>i', b, o)[0]


def gpu_model(pack, rec):
    """src/gpu_room.s, step by step: returns (w, h, pixels, clut words)."""
    w, h, n, clut_off, tab_off = (L(pack, rec + 4 * i) for i in range(5))
    hpar = [S(pack, rec + 20 + 4 * i) for i in range(4)]
    vpar = [S(pack, rec + 36 + 4 * i) for i in range(4)]
    buf = bytearray(w * h)
    for s in range(n):
        p = rec + L(pack, rec + tab_off + 4 * s)
        hdr = L(pack, p)
        p += 4
        shr = hdr & 0xFF
        in_s, in_c, out_s, out_c = vpar if hdr & 0x100 else hpar
        dst = 8 * s
        st = {'p': p, 'cur': 0, 'n': 0}

        def bit():
            if st['n'] == 0:
                st['cur'] = L(pack, st['p'])
                st['p'] += 4
                st['n'] = 32
            b = st['cur'] & 1
            st['cur'] >>= 1
            st['n'] -= 1
            return b

        def bits(k):
            v, m = 0, 1
            for _ in range(k):
                if bit():
                    v |= m
                m <<= 1
            return v

        color = bits(8)
        inc = -1
        for _ in range(out_c):
            for _ in range(in_c):
                buf[dst] = color & 0xFF
                dst += in_s
                if bit():
                    if not bit():
                        color = bits(shr)
                        inc = -1
                    elif not bit():
                        color += inc
                    else:
                        inc = -inc
                        color += inc
            dst += out_s
    clut = [struct.unpack_from('>H', pack, rec + clut_off + 2 * i)[0] for i in range(256)]
    return w, h, buf, clut


def main():
    args = sys.argv[1:]
    game_dir = os.path.join(REPO, 'MI_1')
    if args and not args[0].isdigit():
        game_dir = args.pop(0)
    wanted = [int(a) for a in args] or [38, 33]
    pack = open(os.path.join(REPO, 'cart', 'rooms.bin'), 'rb').read()
    g = sv4.Game(game_dir)
    checked = 0
    models = {}
    for num in range(128):
        rec = L(pack, 4 * num)
        if not rec:
            continue
        w, h, px, clut = gpu_model(pack, rec)
        ref = sv4.decode_room(g.rooms[num])
        if px != ref:
            bad = next(i for i in range(len(px)) if px[i] != ref[i])
            raise SystemExit(f'm0_ref: room {num} differs from ScummVM at x={bad % w}, y={bad // w}')
        checked += 1
        models[num] = (w, h, px, clut)
    print(f'm0_ref: {checked} rooms, the GPU model equals ScummVM\'s decoder on every pixel')

    from PIL import Image
    for num in wanted:
        w, h, px, clut = models[num]
        view = bytes(b for y in range(h) for b in px[y * w:y * w + 320])
        im = Image.frombytes('P', (320, h), view)
        im.putpalette([c for v in clut for c in sv4.rgb16_to_rgb(v)])
        out = os.path.join(REPO, 'docs', f'm0_expected_{num}.png')
        im.convert('RGB').save(out)
        print(f'  expected frame: {os.path.relpath(out, REPO)} (room {num}, {w}x{h}, camera 0)')


if __name__ == '__main__':
    main()
