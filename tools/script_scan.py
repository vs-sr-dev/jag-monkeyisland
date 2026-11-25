#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""What in the game's scripts can stop the interpreter: a static scan of every script.

    python tools/script_scan.py [LISTINGS_DIR ...]      default: build/scripts/en build/scripts/it

The listings are descumm's, one per script (tools/script_dump.py).  The scan reports:
  - the opcodes the game uses that csrc/scumm/ops_v5.c does not implement (the table
    tools/gen_opcodes.py generates; an unimplemented opcode halts the ROM);
  - the paths of the interpreter that halt or stop today, or did until they were written,
    where the game reaches them (banners, quit, flobjects, image verbs, save/load...);
  - every sub-opcode the game uses per opcode family, to check against the C's cases.
The answers come from the data, not from what the runs happened to reach (docs/todo.md,
session 10).  Whether a path is reachable in play is read from the listing around it.
"""
import collections
import glob
import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# (name, regex on a listing line, what it means for the port)
PATHS = [
    ('flobject', r'loadFlObject', 'SO_LOAD_OBJECT: an object of another room (flobjects)'),
    ('systemOps', r'\(98\) systemOps', 'o5_systemOps (1 restart, 2 pause, 3 quit)'),
    ('saveLoadGame', r'\((22|A2)\) ', 'o4_saveLoadGame'),
    ('saveLoadVars', r'\(A7\) ', 'o4_saveLoadVars'),
    ('banner', r'\((14|94|D8|58)\)\s+print(Ego)?\(252,\[[^\]]*Text\(',
     'print(252, text): showMessageDialog, a banner waiting for a key'),
    ('verbImage', r'VerbOps\([^)]*\[[^\]]*\bImage\(', 'SO_VERB_IMAGE'),
    ('verbObject', r'SetToObject\(', 'the verb assigned an object'),
    ('erase', r'\.Erase\(|\bErase\(', 'SO_ERASE'),
    ('sayVoice', r'SayVoice', 'SO_SAY_VOICE'),
    ('unknown', r'Unknown|unknown[0-79]|^[^"]*\bx[0-9A-F]{2}\(', 'a sub-opcode descumm does not name'),
    ('textCode8', r'unknown8\(', "a text's code 8 (a verb's newline: drawString's case 8; "
     "descumm takes the next two characters for an argument)"),
    ('cursorImage', r'SetCursorImg|CursorImage|SetCursor\(', 'SO_CURSOR_IMAGE / SET'),
]

LINE = re.compile(r'\[([0-9A-F]+)\] \(([0-9A-F]{2})\)\s*(.*)')


def implemented_opcodes():
    argv, sys.argv = sys.argv, sys.argv[:1]  # gen_opcodes reads ScummVM's path from argv[1]
    import gen_opcodes
    sys.argv = argv
    ops = {i: None for i in range(256)}
    ops.update(gen_opcodes.table('script_v5.cpp', 'ScummEngine_v5::setupOpcodes'))
    ops.update(gen_opcodes.table('script_v4.cpp', 'ScummEngine_v4::setupOpcodes'))
    impl = set(re.findall(r'static void (o[45]_\w+)\(void\)',
                          open(os.path.join(REPO, 'csrc', 'scumm', 'ops_v5.c')).read()))
    return ops, impl


def scan(d):
    used = collections.Counter()               # opcode byte -> lines
    sites = collections.defaultdict(list)      # path -> [(script, offset, text)]
    subops = collections.defaultdict(collections.Counter)
    for f in sorted(glob.glob(os.path.join(d, '*.txt'))):
        name = os.path.basename(f)[:-4]
        for ln in open(f, encoding='latin1'):
            m = LINE.match(ln.rstrip('\n'))
            if not m:
                continue
            off, op, text = m.groups()
            if op == '**':
                continue
            used[int(op, 16)] += 1
            for key, rx, _ in PATHS:
                if re.search(rx, ln):
                    sites[key].append((name, off, text.strip()[:110]))
            calls = re.findall(r'([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)?)\(', text)
            if calls:
                fam = calls[0]
                if not fam.startswith(('Var', 'Local', 'Bit')):
                    subops[f'{op} (statements)'][fam] += 1
                for c in calls[1:]:
                    if not c.startswith(('Var', 'Local', 'Bit', 'get')):
                        subops[f'{op} {fam}'][c] += 1
    return used, sites, subops


def main():
    dirs = sys.argv[1:] or [os.path.join(REPO, 'build', 'scripts', 'en'),
                            os.path.join(REPO, 'build', 'scripts', 'it')]
    ops, impl = implemented_opcodes()
    for d in dirs:
        if not glob.glob(os.path.join(d, '*.txt')):
            sys.exit(f'{d}: no listings (tools/script_dump.py GAME_DIR {d} descumm.exe)')
        used, sites, subops = scan(d)
        print(f'== {d}: {len(glob.glob(os.path.join(d, "*.txt")))} scripts')
        missing = sorted((op, ops[op]) for op in used if ops[op] not in impl)
        print('opcodes used and not implemented:', ', '.join(f'{op:02X} {n} ({used[op]})'
                                                            for op, n in missing) or 'none')
        for key, _, what in PATHS:
            s = sites.get(key, [])
            print(f'{key:13} {len(s):4}  {what}')
            for name, off, text in s[:12]:
                print(f'      {name} [{off}] {text}')
            if len(s) > 12:
                print(f'      ... {len(s) - 12} more')
        print('sub-opcodes by family:')
        for fam in sorted(subops):
            print(f'  {fam}: ' + ', '.join(f'{k} {v}' for k, v in sorted(subops[fam].items())))
        print()


if __name__ == '__main__':
    main()
