/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * image.c - the game image: where every room, script, sound and costume of the game is.
 *
 * The platform hands over the game's own files, already decrypted (XOR 0x69 undone): the
 * index 000.LFL and the four disks DISKnn.LEC.  Nothing is copied: the PC passes the files it
 * read, the Jaguar passes the cartridge.  The interpreter reads every resource in place, so the
 * data must stay where it is and is never written.
 *
 * ScummVM resource_v4.cpp (readIndexFile: the 0R, 0S, 0N, 0C directories and the 0O object
 * table) and the LF blocks inside each disk's LE (room number -> offset), which ScummVM finds
 * through the room offsets of its index.
 */
#include "scumm.h"

static void read_dir(const uint8_t *p, uint8_t *rooms, uint32_t *offs, int max)
{
    int n = rd16(p), i;
    p += 2;
    for (i = 0; i < n && i < max; i++, p += 5) {
        rooms[i] = p[0];
        offs[i] = rd32(p + 1);
    }
}

/* 000.LFL: the directories of scripts, sounds and costumes, and the global objects */
void image_index(GameImage *img, const uint8_t *idx, uint32_t size)
{
    uint32_t pos;
    for (pos = 0; pos + 6 <= size;) {
        uint32_t s = rd32(idx + pos);
        const uint8_t *b = idx + pos + 6;
        if (idx[pos + 4] == '0') {
            switch (idx[pos + 5]) {
            case 'S': read_dir(b, img->script_room, img->script_offs, NUM_GLOBAL_SCRIPTS); break;
            case 'N': read_dir(b, img->sound_room, img->sound_offs, 200); break;
            case 'C': read_dir(b, img->costume_room, img->costume_offs, 200); break;
            case 'O': img->objects = b + 2; break;      /* read once, at scumm_init */
            default: break;                             /* 0R: the LF blocks say it too */
            }
        }
        if (s < 6)
            break;
        pos += s;
    }
}

/* DISKnn.LEC, decrypted: the LF blocks inside its LE, room number -> offset */
void image_disk(GameImage *img, int d, const uint8_t *data, uint32_t size)
{
    uint32_t pos;
    img->disk[d] = data;
    img->disk_size[d] = size;
    for (pos = 6; pos + 8 <= size;) {
        uint32_t s = rd32(data + pos);
        if (s < 6)
            break;
        if (data[pos + 4] == 'L' && data[pos + 5] == 'F') {
            int room = rd16(data + pos + 6);
            if (room > 0 && room < NUM_ROOMS) {
                img->room_disk[room] = (uint8_t)d;
                img->room_lf[room] = pos;
            }
        }
        pos += s;
    }
}
