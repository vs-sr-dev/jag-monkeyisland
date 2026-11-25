#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""Compare the interpreter's opcode trace with ScummVM's.

    python tools/trace_cmp.py SCUMMVM_STDOUT OURS_STDOUT [CONTEXT] [--vars]

Both files are filtered to the lines ScummVM prints for DEBUG_OPCODES and DEBUG_SCRIPTS
("Script N, offset 0xX: [OP] name()" and "runScript(...) from ..."); the first line that
differs is shown with CONTEXT lines (default 8) before it.  Exit status 0 when our trace is a
prefix of ScummVM's (it ran fewer steps), 1 at the first difference.
--vars also compares the values written through writeVar (ScummVM's DEBUG_VARS lines).
"""
import re
import sys

PAT = re.compile(r'^(Script \d+, offset 0x[0-9a-f]+: \[[0-9A-F]+\] \w+\(\)|runScript\(.*)$')
VARS = re.compile(r'^writeVar\(-?\d+, -?\d+\)$')
use_vars = '--vars' in sys.argv
if use_vars:
    sys.argv.remove('--vars')


def lines(path):
    out = []
    for ln in open(path, encoding='latin-1', errors='replace'):
        ln = ln.rstrip('\r\n')
        if PAT.match(ln) or (use_vars and VARS.match(ln)):
            out.append(ln)
        elif ln.startswith(('HALT', 'FATAL')):
            out.append(ln)
    return out


def main():
    ref, ours = lines(sys.argv[1]), lines(sys.argv[2])
    ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    n = 0
    for n, (a, b) in enumerate(zip(ref, ours)):
        if a != b:
            print(f'trace_cmp: first difference at line {n + 1} (ScummVM {len(ref)} lines, ours {len(ours)})')
            for i in range(max(0, n - ctx), n):
                print(f'   {ref[i]}')
            print(f' S {a}')
            print(f' J {b}')
            for i in range(n + 1, min(len(ref), n + 4)):
                print(f' S+ {ref[i]}')
            for i in range(n + 1, min(len(ours), n + 4)):
                print(f' J+ {ours[i]}')
            sys.exit(1)
    m = min(len(ref), len(ours))
    print(f'trace_cmp: identical for {m} lines (ScummVM {len(ref)}, ours {len(ours)})')
    if len(ours) > m:
        print(f'  ours continues: {ours[m]}')
    sys.exit(0)


if __name__ == '__main__':
    main()
