#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""Monkey Island 1 (DOS VGA floppy) -> the cartridge's room pack.

    python tools/mi2jag.py [GAME_DIR]        default: MI_1 (the user's own copy, BYOA)

Writes cart/rooms.bin, read in place by the 68000 and the GPU (src/gpu_room.s):

    directory   128 longs: offset of room n's record from the start of rooms.bin, 0 = none
    record      (8-aligned, big-endian longs)
      +0  width          +4  height        +8  strips        +12 CLUT offset (64)
      +16 strip table offset (576)
      +20 H: inner step (1), inner count (8), outer step (w - 8), outer count (h)
      +36 V: inner step (w), inner count (h), outer step (1 - h*w), outer count (8)
      +52 name, 12 bytes, NUL-padded
      +64 CLUT, 256 RGB16 words (JaguarDocs: r5 b5 g6)
      +576 strip table, one long per strip: offset of the strip from the record
      strips: one header long (bits 0-7 colour-field width = codec % 10, bit 8 vertical),
              then the strip's bytes after the codec, zero-padded to a long and REVERSED in
              each group of four, so that a big-endian long load gives them least significant
              byte first: the GPU consumes the bit stream from bit 0 up, as ScummVM does.

The two direction blocks are the room's own numbers, so the GPU never multiplies: it walks a
strip with (inner step, inner count) inside (outer step, outer count), whatever the codec.
Only the two basic families (ZIGZAG_V 14-18, ZIGZAG_H 24-28) occur in room images
(docs/feasibility.md sec.8); anything else is a build error.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import scumm_v4 as sv4

DIR_ENTRIES = 128
CLUT_OFF = 64
TAB_OFF = CLUT_OFF + 512


def strip_blob(strip):
    code = strip[0]
    if code in sv4.ZIGZAG_V:
        vert = 1
    elif code in sv4.ZIGZAG_H:
        vert = 0
    else:
        raise SystemExit(f'codec {code} in a room image: the GPU decoder knows only 14-18, 24-28')
    data = bytearray(strip[1:])
    data += bytes(-len(data) % 4)
    for i in range(0, len(data), 4):
        data[i:i + 4] = data[i:i + 4][::-1]
    return struct.pack('>I', (code % 10) | (vert << 8)) + bytes(data)


def room_record(room):
    w, h, n = room.w, room.h, room.w // 8
    hdr = struct.pack('>13i', w, h, n, CLUT_OFF, TAB_OFF,
                      1, 8, w - 8, h,
                      w, h, 1 - h * w, 8)
    hdr += room.name.encode('latin1')[:11].ljust(12, b'\0')
    assert len(hdr) == CLUT_OFF
    pal = room.palette
    clut = b''.join(struct.pack('>H', sv4.rgb16(pal[3 * i], pal[3 * i + 1], pal[3 * i + 2]))
                    for i in range(256))
    body, table = bytearray(), []
    pos = TAB_OFF + 4 * n
    for strip in room.strips():
        blob = strip_blob(strip)
        table.append(pos + len(body))
        body += blob
    rec = hdr + clut + struct.pack(f'>{n}I', *table) + bytes(body)
    return rec + bytes(-len(rec) % 8)


GAME_MAGIC = b'MIGM'
GAME_FILES = ['000.LFL', 'DISK01.LEC', 'DISK02.LEC', 'DISK03.LEC', 'DISK04.LEC',
              '901.LFL', '902.LFL', '903.LFL', '904.LFL']


def game_image(game_dir):
    """cart/game.bin: the game's own files for the interpreter (csrc/scumm/image.c), byte for
    byte but decrypted (the disks are XOR 0x69), each 32-byte aligned.  Header, big-endian:
    'MIGM', the file count, then (offset, size) per file of GAME_FILES; size 0 = absent."""
    hdr_len = 8 + 8 * len(GAME_FILES)
    body = bytearray(hdr_len + (-hdr_len % 32))
    table = []
    for i, name in enumerate(GAME_FILES):
        disk = int(name[5]) if name.startswith('DISK') else 1
        try:
            data = open(sv4._find(game_dir, name, disk), 'rb').read()
        except FileNotFoundError:
            if name.startswith('90'):
                table.append((0, 0))
                continue
            raise
        if name.endswith('.LEC'):
            data = bytes(b ^ sv4.XOR_KEY for b in data)
        table.append((len(body), len(data)))
        body += data
        body += bytes(-len(body) % 32)
    body[0:hdr_len] = GAME_MAGIC + struct.pack('>I', len(GAME_FILES)) + b''.join(
        struct.pack('>II', o, n) for o, n in table)
    with open(os.path.join(REPO, 'cart', 'game.bin'), 'wb') as f:
        f.write(body)
    print(f'mi2jag: game image {len(body):,} bytes -> cart/game.bin')


def main():
    game_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'MI_1')
    g = sv4.Game(game_dir)
    directory = [0] * DIR_ENTRIES
    pack = bytearray(4 * DIR_ENTRIES)
    shown = []
    for num in sorted(g.rooms):
        room = g.rooms[num]
        if not room.has_image:
            continue
        directory[num] = len(pack)
        pack += room_record(room)
        shown.append(num)
    pack[0:4 * DIR_ENTRIES] = struct.pack(f'>{DIR_ENTRIES}I', *directory)
    pack += bytes(-len(pack) % 32)
    os.makedirs(os.path.join(REPO, 'cart'), exist_ok=True)
    with open(os.path.join(REPO, 'cart', 'rooms.bin'), 'wb') as f:
        f.write(pack)
    big = max((g.rooms[n] for n in shown), key=lambda r: r.w * r.h)
    print(f'mi2jag: {len(shown)} rooms, {len(pack):,} bytes -> cart/rooms.bin; '
          f'largest room {big.num} ({big.name}) {big.w}x{big.h} = {big.w * big.h:,} bytes')
    # The room buffers (tools/memory_map.py): pitch = the Blitter's width for the room, rows =
    # the room's or 200, whichever is more (csrc/jag/jag_main.c plat_initBGBuffers).
    from memory_map import blit_width
    need = max(blit_width(g.rooms[n].w) * max(g.rooms[n].h, 200) for n in shown)
    with open(os.path.join(REPO, 'cart', 'rooms_max.txt'), 'w') as f:
        f.write(f'{need}\n')
    game_image(game_dir)


if __name__ == '__main__':
    main()
