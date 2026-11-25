#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""Save, load, go on: the trace must not tell.

    python tools/saveload_check.py STEPS N[,N...] -- SCUMM_PC_ARGS...

For each N: scumm_pc runs STEPS rounds saving at round N (--save-at N), then a fresh
scumm_pc loads that save at round N (--load) and runs on to STEPS; the trace lines after
"SAVELOAD" must be the same in both (opcodes, runScript, writeVar and their values, as
trace_cmp.py --vars, plus the system screen's CRCs if --sccrc is among the args).  The
input file of the args (--input) goes to both: its rounds are the same rounds.
Prints the save's size and the first difference, if any.
"""
import os
import re
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, '..', 'build', 'pc', 'scumm_pc.exe')
PAT = re.compile(r'^(Script \d+, offset 0x[0-9a-f]+: \[[0-9A-F]+\] \w+\(\)|runScript\(.*|'
                 r'writeVar\(-?\d+, -?\d+\)|sc \d+ crc \w+|HALT.*|FATAL.*|QUIT.*)$')


def after(out):
    lines = out.splitlines()
    try:
        k = lines.index('SAVELOAD')
    except ValueError:
        return None
    return [ln for ln in lines[k + 1:] if PAT.match(ln)]


def main():
    argv = sys.argv[1:]
    if '--' not in argv or len(argv) < 3:
        sys.exit(__doc__)
    i = argv.index('--')
    steps, rounds, args = argv[0], [int(x) for x in argv[1].split(',')], argv[i + 1:]
    game, rest = args[0], args[1:]
    bad = 0
    for n in rounds:
        save = os.path.join(HERE, '..', 'build', 'pc', f'saveload_{n}.sav')
        a = subprocess.run([EXE, game, steps] + rest + ['--save-at', str(n), save],
                           capture_output=True, text=True, encoding='latin-1')
        b = subprocess.run([EXE, game, steps] + rest + ['--load', save, str(n)],
                           capture_output=True, text=True, encoding='latin-1')
        la, lb = after(a.stdout), after(b.stdout)
        if la is None or lb is None:
            print(f'round {n}: no SAVELOAD ({"save" if la is None else "load"}): '
                  f'{(a.stderr if la is None else b.stderr).strip()[:200]}')
            bad += 1
            continue
        raw = open(save, 'rb').read()
        # the screens apart: a load clears the text and redraws everything (ScummVM's
        # loadState), so the picture may differ for a while; the logic may not
        before = [x for x in a.stdout.splitlines() if x.startswith('sc ') and int(x.split()[1]) < n]
        sa = before[-1:] + [x for x in la if x.startswith('sc ')]
        sb = [x for x in lb if x.startswith('sc ')]
        la = [x for x in la if not x.startswith('sc ')]
        lb = [x for x in lb if not x.startswith('sc ')]
        if sa or sb:
            ca = {}
            for x in sa:
                ca[max(n, int(x.split()[1]))] = x.split()[3]
            cb = {}
            for x in sb:
                cb[int(x.split()[1])] = x.split()[3]
            last_a = last_b = None
            same_from = None
            for r in range(n, int(steps)):
                last_a = ca.get(r, last_a)
                last_b = cb.get(r, last_b)
                if last_a == last_b:
                    if same_from is None:
                        same_from = r
                else:
                    same_from = None
            print(f'round {n}: the screen ' + (f'equal again from round {same_from}' if same_from is not None
                                                else 'never equal again'))
        k = next((j for j, (x, y) in enumerate(zip(la, lb)) if x != y), min(len(la), len(lb)))
        size = f'save {len(raw)} bytes (zlib {len(zlib.compress(raw, 9))})'
        if k == len(la) == len(lb):
            print(f'round {n}: identical for {k} lines after the load; {size}')
        else:
            bad += 1
            print(f'round {n}: first difference at line {k + 1} after the load '
                  f'(continuous {len(la)}, loaded {len(lb)}); {size}')
            for j in range(max(0, k - 4), k):
                print(f'     {la[j]}')
            print(f'   C {la[k] if k < len(la) else "(end)"}')
            print(f'   L {lb[k] if k < len(lb) else "(end)"}')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
