#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""Compare a room sweep (tools/oracle_sweep.ps1) with scumm_pc, room by room.

    python tools/sweep_cmp.py [ROOMS] [--steps 4000] [--show N]

ROOMS as "1-99" or "1,5,7".  For each build/oracle/room<N>.out, scumm_pc runs boot param N and
the traces are compared as trace_cmp.py --vars does.  One line per room: OK (ScummVM's whole run
matched), the first difference, or our HALT.  --show N prints the context of room N's difference.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, 'build', 'pc', 'scumm_pc.exe')
PAT = re.compile(r'^(Script \d+, offset 0x[0-9a-f]+: \[[0-9A-F]+\] \w+\(\)|runScript\(.*|'
                 r'writeVar\(-?\d+, -?\d+\)|Loading room \d+)$')


def filt(text):
    out = []
    for ln in text.splitlines():
        ln = ln.rstrip('\r')
        if PAT.match(ln) or ln.startswith(('HALT', 'FATAL')):
            out.append(ln)
    return out


def rooms(spec):
    out = []
    for part in spec.split(','):
        if '-' in part:
            a, b = part.split('-')
            out += range(int(a), int(b) + 1)
        else:
            out.append(int(part))
    return out


def main():
    argv = sys.argv[1:]
    steps, show = 4000, None
    if '--steps' in argv:
        i = argv.index('--steps'); steps = int(argv[i + 1]); del argv[i:i + 2]
    if '--show' in argv:
        i = argv.index('--show'); show = int(argv[i + 1]); del argv[i:i + 2]
    spec = argv[0] if argv else '1-99'
    for n in rooms(spec):
        path = os.path.join(ROOT, 'build', 'oracle', f'room{n}.out')
        if not os.path.exists(path):
            continue
        ref = filt(open(path, encoding='latin-1', errors='replace').read())
        res = subprocess.run([EXE, os.path.join(ROOT, 'MI_1'), str(steps), '--bootparam', str(n),
                              '--soundcard', '0', '--debugmode', '1'],
                             capture_output=True, text=True, encoding='latin-1')
        ours = filt(res.stdout + res.stderr)
        k = 0
        while k < min(len(ref), len(ours)) and ref[k] == ours[k]:
            k += 1
        if k == len(ref):
            print(f'room {n:2}: OK      {len(ref):6} lines')
        else:
            got = ours[k] if k < len(ours) else '(ours ended: more steps?)'
            print(f'room {n:2}: line {k + 1:6}/{len(ref):6}  ScummVM: {ref[k]}\n'
                  f'{"":9}{"":21}ours:    {got}')
            if show == n:
                for i in range(max(0, k - 12), k):
                    print('    ' + ref[i])
                for i in range(k, min(k + 6, len(ref))):
                    print('  S ' + ref[i])
                for i in range(k, min(k + 6, len(ours))):
                    print('  O ' + ours[i])


if __name__ == '__main__':
    main()
