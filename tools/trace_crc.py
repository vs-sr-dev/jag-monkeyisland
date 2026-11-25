#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""Check the Jaguar's trace checkpoints (csrc/jag/jag_main.c) against a reference trace.

    python tools/trace_crc.py COM8_LOG REFERENCE [REFERENCE ...]
    python tools/trace_crc.py --opl COM8_LOG REFERENCE      (the AdLib driver's writes)

The Jaguar sends "T <lines> <crc>" every 256 trace lines: the running CRC32 of every line so
far, each followed by '\\n' (zlib.crc32).  A REFERENCE is a ScummVM oracle stdout or a scumm_pc
trace, filtered to the same lines (opcodes, runScript, writeVar, "Loading room").  Each
checkpoint is checked against every reference long enough to have it; the first failing
checkpoint brackets the first different line within 256.

--opl: the AdLib driver's register writes instead (csrc/jag/jag_audio.c, the trace phase):
the Jaguar's "A <lines> <crc>" against the "OPL tick reg val" lines of scumm_pc --opl
--soundcard 3 (the same ticks: VAR_TIMER_NEXT jiffies x 472 / 60 before each round).
"""
import re
import sys
import zlib

PAT = re.compile(r'^(Script \d+, offset 0x[0-9a-f]+: \[[0-9A-F]+\] \w+\(\)|runScript\(.*|'
                 r'writeVar\(-?\d+, -?\d+\)|Loading room \d+)$')
CHK = re.compile(r'\bT (\d+) ([0-9a-f]+)( end)?\s*$')


OPL_PAT = re.compile(r'^OPL \d+ [0-9A-F]{2} [0-9A-F]{2}$')
OPL_CHK = re.compile(r'\bA (\d+) ([0-9a-f]+)( end)?\s*$')


def ref_crcs(path):
    """{line count: crc} after every line of the filtered reference"""
    out, crc, n = {}, 0, 0
    for ln in open(path, encoding='latin-1', errors='replace'):
        ln = ln.rstrip('\r\n')
        if PAT.match(ln):
            crc = zlib.crc32((ln + '\n').encode('latin-1'), crc)
            n += 1
            out[n] = crc
    return out, n


def main():
    global PAT, CHK
    argv = sys.argv[1:]
    if argv and argv[0] == '--opl':
        PAT, CHK = OPL_PAT, OPL_CHK
        argv = argv[1:]
    if len(argv) < 2:
        sys.exit(__doc__)
    log, refs = argv[0], argv[1:]
    checks = []
    for ln in open(log, encoding='latin-1', errors='replace'):
        m = CHK.search(ln)
        if m:
            checks.append((int(m.group(1)), int(m.group(2), 16), bool(m.group(3))))
    if not checks:
        sys.exit(f'{log}: no "T <lines> <crc>" checkpoint')
    ok = True
    for ref in refs:
        crcs, total = ref_crcs(ref)
        good = bad = 0
        first_bad = None
        for n, crc, _end in checks:
            if n > total:
                continue
            if crcs[n] == crc:
                good += 1
            else:
                bad += 1
                if first_bad is None:
                    first_bad = n
        last = max((n for n, _c, _e in checks if n <= total), default=0)
        if bad:
            ok = False
            prev = max((n for n, _c, _e in checks if n < first_bad), default=0)
            print(f'{ref}: {good} checkpoints match, {bad} differ; the first different line is '
                  f'between {prev + 1} and {first_bad}')
        else:
            print(f'{ref}: all {good} checkpoints match, the first {last} lines '
                  f'(reference {total} lines, Jaguar {checks[-1][0]})')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
