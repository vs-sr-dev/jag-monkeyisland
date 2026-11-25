#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""The picture against the oracle: ScummVM's screens, pixel for pixel, against scumm_pc's frames.

    python tools/shot_cmp.py BURST_DIR [--diff OUT_DIR] -- SCUMM_PC_ARGS...

BURST_DIR holds ScummVM's distinct screens from oracle_scummvm.ps1 -Pixels -Burst "a,b"
(320x200 PNGs named by milliseconds), or its -Shots PNGs.  scumm_pc runs with the arguments
after "--" (the same boot param and --input as the trace check) plus --shots, which lists the
CRC32 of every frame it shows (each loop round, and each step of an effect).

Each ScummVM screen is looked up by the CRC32 of its RGB bytes.  A screen equal to one of our
frames is matched; the frames matched must not go backwards.  A screen equal to none is
compared with our frames near where it should be (its time, between the frames its matched
neighbours had; scumm_pc --ppm), and the closest frame is reported with the number of
differing pixels and their bounding box; --diff writes the ScummVM screen, ours and a
difference image for each.

The comparison is in RGB: ScummVM's OpenGL window shows the palette's 8-bit values exactly,
and two palette entries may hold one colour, so indices could not be recovered from a shot.
"""
import os
import re
import subprocess
import sys
import tempfile
import zlib

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, 'build', 'pc', 'scumm_pc.exe')
LINE = re.compile(r'^(-?\d+)(?:\.(\d+))? ([0-9a-f]{8})$')


def run_pc(args, extra):
    r = subprocess.run([EXE] + args + extra, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                       text=True, errors='replace')
    for ln in r.stdout.splitlines()[-3:]:
        if ln.startswith(('FATAL', 'HALT')):
            print('scumm_pc:', ln)


def load_frames(path):
    """[(frame, sub, crc)] in the order shown"""
    out = []
    for ln in open(path):
        m = LINE.match(ln.strip())
        if m:
            out.append((int(m.group(1)), int(m.group(2) or 0), int(m.group(3), 16)))
    return out


def ppm(path):
    return Image.open(path).convert('RGB')


def diff_count(a, b):
    """a, b: 200x320x3 arrays -> (differing pixels, their box or None)"""
    d = np.any(a != b, axis=2)
    n = int(d.sum())
    if not n:
        return 0, None
    ys, xs = np.nonzero(d)
    return n, (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))


def shot_ms(name):
    stem = os.path.splitext(name)[0]
    return int(stem) if stem.isdigit() else None


def main():
    argv = sys.argv[1:]
    if '--' not in argv:
        print(__doc__)
        sys.exit(1)
    i = argv.index('--')
    ours, pcargs = argv[:i], argv[i + 1:]
    diffdir = None
    if '--diff' in ours:
        k = ours.index('--diff')
        diffdir = ours[k + 1]
        del ours[k:k + 2]
    burst = ours[0]
    shots = sorted(f for f in os.listdir(burst) if f.lower().endswith('.png'))
    if not shots:
        print('no PNGs in', burst)
        sys.exit(1)

    tmp = tempfile.mkdtemp(prefix='shot_cmp_')
    crcfile = os.path.join(tmp, 'shots.txt')
    run_pc(pcargs, ['--shots', crcfile])
    frames = load_frames(crcfile)
    where = {}
    for idx, (f, s, c) in enumerate(frames):
        where.setdefault(c, []).append(idx)

    results = []                          # (shot, index into frames or None)
    last = -1
    for name in shots:
        img = Image.open(os.path.join(burst, name)).convert('RGB')
        if img.size != (320, 200):
            print(f'{name}: {img.size[0]}x{img.size[1]}, not a -Pixels shot')
            sys.exit(1)
        crc = zlib.crc32(img.tobytes())
        cands = [k for k in where.get(crc, []) if k >= last]
        if cands:
            last = cands[0]
            results.append((name, last))
        else:
            results.append((name, None))

    matched = sum(1 for _, k in results if k is not None)
    print(f'{matched} of {len(results)} ScummVM screens equal one of our frames '
          f'({len(frames)} frames shown)')
    misses = [(n, j) for j, (n, k) in enumerate(results) if k is None]
    if not misses:
        a = frames[results[0][1]]
        b = frames[results[-1][1]]
        print(f'all matched, in order: frames {a[0]}..{b[0]}')
        return 0

    # the frames around each miss: between the matches before and after it, and near the
    # frame its time says (ScummVM shows about ten frames a second; +-15 frames of slack)
    def window(j):
        lo = next((t for t in range(j - 1, -1, -1) if results[t][1] is not None), None)
        hi = next((t for t in range(j + 1, len(results)) if results[t][1] is not None), None)
        flo = frames[results[lo][1]][0] if lo is not None else None
        fhi = frames[results[hi][1]][0] if hi is not None else None
        t = shot_ms(results[j][0])
        if flo is None and fhi is None:
            return 0, 60
        if flo is None:
            flo = max(0, fhi - 60)
            tlo = None
        else:
            tlo = shot_ms(results[lo][0])
        if fhi is None:
            fhi = flo + 600
            thi = None
        else:
            thi = shot_ms(results[hi][0])
        if t is not None and tlo is not None:
            rate = (fhi - flo) / (thi - tlo) if thi is not None and thi > tlo else 0.01
            est = flo + (t - tlo) * rate
            return max(flo, int(est) - 15), min(fhi, int(est) + 15)
        return flo, fhi

    spans = [window(j) for _, j in misses]
    a, b = min(s[0] for s in spans), max(s[1] for s in spans)
    if b - a > 1000:
        print(f'{len(misses)} misses over frames {a}..{b}: too wide to dump; first misses:')
        for (n, _), sp in list(zip(misses, spans))[:10]:
            print(f'  {n}: between frames {sp[0]} and {sp[1]}')
        return 1
    ppmdir = os.path.join(tmp, 'ppm')
    os.makedirs(ppmdir)
    run_pc(pcargs, ['--ppm', ppmdir, '--ppm-frames', f'{a}-{b}'])
    pcs = {}
    for f in os.listdir(ppmdir):
        m = re.match(r'f(\d+)(?:_(\d+))?\.ppm$', f)
        if m:
            pcs[(int(m.group(1)), int(m.group(2) or 0))] = os.path.join(ppmdir, f)
    arrays = {}

    def arr(key):
        if key not in arrays:
            arrays[key] = np.asarray(ppm(pcs[key]))
        return arrays[key]
    if diffdir:
        os.makedirs(diffdir, exist_ok=True)
    for (n, j), (flo, fhi) in zip(misses, spans):
        img = Image.open(os.path.join(burst, n)).convert('RGB')
        shot = np.asarray(img)
        best = None
        for key in sorted(pcs):
            if not (flo <= key[0] <= fhi):
                continue
            d, box = diff_count(shot, arr(key))
            if best is None or d < best[0]:
                best = (d, box, key)
        if best is None:
            print(f'{n}: no frame of ours between {flo} and {fhi}')
            continue
        d, box, key = best
        tag = f'{key[0]}' + (f'.{key[1]}' if key[1] else '')
        print(f'{n}: closest frame {tag}, {d} pixels differ, box {box}')
        if diffdir:
            stem = os.path.splitext(n)[0]
            ours_img = ppm(pcs[key])
            img.save(os.path.join(diffdir, f'{stem}_scummvm.png'))
            ours_img.save(os.path.join(diffdir, f'{stem}_ours_{tag}.png'))
            a, b = shot, np.asarray(ours_img)
            d = np.any(a != b, axis=2)
            out = (a // 3).astype(np.uint8)
            out[d] = (255, 0, 255)
            Image.fromarray(out).save(os.path.join(diffdir, f'{stem}_diff.png'))
    return 1


if __name__ == '__main__':
    sys.exit(main())
