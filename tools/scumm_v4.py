# SPDX-License-Identifier: GPL-2.0-or-later
# Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
# developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING.
"""Reader for Monkey Island 1 DOS VGA floppy data (SCUMM v4), and ScummVM's room decoder.

The game's own files are the input: 000.LFL (index, plain) and DISK01-04.LEC (XOR 0x69), either
flat in one folder or one disk per folder (d1..d4, as 7-Zip extracts the floppy images).

Block format (v4 "small header"): LE32 size, header included, then a 2-byte tag.
    LE > FO, LF;  LF = LE16 room number, then RO (the room) and its SC/SO/CO resources.
    RO > HD (LE16 width, height, objects), PA (LE16 768 + RGB 8-bit), BM (the image), ...
BM: LE32 length up to the z-planes, one LE32 offset per 8-pixel strip (relative to that
length field), the strips (first byte = codec), then the z-planes chained by their LE16 sizes.

decode_strip() is ScummVM's Gdi::drawStripBasicH / drawStripBasicV (engines/scumm/gfx.cpp,
GitHub master, November 2026), transcribed line by line: it is the oracle the Jaguar's GPU
decoder is checked against.  writeRoomColor goes through _roomPalette, the identity for this
game (the expected frames are checked by eye against ScummVM to confirm it).
"""
import os
import struct

XOR_KEY = 0x69

# Codec numbers (gfx.cpp decompressBitmap): code % 10 is the colour-field width.
ZIGZAG_V = range(14, 19)        # BMCOMP_ZIGZAG_V4..V8 -> drawStripBasicV
ZIGZAG_H = range(24, 29)        # BMCOMP_ZIGZAG_H4..H8 -> drawStripBasicH
RAW256 = 1                      # BMCOMP_RAW256 -> drawStripRaw (object images only)


def _find(d, name, disk):
    for p in (os.path.join(d, name), os.path.join(d, f'd{disk}', name)):
        if os.path.exists(p):
            return p
    raise FileNotFoundError(f'{name} not found in {d} (flat or d{disk}/)')


def blocks(b, off, end):
    """(offset, size, tag) of the blocks between off and end; stops at the first bad header."""
    while off + 6 <= end:
        size = struct.unpack_from('<I', b, off)[0]
        if size < 6 or off + size > end:
            return
        yield off, size, b[off + 4:off + 6].decode('latin1')
        off += size


class Room:
    def __init__(self, num, name, data, ro, rsize):
        self.num, self.name = num, name
        self.w = self.h = self.nobj = 0
        self.palette = None          # 768 bytes, RGB 8-bit
        self.bm = None               # (data, offset of the SMAP length field, end of BM)
        for o, size, tag in blocks(data, ro + 6, ro + rsize):
            if tag == 'HD':
                self.w, self.h, self.nobj = struct.unpack_from('<HHH', data, o + 6)
            elif tag == 'PA':
                n = struct.unpack_from('<H', data, o + 6)[0]
                self.palette = data[o + 8:o + 8 + n]
            elif tag == 'BM':
                self.bm = (data, o + 6, o + size)

    @property
    def has_image(self):
        return self.h > 0 and self.bm is not None

    def strips(self):
        """The raw bytes of each strip: [codec, data...], data running to the next strip."""
        data, sp, end = self.bm
        n = self.w // 8
        smaplen = struct.unpack_from('<I', data, sp)[0]
        offs = [struct.unpack_from('<I', data, sp + 4 + 4 * i)[0] for i in range(n)] + [smaplen]
        return [data[sp + offs[i]:sp + offs[i + 1]] for i in range(n)]


class Game:
    def __init__(self, d):
        self.dir = d
        idx = open(_find(d, '000.LFL', 1), 'rb').read()
        self.names = {}
        for o, size, tag in blocks(idx, 0, len(idx)):
            if tag == 'RN':
                p = o + 6
                while idx[p]:
                    raw = bytes(c ^ 0xFF for c in idx[p + 1:p + 10])
                    self.names[idx[p]] = raw.split(b'\0')[0].decode('latin1')
                    p += 10
        self.rooms = {}
        for disk in range(1, 5):
            raw = open(_find(d, f'DISK0{disk}.LEC', disk), 'rb').read()
            data = bytes(x ^ XOR_KEY for x in raw)
            for _, lsize, ltag in blocks(data, 0, len(data)):
                for o, size, tag in blocks(data, 6, lsize):
                    if tag != 'LF':
                        continue
                    num = struct.unpack_from('<H', data, o + 6)[0]
                    ro, rsize, rtag = next(blocks(data, o + 8, o + size))
                    assert rtag == 'RO', (num, rtag)
                    self.rooms[num] = Room(num, self.names.get(num, '?'), data, ro, rsize)


def decode_strip(strip, height):
    """ScummVM's drawStripBasicH/V on one strip -> 8 x height pixel indices, row-major."""
    code = strip[0]
    src = iter(strip[1:])
    shr = code % 10
    mask = 0xFF >> (8 - shr)
    out = bytearray(8 * height)
    if code in ZIGZAG_V:
        order = [y * 8 + x for x in range(8) for y in range(height)]
    elif code in ZIGZAG_H:
        order = [y * 8 + x for y in range(height) for x in range(8)]
    else:
        raise ValueError(f'codec {code} is not a basic (zigzag) codec')

    color = next(src)
    bits = next(src)
    cl = 8
    inc = -1

    def fill():
        nonlocal bits, cl
        if cl <= 8:
            bits |= next(src, 0) << cl
            cl += 8

    def read_bit():
        nonlocal bits, cl
        cl -= 1
        bit = bits & 1
        bits >>= 1
        return bit

    for pos in order:
        fill()
        out[pos] = color
        if not read_bit():
            pass
        elif not read_bit():
            fill()
            color = bits & mask
            bits >>= shr
            cl -= shr
            inc = -1
        elif not read_bit():
            color = (color + inc) & 0xFF
        else:
            inc = -inc
            color = (color + inc) & 0xFF
    return out


def decode_room(room):
    """The whole room image, w x h, 8 bpp, row-major."""
    img = bytearray(room.w * room.h)
    for s, strip in enumerate(room.strips()):
        px = decode_strip(strip, room.h)
        for y in range(room.h):
            img[y * room.w + s * 8:y * room.w + s * 8 + 8] = px[y * 8:y * 8 + 8]
    return img


def rgb16(r, g, b):
    """Jaguar RGB16: red 5 bits at the top, blue 5 in the middle, green 6 at the bottom."""
    return ((r >> 3) << 11) | ((b >> 3) << 6) | (g >> 2)


def rgb16_to_rgb(v):
    """What the Jaguar shows for an RGB16 entry, back in 8-bit RGB (for the expected frames)."""
    r, b, g = (v >> 11) & 31, (v >> 6) & 31, v & 63
    return (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)
