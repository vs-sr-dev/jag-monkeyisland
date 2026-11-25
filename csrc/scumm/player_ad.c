/* SPDX-License-Identifier: GPL-2.0-or-later
 * Transcribed from ScummVM (https://www.scummvm.org), the legal property of its
 * developers; the port's parts Copyright (c) 2026 Samuele Voltan.  See COPYING. */
/*
 * player_ad.c - the AdLib player of the original interpreter: music and sound effects on one
 * OPL2, register by register.
 *
 * ScummVM engines/scumm/players/player_ad.cpp (Player_AD), transcribed function by function.
 * ScummVM uses that class for Indy3 and Loom only and plays Monkey Island through iMUSE (the
 * AD chunks converted to MIDI), because iMUSE lets it offer multi-MIDI; but Player_AD is the
 * reverse-engineered original driver, and its own comment says the original used it for Monkey
 * Island DOS EGA/VGA too.  The data agree: every MI1 AD chunk, both languages, is exactly the
 * grammar parseCommand reads (note on/off and end of track, no tempo or rhythm events, VLQs of
 * at most 2 bytes) and every effect uses at most 3 channels (docs/todo.md, the survey).  So the
 * port plays what the original played, with the original's song timer (VAR_MUSIC_TIMER).
 *
 * Left out, ScummVM-only: the volume scaling in writeReg (the original has none: full volume
 * leaves the values unchanged), setupVolume's rewrite of the levels, seeking for savegames.  The
 * mutex is the platform's (plat_ad_lock), held where ScummVM holds _mutex: the Jaguar runs
 * ad_onTimer from its video interrupt, and leaves it alone while the main loop is inside.
 * ad_onTimer is the 472 Hz callback (AD_CALLBACK_FREQUENCY): the platform calls it.
 */
#include <string.h>
#include "scumm.h"

#define AD_NUM_HW 9

enum { NOTE_PREINIT = -1, NOTE_ATTACK = 0, NOTE_DECAY = 1, NOTE_SUSTAIN = 2,
       NOTE_RELEASE = 3, NOTE_OFF = 4 };
enum { CH_OFF = 0, CH_PARSE = 1, CH_PLAY = 2 };

typedef struct {
    int state, playTime, sustainTimer, instrumentValue, bias, preIncrease, adjust;
    struct { int stepIncrease, step, stepCounter, timer; } envelope;
} Note;

typedef struct {
    int state;
    const uint8_t *currentOffset, *startOffset;
    uint8_t instrumentData[7];
    Note notes[2];
    int hardwareChannel;
} Channel;

typedef struct SfxSlot {
    int resource, priority;
    Channel channels[3];
} SfxSlot;

static struct {
    int musicResource;
    int32_t engineMusicTimer;

    struct { int allocated, priority; SfxSlot *sfxOwner; } hw[AD_NUM_HW];
    int numHWChannels;
    uint8_t reg[256];                     /* _registerBackUpTable */

    const uint8_t *musicData;
    unsigned timerLimit, musicTicks;
    uint32_t musicTimer, internalMusicTimer;
    int loopFlag;
    unsigned musicLoopStart;
    unsigned instrumentOffset[16];
    struct { unsigned lastEvent, frequency, b0Reg; } voice[AD_NUM_HW];
    unsigned mdvdrState;
    uint32_t curOffset, nextEventTimer;

    SfxSlot sfx[3];
    int sfxTimer;
    uint8_t rndSeed;
} ad;

static const int operatorOffsetTable[18] = {
    0, 3, 1, 4, 2, 5, 8, 11, 9, 12, 10, 13, 16, 19, 17, 20, 18, 21
};
static const unsigned noteFrequencies[12] = {
    0x200, 0x21E, 0x23F, 0x261, 0x285, 0x2AB, 0x2D4, 0x300, 0x32E, 0x35E, 0x390, 0x3C7
};
static const unsigned mdvdrTable[6] = { 0x00, 0x10, 0x08, 0x04, 0x02, 0x01 };
static const unsigned rhythmOperatorTable[6] = { 0x00, 0x00, 0x14, 0x12, 0x15, 0x11 };
static const unsigned rhythmChannelTable[6] = { 0x00, 0x00, 0x07, 0x08, 0x08, 0x07 };
static const unsigned noteBiasTable[7] = { 0x00, 0x00, 0x3F, 0x00, 0x3F, 0x00, 0x00 };
static const unsigned numStepsTable[16] = {
    1, 4, 6, 8, 10, 14, 18, 24, 36, 64, 100, 160, 240, 340, 600, 1200
};
static const unsigned noteAdjustScaleTable[7] = { 255, 7, 63, 15, 63, 15, 63 };
static const unsigned noteAdjustTable[16] = {
    0, 4369, 8738, 13107, 17476, 21845, 26214, 30583,
    34952, 39321, 43690, 48059, 52428, 56797, 61166, 65535
};
static const uint8_t useOperatorTable[7] = { 0, 0, 1, 1, 1, 1, 0 };
static const unsigned channelOffsetTable[11] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 7 };
static const unsigned channelOperatorOffsetTable[7] = { 0, 0, 1, 1, 0, 0, 0 };
static const unsigned baseRegisterTable[7] = { 0xA0, 0xC0, 0x40, 0x20, 0x40, 0x20, 0x00 };
static const unsigned registerMaskTable[7] = { 0xFF, 0x0E, 0x3F, 0x0F, 0x3F, 0x0F, 0x00 };
static const unsigned registerShiftTable[7] = { 0, 1, 0, 0, 0, 0, 0 };

static void stopSfx(SfxSlot *sfx);

/* --- registers ----------------------------------------------------------------------- */
static int isSeeking;                     /* _isSeeking: musicSeekTo (a load) */

/* writeReg: the backup table keeps the value; a key on of the music is muted while seeking
 * (the volumes are full here: ScummVM's scaling and its other mutes do nothing) */
static void writeReg(int r, int v)
{
    ad.reg[r & 0xFF] = (uint8_t)v;
    if (r >= 0xB0 && r <= 0xB8 && isSeeking && !ad.hw[r - 0xB0].sfxOwner)
        v &= ~0x20;
    plat_opl_write(r, v & 0xFF);
}

static uint8_t readReg(int r)
{
    return ad.reg[r & 0xFF];
}

/* --- hardware channels --------------------------------------------------------------- */
static int allocateHWChannel(int priority, SfxSlot *owner)
{
    int channel = -1, minPrio = priority, i;
    for (i = 0; i < ad.numHWChannels; ++i) {
        if (!ad.hw[i].allocated) {
            channel = i;
            break;
        }
        /* an effect never takes its own channels, music never takes music channels */
        if (ad.hw[i].priority <= minPrio && ad.hw[i].sfxOwner != owner) {
            minPrio = ad.hw[i].priority;
            channel = i;
        }
    }
    if (channel != -1) {
        if (ad.hw[channel].allocated && ad.hw[channel].sfxOwner)
            stopSfx(ad.hw[channel].sfxOwner);
        ad.hw[channel].allocated = 1;
        ad.hw[channel].priority = priority;
        ad.hw[channel].sfxOwner = owner;
    }
    return channel;
}

static void freeHWChannel(int channel)
{
    ad.hw[channel].allocated = 0;
    ad.hw[channel].sfxOwner = 0;
}

static void limitHWChannels(int newCount)
{
    int i;
    for (i = newCount; i < AD_NUM_HW; ++i)
        if (ad.hw[i].allocated)
            freeHWChannel(i);
    ad.numHWChannels = newCount;
}

/* --- instruments --------------------------------------------------------------------- */
static void setupOperator(unsigned opr, const uint8_t **instr)
{
    writeReg(0x20 + opr, *(*instr)++);
    writeReg(0x40 + opr, *(*instr)++);
    writeReg(0x60 + opr, *(*instr)++);
    writeReg(0x80 + opr, *(*instr)++);
    writeReg(0xE0 + opr, *(*instr)++);
}

static void setupChannel(unsigned channel, const uint8_t *instrOffset)
{
    instrOffset += 2;
    writeReg(0xC0 + channel, *instrOffset++);
    setupOperator(operatorOffsetTable[channel * 2 + 0], &instrOffset);
    setupOperator(operatorOffsetTable[channel * 2 + 1], &instrOffset);
}

/* --- music --------------------------------------------------------------------------- */
static void freeVoiceChannel(unsigned channel)
{
    freeHWChannel(channel);
    ad.voice[channel].lastEvent = 0;
    ad.voice[channel].b0Reg = 0;
    ad.voice[channel].frequency = 0;
}

static void noteOff(unsigned channel)
{
    writeReg(0xB0 + channel, ad.voice[channel].b0Reg & 0xDF);
    freeVoiceChannel(channel);
}

static void startMusic(void)
{
    int hasRhythmData = 0;
    unsigned instruments = ad.musicData[10], i;

    memset(ad.instrumentOffset, 0, sizeof(ad.instrumentOffset));
    for (i = 0; i < instruments; ++i) {
        int instrIndex = ad.musicData[11 + i] - 1;
        if (0 <= instrIndex && instrIndex < 16) {
            ad.instrumentOffset[instrIndex] = i * 16 + 16 + 3;
            hasRhythmData |= (ad.musicData[ad.instrumentOffset[instrIndex] + 13] != 0);
        }
    }
    if (hasRhythmData) {
        ad.mdvdrState = 0x20;
        limitHWChannels(6);
    } else {
        ad.mdvdrState = 0;
        limitHWChannels(9);
    }

    ad.curOffset = 0x93;
    ad.nextEventTimer = 40;
    ad.engineMusicTimer = 0;
    ad.internalMusicTimer = 0;
    ad.musicTimer = 0;

    writeReg(0xBD, (int)ad.mdvdrState);

    ad.timerLimit = 256;                  /* Loom: 473 */
    ad.musicTicks = ad.musicData[3];      /* Loom: x 2 */
    ad.loopFlag = (ad.musicData[4] == 0);
    ad.musicLoopStart = ad.curOffset + rd16(ad.musicData + 5);
}

static void stopMusic(void)
{
    int i;
    if (ad.musicResource == -1)
        return;
    ad.musicResource = -1;
    ad.curOffset = 0;
    for (i = 0; i < AD_NUM_HW; ++i)
        if (ad.voice[i].lastEvent)
            noteOff((unsigned)i);
    writeReg(0xBD, 0x00);
    limitHWChannels(9);
}

static void setupFrequency(unsigned channel, int8_t frequency)
{
    unsigned octave = 0, noteFrequency;
    frequency = (int8_t)(frequency - 31);
    if (frequency < 0)
        frequency = 0;
    while (frequency >= 12) {
        frequency = (int8_t)(frequency - 12);
        ++octave;
    }
    noteFrequency = noteFrequencies[(int)frequency];
    octave <<= 2;
    octave |= noteFrequency >> 8;
    octave |= 0x20;
    writeReg(0xA0 + channel, noteFrequency & 0xFF);
    ad.voice[channel].b0Reg = octave;
    writeReg(0xB0 + channel, (int)octave);
}

static void setupRhythm(unsigned rhythmInstr, unsigned instrOffset)
{
    if (rhythmInstr == 1) {
        setupChannel(6, ad.musicData + instrOffset);
        writeReg(0xA6, ad.musicData[instrOffset++]);
        writeReg(0xB6, ad.musicData[instrOffset] & 0xDF);
        ad.mdvdrState |= 0x10;
        writeReg(0xBD, (int)ad.mdvdrState);
    } else if (rhythmInstr < 6) {
        const uint8_t *secondOperatorOffset = ad.musicData + instrOffset + 8;
        setupOperator(rhythmOperatorTable[rhythmInstr], &secondOperatorOffset);
        writeReg(0xA0 + rhythmChannelTable[rhythmInstr], ad.musicData[instrOffset++]);
        writeReg(0xB0 + rhythmChannelTable[rhythmInstr], ad.musicData[instrOffset++] & 0xDF);
        writeReg(0xC0 + rhythmChannelTable[rhythmInstr], ad.musicData[instrOffset]);
        ad.mdvdrState |= mdvdrTable[rhythmInstr];
        writeReg(0xBD, (int)ad.mdvdrState);
    }
}

/* One event; 1 at the end of the track (looped or stopped). */
static int parseCommand(void)
{
    unsigned command = ad.musicData[ad.curOffset++];
    if (command == 0xFF) {
        command = ad.musicData[ad.curOffset++];
        if (command == 47) {                          /* end of track */
            if (ad.loopFlag) {
                ad.curOffset = ad.musicLoopStart;
                ad.nextEventTimer = 0;
            } else {
                stopMusic();
            }
            return 1;
        } else if (command == 88) {                   /* debug information */
            ad.curOffset += 5;
        } else if (command == 81) {                   /* tempo: Loom only */
            unsigned timing = ad.musicData[ad.curOffset + 2] | (ad.musicData[ad.curOffset + 1] << 8);
            ad.musicTicks = 0x73000 / timing;
            command = ad.musicData[ad.curOffset++];
            ad.curOffset += command;
        } else {
            command = ad.musicData[ad.curOffset++];
            ad.curOffset += command;
        }
    } else {
        if (command >= 0x90) {                        /* note on */
            unsigned instrOffset;
            command -= 0x90;
            instrOffset = ad.instrumentOffset[command & 15];   /* MI1: 0x90-0x97 only */
            if (instrOffset) {
                if (ad.musicData[instrOffset + 13] != 0) {
                    setupRhythm(ad.musicData[instrOffset + 13], instrOffset);
                } else {
                    int channel = allocateHWChannel(256, 0);
                    if (channel != -1) {
                        setupChannel((unsigned)channel, ad.musicData + instrOffset);
                        ad.voice[channel].lastEvent = command + 0x90;
                        ad.voice[channel].frequency = ad.musicData[ad.curOffset];
                        setupFrequency((unsigned)channel, (int8_t)ad.musicData[ad.curOffset]);
                    }
                }
            }
        } else {                                      /* note off */
            unsigned note = ad.musicData[ad.curOffset], channel = 0xFF;
            int i;
            command += 0x10;
            for (i = 0; i < AD_NUM_HW; ++i)
                if (ad.voice[i].frequency == note && ad.voice[i].lastEvent == command) {
                    channel = (unsigned)i;
                    break;
                }
            if (channel != 0xFF) {
                noteOff(channel);
            } else {
                unsigned instrOffset;
                command -= 0x90;
                instrOffset = ad.instrumentOffset[command & 15];
                if (instrOffset && ad.musicData[instrOffset + 13] != 0) {
                    unsigned rhythmInstr = ad.musicData[instrOffset + 13];
                    if (rhythmInstr < 6) {
                        ad.mdvdrState &= mdvdrTable[rhythmInstr] ^ 0xFF;
                        writeReg(0xBD, (int)ad.mdvdrState);
                    }
                }
            }
        }
        ad.curOffset += 2;
    }
    return 0;
}

static unsigned parseVLQ(void)
{
    unsigned vlq = ad.musicData[ad.curOffset++];
    if (vlq & 0x80) {
        vlq -= 0x80;
        vlq <<= 7;
        vlq |= ad.musicData[ad.curOffset++];
    }
    return vlq;
}

static void updateMusic(void)
{
    ad.musicTimer += ad.musicTicks;
    if (ad.musicTimer < ad.timerLimit)
        return;
    ad.musicTimer -= ad.timerLimit;

    ++ad.internalMusicTimer;
    if (ad.internalMusicTimer > 120) {
        ad.internalMusicTimer = 0;
        ++ad.engineMusicTimer;
    }

    --ad.nextEventTimer;
    if (ad.nextEventTimer)
        return;

    for (;;) {
        if (parseCommand()) {
            /* end of track: stopped, or looped and the next command follows at once */
            if (ad.musicResource == -1)
                return;
            continue;
        }
        if (ad.musicData[ad.curOffset] != 0)
            break;
        ++ad.curOffset;
    }

    ad.nextEventTimer = parseVLQ();
    ad.nextEventTimer >>= 1;                          /* Loom: >> 2 */
    if (!ad.nextEventTimer)
        ad.nextEventTimer = 1;
}

/* --- sound effects ------------------------------------------------------------------- */
static uint8_t getRnd(void)
{
    if (ad.rndSeed & 1) {
        ad.rndSeed >>= 1;
        ad.rndSeed ^= 0xB8;
    } else {
        ad.rndSeed >>= 1;
    }
    return ad.rndSeed;
}

static void clearChannel(const Channel *channel)
{
    writeReg(0xA0 + channel->hardwareChannel, 0x00);
    writeReg(0xB0 + channel->hardwareChannel, 0x00);
}

static void stopSfx(SfxSlot *sfx)
{
    int i;
    if (sfx->resource == -1)
        return;
    for (i = 0; i < 3; ++i) {
        if (sfx->channels[i].state) {
            clearChannel(&sfx->channels[i]);
            sfx->channels[i].state = CH_OFF;
        }
        if (sfx->channels[i].hardwareChannel != -1) {
            freeHWChannel(sfx->channels[i].hardwareChannel);
            sfx->channels[i].hardwareChannel = -1;
        }
    }
    sfx->resource = -1;
}

static SfxSlot *allocateSfxSlot(int priority)
{
    SfxSlot *sfx = 0;
    int minPrio = priority, i;
    for (i = 0; i < 3; ++i) {
        if (ad.sfx[i].resource == -1)
            return &ad.sfx[i];
        else if (ad.sfx[i].priority <= minPrio) {
            minPrio = ad.sfx[i].priority;
            sfx = &ad.sfx[i];
        }
    }
    if (sfx)
        stopSfx(sfx);
    return sfx;
}

static int startSfx(SfxSlot *sfx, const uint8_t *resource)
{
    const uint8_t *bufferPosition = resource + 2;
    int curChannel = 1;
    uint8_t command;

    writeReg(0xBD, 0x00);

    sfx->channels[0].state = CH_OFF;
    sfx->channels[1].state = CH_OFF;
    sfx->channels[2].state = CH_OFF;

    sfx->channels[0].hardwareChannel = allocateHWChannel(sfx->priority, sfx);
    if (sfx->channels[0].hardwareChannel == -1)
        return 0;
    sfx->channels[0].currentOffset = sfx->channels[0].startOffset = resource + 2;
    sfx->channels[0].state = CH_PARSE;

    while ((command = *bufferPosition) != 0xFF) {
        switch (command) {
        case 1:    bufferPosition += 15; break;
        case 2:    bufferPosition += 11; break;
        case 0x80: bufferPosition += 1; break;
        default:
            bufferPosition += 1;
            if (curChannel >= 3) {
                plat_error("AD: an effect with more than 3 channels");
                return 0;
            }
            sfx->channels[curChannel].hardwareChannel = allocateHWChannel(sfx->priority, sfx);
            if (sfx->channels[curChannel].hardwareChannel == -1)
                return 0;
            sfx->channels[curChannel].currentOffset = bufferPosition;
            sfx->channels[curChannel].startOffset = bufferPosition;
            sfx->channels[curChannel].state = CH_PARSE;
            ++curChannel;
            break;
        }
    }
    return 1;
}

static uint8_t readRegisterSpecial(int channel, uint8_t defaultValue, int offset)
{
    uint8_t regNum, regValue;
    if (offset == 6)
        return 0;
    if (useOperatorTable[offset])
        regNum = (uint8_t)operatorOffsetTable[channelOperatorOffsetTable[offset] + channel * 2];
    else
        regNum = (uint8_t)channelOffsetTable[channel];
    regNum = (uint8_t)(regNum + baseRegisterTable[offset]);
    regValue = defaultValue ? defaultValue : readReg(regNum);
    regValue &= registerMaskTable[offset];
    regValue >>= registerShiftTable[offset];
    return regValue;
}

static void writeRegisterSpecial(int channel, uint8_t value, int offset)
{
    uint8_t regNum, regValue;
    if (offset == 6)
        return;
    if (useOperatorTable[offset])
        regNum = (uint8_t)operatorOffsetTable[channelOperatorOffsetTable[offset] + channel * 2];
    else
        regNum = (uint8_t)channelOffsetTable[channel];
    regNum = (uint8_t)(regNum + baseRegisterTable[offset]);
    regValue = (uint8_t)(readReg(regNum) & ~registerMaskTable[offset]);
    regValue = (uint8_t)(regValue | (value << registerShiftTable[offset]));
    writeReg(regNum, regValue);
}

static void setupNoteEnvelopeState(Note *note, int steps, int adjust)
{
    note->preIncrease = 0;
    if ((adjust < 0 ? -adjust : adjust) > steps) {
        int r = adjust % steps;
        note->preIncrease = 1;
        note->adjust = adjust / steps;
        note->envelope.stepIncrease = r < 0 ? -r : r;
    } else {
        note->adjust = adjust;
        note->envelope.stepIncrease = adjust < 0 ? -adjust : adjust;
    }
    note->envelope.step = steps;
    note->envelope.stepCounter = 0;
    note->envelope.timer = steps;
}

static int processNoteEnvelope(Note *note)
{
    if (note->preIncrease)
        note->instrumentValue += note->adjust;
    note->envelope.stepCounter += note->envelope.stepIncrease;
    if (note->envelope.stepCounter >= note->envelope.step) {
        note->envelope.stepCounter -= note->envelope.step;
        if (note->adjust < 0)
            --note->instrumentValue;
        else
            ++note->instrumentValue;
    }
    return --note->envelope.timer ? 0 : 1;
}

/* 1 when the note's envelope has run out (state Off) */
static int processNote(Note *note, const Channel *channel, const uint8_t *offset)
{
    int instrumentDataOffset;
    uint8_t instrumentDataValue = 0, noteInstrumentValue;

    if (++note->state == NOTE_OFF)
        return 1;

    instrumentDataOffset = *offset & 0x07;
    note->bias = (int)noteBiasTable[instrumentDataOffset];

    if (note->state == NOTE_ATTACK)
        instrumentDataValue = channel->instrumentData[instrumentDataOffset];

    noteInstrumentValue = readRegisterSpecial(channel->hardwareChannel, instrumentDataValue,
                                              instrumentDataOffset);
    if (note->bias)
        noteInstrumentValue = (uint8_t)(note->bias - noteInstrumentValue);
    note->instrumentValue = noteInstrumentValue;

    if (note->state == NOTE_SUSTAIN) {
        note->sustainTimer = (int)numStepsTable[*(offset + 3) >> 4];
        if (*offset & 0x40)
            note->sustainTimer = (((getRnd() << 8) * note->sustainTimer) >> 16) + 1;
    } else {
        int timer1, timer2, adjustValue;
        if (note->state == NOTE_RELEASE) {
            timer1 = *(offset + 3) & 0x0F;
            timer2 = 0;
        } else {
            timer1 = *(offset + note->state + 1) >> 4;
            timer2 = *(offset + note->state + 1) & 0x0F;
        }
        adjustValue = (int)((noteAdjustTable[timer2] * noteAdjustScaleTable[instrumentDataOffset]) >> 16)
                      - noteInstrumentValue;
        setupNoteEnvelopeState(note, (int)numStepsTable[timer1], adjustValue);
    }
    return 0;
}

static void noteOffOn(int channel)
{
    uint8_t regValue = readReg(0xB0 | channel);
    writeReg(0xB0 | channel, regValue & 0xDF);
    writeReg(0xB0 | channel, regValue | 0x20);
}

static void parseNote(Note *note, const Channel *channel, const uint8_t *offset)
{
    if (*offset & 0x80) {
        note->state = NOTE_PREINIT;
        processNote(note, channel, offset);
        note->playTime = 0;
        if (*offset & 0x20) {
            note->playTime = (*(offset + 4) >> 4) * 118;
            note->playTime += (*(offset + 4) & 0x0F) * 8;
        }
    }
}

static void parseSlot(Channel *channel)
{
    for (;;) {
        const uint8_t *curOffset = channel->currentOffset;
        switch (*curOffset) {
        case 1:
            ++curOffset;
            channel->instrumentData[0] = *(curOffset + 0);
            channel->instrumentData[1] = *(curOffset + 2);
            channel->instrumentData[2] = *(curOffset + 9);
            channel->instrumentData[3] = *(curOffset + 8);
            channel->instrumentData[4] = *(curOffset + 4);
            channel->instrumentData[5] = *(curOffset + 3);
            channel->instrumentData[6] = 0;
            setupChannel((unsigned)channel->hardwareChannel, curOffset);
            writeReg(0xA0 + channel->hardwareChannel, *(curOffset + 0));
            writeReg(0xB0 + channel->hardwareChannel, *(curOffset + 1) & 0xDF);
            channel->currentOffset += 15;
            break;
        case 2:
            ++curOffset;
            channel->state = CH_PLAY;
            noteOffOn(channel->hardwareChannel);
            parseNote(&channel->notes[0], channel, curOffset + 0);
            parseNote(&channel->notes[1], channel, curOffset + 5);
            return;
        case 0x80:
            channel->currentOffset = channel->startOffset;
            break;
        default:
            clearChannel(channel);
            channel->state = CH_OFF;
            return;
        }
    }
}

static void updateSlot(Channel *channel)
{
    const uint8_t *curOffset = channel->currentOffset + 1;
    int num;
    for (num = 0; num <= 1; ++num, curOffset += 5) {
        Note *note;
        int updateNote = 0;
        if (!(*curOffset & 0x80))
            continue;
        note = &channel->notes[num];
        if (note->state == NOTE_SUSTAIN) {
            if (!--note->sustainTimer)
                updateNote = 1;
        } else {
            updateNote = processNoteEnvelope(note);
            if (note->bias)
                writeRegisterSpecial(channel->hardwareChannel,
                                     (uint8_t)(note->bias - note->instrumentValue), *curOffset & 0x07);
            else
                writeRegisterSpecial(channel->hardwareChannel,
                                     (uint8_t)note->instrumentValue, *curOffset & 0x07);
        }
        if (updateNote) {
            if (processNote(note, channel, curOffset)) {
                if (!(*curOffset & 0x08)) {
                    channel->currentOffset += 11;
                    channel->state = CH_PARSE;
                    continue;
                } else if (*curOffset & 0x10) {
                    noteOffOn(channel->hardwareChannel);
                }
                note->state = NOTE_PREINIT;
                processNote(note, channel, curOffset);
            }
        }
        if ((*curOffset & 0x20) && !--note->playTime) {
            channel->currentOffset += 11;
            channel->state = CH_PARSE;
        }
    }
}

static void updateSfx(void)
{
    int i, j;
    if (--ad.sfxTimer)
        return;
    ad.sfxTimer = 4;
    for (i = 0; i < 3; ++i) {
        int hasActiveChannel = 0;
        if (ad.sfx[i].resource == -1)
            continue;
        for (j = 0; j < 3; ++j) {
            Channel *ch = &ad.sfx[i].channels[j];
            if (ch->state) {
                hasActiveChannel = 1;
                if (ch->state == CH_PARSE)
                    parseSlot(ch);
                else
                    updateSlot(ch);
            }
        }
        if (!hasActiveChannel)
            stopSfx(&ad.sfx[i]);
    }
}

/* --- the interface (MusicEngine) ----------------------------------------------------- */
void ad_init(void)
{
    plat_ad_lock(1);
    int i, j;
    memset(&ad, 0, sizeof(ad));
    writeReg(0x01, 0x00);
    writeReg(0xBD, 0x00);
    writeReg(0x08, 0x00);
    writeReg(0x01, 0x20);
    ad.musicResource = -1;
    ad.sfxTimer = 4;
    ad.rndSeed = 1;
    for (i = 0; i < 3; ++i) {
        ad.sfx[i].resource = -1;
        for (j = 0; j < 3; ++j)
            ad.sfx[i].channels[j].hardwareChannel = -1;
    }
    ad.numHWChannels = AD_NUM_HW;
    plat_ad_lock(0);
}

/* res: the AD chunk's data (its 6-byte block header skipped), as readSoundResourceSmallHeader
 * stores it for Player_AD. */
static void startSound(int sound, const uint8_t *res);

void ad_startSound(int sound, const uint8_t *res)
{
    plat_ad_lock(1);
    startSound(sound, res);
    plat_ad_lock(0);
}

static void startSound(int sound, const uint8_t *res)
{
    if (res[2] == 0x80) {
        stopMusic();
        ad.musicResource = sound;
        ad.musicData = res;
        startMusic();
    } else {
        int i;
        SfxSlot *sfx = allocateSfxSlot(res[0]);       /* res[1], the channel, is ignored */
        if (!sfx)
            return;
        sfx->resource = sound;
        sfx->priority = res[0];
        if (!startSfx(sfx, res)) {
            sfx->resource = -1;
            for (i = 0; i < 3; ++i) {
                sfx->channels[i].state = CH_OFF;
                if (sfx->channels[i].hardwareChannel != -1) {
                    freeHWChannel(sfx->channels[i].hardwareChannel);
                    sfx->channels[i].hardwareChannel = -1;
                }
            }
        }
    }
}

void ad_stopSound(int sound)
{
    plat_ad_lock(1);
    int i;
    if (sound == ad.musicResource) {
        stopMusic();
    } else {
        for (i = 0; i < 3; ++i)
            if (ad.sfx[i].resource == sound)
                stopSfx(&ad.sfx[i]);
    }
    plat_ad_lock(0);
}

void ad_stopAllSounds(void)
{
    plat_ad_lock(1);
    int i;
    stopMusic();
    for (i = 0; i < 3; ++i)
        stopSfx(&ad.sfx[i]);
    plat_ad_lock(0);
}

int ad_getMusicTimer(void)
{
    return ad.engineMusicTimer;
}

int ad_getSoundStatus(int sound)
{
    int i;
    if (sound == ad.musicResource)
        return 1;
    for (i = 0; i < 3; ++i)
        if (ad.sfx[i].resource == sound)
            return 1;
    return 0;
}

void ad_onTimer(void)
{
    if (ad.curOffset)
        updateMusic();
    updateSfx();
}

/* --- save and load (Player_AD::saveLoadWithSerializer, musicSeekTo) ------------------------ */

/* musicSeekTo: the music parsed from its start to the position, its key ons muted, then the
 * notes that were on turned on again (only for a load: no music timer moves) */
static void musicSeekTo(uint32_t position)
{
    int i;
    isSeeking = 1;
    while (ad.curOffset != position) {
        if (parseCommand())
            break;                           /* an end of track: a position the song lacks */
        parseVLQ();
    }
    isSeeking = 0;
    for (i = 0; i < AD_NUM_HW; ++i)
        if (ad.voice[i].lastEvent != 0)
            writeReg(0xB0 + i, readReg(0xB0 + i));
}

void ad_saveLoad(Ser *s)
{
    int32_t res[4];
    uint32_t musicOffset, t;
    int i;
    plat_ad_lock(1);
    res[0] = ad.musicResource;
    for (i = 0; i < 3; i++)
        res[i + 1] = ad.sfx[i].resource;
    SER_ARR(s, res);
    if (s->loading && !g.soundcard) {        /* no AdLib (an oracle's run): no Player_AD in */
        uint32_t skip[5];                    /* ScummVM; the fields are read and dropped */
        SER_ARR(s, skip);
        plat_ad_lock(0);
        return;
    }
    if (s->loading && res[0] != -1 && res_sound_ad(res[0]))
        startSound(res[0], res_sound_ad(res[0]));
    musicOffset = ad.curOffset;
    SER(s, ad.engineMusicTimer);
    SER(s, ad.musicTimer);
    SER(s, ad.internalMusicTimer);
    SER(s, ad.curOffset);
    SER(s, ad.nextEventTimer);
    if (s->loading) {
        t = musicOffset;
        musicOffset = ad.curOffset;
        ad.curOffset = t;
        musicSeekTo(musicOffset);
        for (i = 1; i < 4; ++i)
            if (res[i] != -1 && res_sound_ad(res[i]))
                startSound(res[i], res_sound_ad(res[i]));
    }
    plat_ad_lock(0);
}
