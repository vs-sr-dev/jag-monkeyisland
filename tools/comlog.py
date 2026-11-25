#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""Boot a ROM on the real Jaguar and print what it says on COM8.

    python tools/comlog.py run ROM.j64 [SECS]   boot the ROM, then listen (default 60 s)
    python tools/comlog.py listen [SECS]        listen only (0: until Ctrl+C)

One process does both: jaggd's run, then the serial port (two processes on
the GameDrive at once wedge it: JaguarDocs metodi/gamedrive_usb.md 6.1).
Lines go to the screen and to build/logs/<rom>_<hhmmss>.log.  (The flow is
JagSkyroads' tools/comlog.py, without its timing table.)
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)


def listen(secs, log_path):
    import jaggd
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    with open(log_path, 'w', encoding='latin-1') as log:
        for t, ln in jaggd.lines(secs):
            log.write(f'{t:8.3f} {ln}\n')
            log.flush()
            print(f'{t:7.2f}  {ln}', flush=True)
    print(f'log: {os.path.relpath(log_path, REPO)}')


def main(argv):
    if not argv:
        sys.exit(__doc__)
    stamp = time.strftime('%H%M%S')
    if argv[0] == 'run':
        import jaggd
        rom = argv[1]
        secs = int(argv[2]) if len(argv) > 2 else 60
        jaggd.run_rom(rom)
        name = os.path.splitext(os.path.basename(rom))[0]
        listen(secs, os.path.join(REPO, 'build', 'logs', f'{name}_{stamp}.log'))
    elif argv[0] == 'listen':
        secs = int(argv[1]) if len(argv) > 1 else 0
        listen(secs, os.path.join(REPO, 'build', 'logs', f'listen_{stamp}.log'))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
