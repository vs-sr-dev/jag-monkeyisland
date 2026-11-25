/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * sound.c - ScummVM's Sound (engines/scumm/sound.cpp): the queue the scripts fill
 * (startSound, addSoundToQueue), drained at the end of each loop newest first
 * (processSoundQueues, triggerSound), and the questions the scripts ask (isSoundRunning,
 * isSoundInQueue, updateMusicTimer).  The player behind it is the original AdLib driver
 * (player_ad.c); the sound resource handed to it is the AD chunk, found as
 * readSoundResourceSmallHeader finds it.
 *
 * With VAR_SOUNDCARD 0 (an oracle run against ScummVM with music_driver=null) there is no
 * player: a sound taken off the queue has finished and the music timer stays at 0, which is
 * what ScummVM's iMUSE does without a driver.
 */
#include "scumm.h"

#define SOUND_QUEUE 16

static int queue[SOUND_QUEUE];
static int queuePos;

static int has_player(void)
{
    return g.soundcard != 0;
}

/* readSoundResourceSmallHeader, the v4 path: the first AD block of the sound's SO block,
 * walking nested SO blocks flat (an outer SO's size may cover the next sound's blocks, so the
 * walk goes by the outer size, as ScummVM's does).  Returns the AD data, header skipped. */
const uint8_t *res_sound_ad(int sound)
{
    const uint8_t *so, *p;
    uint32_t total_size, pos, size;

    if (sound <= 0 || sound >= 200 || !g.img->sound_room[sound])
        return 0;
    so = res_room_offs(g.img->sound_room[sound], g.img->sound_offs[sound]);
    if (!so)
        return 0;
    total_size = rd32(so);
    if (so[4] == 'R' && so[5] == 'O')
        return 0;                             /* an MT-32-only resource */
    pos = 6;
    p = so + 6;
    while (pos < total_size) {
        size = rd32(p);
        if (size < 6)
            return 0;
        pos += size;
        if (p[4] == 'A' && p[5] == 'D')
            return p + 6;
        if (p[4] == 'S' && p[5] == 'O') {      /* nested: step inside it */
            pos -= size;
            size = 6;
            pos += 6;
        }
        p += size;
    }
    return 0;
}

/* Sound::startSound */
void sound_queue(int sound)
{
    VAR(VAR_LAST_SOUND) = sound;
    if (queuePos < SOUND_QUEUE)
        queue[queuePos++] = sound;
    else
        plat_error("sound queue overflow");
}

static void triggerSound(int sound)
{
    const uint8_t *ad = res_sound_ad(sound);
    if (ad)
        ad_startSound(sound, ad);
}

/* Sound::processSoundQueues (scummLoop_handleSound -> processSound) */
void sound_process(void)
{
    while (queuePos) {
        int snd = queue[--queuePos];
        if (snd && has_player())
            triggerSound(snd);
    }
}

static int isSoundInQueue(int sound)
{
    int i = queuePos;
    while (i--)
        if (queue[i] == sound)
            return 1;
    return 0;
}

/* Sound::isSoundRunning */
int sound_running(int sound)
{
    if (isSoundInQueue(sound))
        return 1;
    if (!has_player())
        return 0;
    return ad_getSoundStatus(sound);
}

/* Sound::stopSound */
void sound_stop(int sound)
{
    int i;
    if (has_player())
        ad_stopSound(sound);
    for (i = 0; i < SOUND_QUEUE; i++)
        if (queue[i] == sound)
            queue[i] = 0;
}

/* Sound::stopAllSounds */
void sound_stop_all(void)
{
    int i;
    queuePos = 0;
    for (i = 0; i < SOUND_QUEUE; i++)
        queue[i] = 0;
    if (has_player())
        ad_stopAllSounds();
}

/* Sound::updateMusicTimer: the player's song position, read once per loop */
void sound_updateMusicTimer(void)
{
    if (has_player())
        VAR(VAR_MUSIC_TIMER) = ad_getMusicTimer();
}

void sound_init(void)
{
    int i;
    queuePos = 0;
    for (i = 0; i < SOUND_QUEUE; i++)
        queue[i] = 0;
    if (has_player())
        ad_init();
}

/* --- save and load: the sound queue ------------------------------------------------------- */
void sound_saveLoad(Ser *s)
{
    SER_ARR(s, queue);
    SER(s, queuePos);
}
