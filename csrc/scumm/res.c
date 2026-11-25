/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * res.c - resources read in place from the game image (v4 "small header" blocks).
 *
 * A v4 resource is the block found at: its room's LF block + 8 (the LF header and the room
 * number) + the offset in the index directory; a room is the RO block itself (offset 0).
 * ScummVM: ScummEngine::loadResource (resource.cpp), the GF_SMALL_HEADER / version 4 branch.
 */
#include "scumm.h"

uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

const uint8_t *res_room_offs(int room, uint32_t offs)
{
    const GameImage *img = g.img;
    if (room <= 0 || room >= NUM_ROOMS || !img->room_lf[room])
        return 0;
    return img->disk[img->room_disk[room]] + img->room_lf[room] + 8 + offs;
}

const uint8_t *res_room(int room)
{
    return res_room_offs(room, 0);
}

const uint8_t *res_script(int n)
{
    if (n <= 0 || n >= NUM_GLOBAL_SCRIPTS)
        return 0;
    return res_room_offs(g.img->script_room[n], g.img->script_offs[n]);
}

/* a costume block, header included (GF_SMALL_HEADER: _baseptr = the block) */
const uint8_t *res_costume(int n)
{
    if (n <= 0 || n >= 200)
        return 0;
    return res_room_offs(g.img->costume_room[n], g.img->costume_offs[n]);
}

/* findResourceData(tag, block) for small headers: the data of the first sub-block of 'block'
 * whose 2-byte tag matches (ScummVM maps RMHD->HD, EXCD->EX, ... before searching). */
const uint8_t *res_find_data(const uint8_t *block, const char tag[2])
{
    uint32_t size = rd32(block), pos = 6;
    while (pos + 6 <= size) {
        const uint8_t *p = block + pos;
        uint32_t s = rd32(p);
        if (s < 6)
            break;
        if (p[4] == (uint8_t)tag[0] && p[5] == (uint8_t)tag[1])
            return p + 6;
        pos += s;
    }
    return 0;
}
