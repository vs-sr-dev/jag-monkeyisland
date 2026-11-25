#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""Find the frames at which a ScummVM oracle run received its input, so that scumm_pc can
replay the same input and be compared line for line.

    python tools/input_fit.py ORACLE_OUT STEPS "move 240 120" "down L" [...] \
        [--args "--bootparam 1 --soundcard 0 --debugmode 1"] [--out build/pc/input.txt]

The oracle's input is real (tools/oracle_scummvm.ps1 -Clicks): it lands on whatever frame it
lands on.  Without an event, our trace departs from ScummVM's in the very frame where ScummVM
received it (the scripts read the mouse every frame), so the events are placed one at a time,
in order: run scumm_pc with the events placed so far (--frames marks the loop rounds), take the
frame of the first difference, put the next event there, and check that the match grows.  An
"up" event follows its "down" one frame later: in v4 only the click itself is visible to the
scripts.  Comparison as trace_cmp.py --vars (opcodes, runScript, writeVar values).
"""
import os
import re
import shlex
import subprocess
import sys

PAT = re.compile(r'^(Script \d+, offset 0x[0-9a-f]+: \[[0-9A-F]+\] \w+\(\)|runScript\(.*|'
                 r'writeVar\(-?\d+, -?\d+\))$')
EXE = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'build', 'pc', 'scumm_pc.exe'))


def filt(lines, frames=False):
    out, frame_of = [], []
    frame = -1
    for ln in lines:
        ln = ln.rstrip('\r\n')
        if ln.startswith('FRAME '):
            frame = int(ln[6:])
        elif PAT.match(ln) or ln.startswith(('HALT', 'FATAL')):
            out.append(ln)
            frame_of.append(frame)
    return (out, frame_of) if frames else out


def run(steps, base, events):
    cmd = [EXE, 'MI_1', str(steps)] + base + ['--frames']
    for e in events:
        cmd += ['--ev', e]
    res = subprocess.run(cmd, capture_output=True, text=True, encoding='latin-1')
    return filt(res.stdout.splitlines(), frames=True)


def first_diff(ref, ours):
    for n, (a, b) in enumerate(zip(ref, ours)):
        if a != b:
            return n
    return min(len(ref), len(ours))


def main():
    argv = sys.argv[1:]
    base = shlex.split('--bootparam 1 --soundcard 0 --debugmode 1')
    out_path = 'build/pc/input.txt'
    if '--args' in argv:
        i = argv.index('--args'); base = shlex.split(argv[i + 1]); del argv[i:i + 2]
    if '--out' in argv:
        i = argv.index('--out'); out_path = argv[i + 1]; del argv[i:i + 2]
    oracle, steps, specs = argv[0], int(argv[1]), argv[2:]
    ref = filt(open(oracle, encoding='latin-1', errors='replace'))

    placed = []

    def refine(k):
        """The pixel ScummVM read can differ by one or two from the one the oracle aimed at
        (its window-to-game rounding), and that only shows once the click acted (where a walk
        ends): try the neighbours of move k on the run so far."""
        parts = placed[k].split()
        f, x, y = int(parts[0]), int(parts[2]), int(parts[3])
        best = None
        for dy in (0, 1, -1, 2, -2):
            for dx in (0, 1, -1, 2, -2):
                trial = placed[:k] + [f'{f} move {x + dx} {y + dy}'] + placed[k + 1:]
                m = first_diff(ref, run(steps, base, trial)[0])
                if best is None or m > best[0]:
                    best = (m, trial[k])
        if best[1] != placed[k]:
            print(f'  {placed[k]}  ->  {best[1]}   (now identical for {best[0]} lines)')
            placed[k] = best[1]

    pending = None
    for spec in specs:
        if spec.startswith('up'):
            down = [e for e in placed if ' down ' in e][-1]
            placed.append(f'{int(down.split()[0]) + 1} {spec}')
            print(f'  {placed[-1]}   (one frame after its down)')
            moves = [k for k, e in enumerate(placed) if e.split()[1] == 'move']
            if moves:
                refine(moves[-1])
            continue
        ours, frame_of = run(steps, base, placed)
        n = first_diff(ref, ours)
        if n >= min(len(ref), len(ours)):
            print(f'no difference left before "{spec}": the oracle never saw it?')
            break
        f0 = frame_of[n]
        best = None
        for f in (f0, f0 - 1, f0 + 1, f0 - 2):
            trial = placed + [f'{f} {spec}']
            m = first_diff(ref, run(steps, base, trial)[0])
            if best is None or m > best[0]:
                best = (m, f)
        m, f = best
        if spec.startswith('move') and m <= n:
            # a move that changes nothing the scripts read yet (e.g. into the verb area, where
            # the virtual mouse is -1): it is placed with the click that follows
            pending = spec
            print(f'  {spec}   (nothing visible yet: placed with the next click)')
            continue
        if spec.startswith('down') and pending:
            # oracle_scummvm.ps1 moves the cursor 0.4 s (4 rounds) before the button: the
            # scripts cannot tell, but the screen can (the cursor), so the move goes 4 rounds
            # before its click, never before the previous event
            prev = max([int(e.split()[0]) for e in placed] or [0])
            best = None
            for f in (f0, f0 - 1, f0 + 1, f0 - 2):
                mf = max(prev, f - 4)
                trial = placed + [f'{mf} {pending}', f'{f} {spec}']
                m = first_diff(ref, run(steps, base, trial)[0])
                if best is None or m > best[0]:
                    best = (m, f, mf)
            m, f, mf = best
            placed.append(f'{mf} {pending}')
            print(f'  {placed[-1]}   (4 rounds before its click)')
            pending = None
        placed.append(f'{f} {spec}')
        print(f'  {placed[-1]}   (difference at line {n + 1} in frame {f0}; now identical for {m} lines)')

    ours, _ = run(steps, base, placed)
    n = first_diff(ref, ours)
    print(f'input_fit: identical for {n} lines (ScummVM {len(ref)}, ours {len(ours)})')
    if n < min(len(ref), len(ours)):
        for i in range(max(0, n - 6), n):
            print(f'    {ref[i]}')
        print(f'  S {ref[n]}')
        print(f'  J {ours[n]}')
    with open(out_path, 'w') as f:
        f.write(f'# input for {oracle} (tools/input_fit.py)\n')
        for e in placed:
            f.write(e + '\n')
    print(f'  events written to {out_path}')


if __name__ == '__main__':
    main()
