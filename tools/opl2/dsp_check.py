#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""Jerry's OPL2 (src/dsp_audio.s) against its specification (tools/opl2/opl2_model.c).

    python tools/opl2/dsp_check.py [SECONDS] [ROOM]

JagSkyroads' check, then JagGOT's, for JaguarMI's ring.  Runs the REAL DSP binary (from
build/main.bin, located through build/main.lst) in tools/opl2/jrisc.py, on a DRAM image laid
out as on the Jaguar, and plays the I2S itself: at dsp_emitted it takes the sample the loop
has just produced and empties the ring, so the loop never waits.

The stream is Monkey Island's: scumm_pc --opl --soundcard 3 --bootparam ROOM (default 28, the
bar), its writes grouped into the driver's ticks as csrc/jag/jag_audio.c groups them (each
tick's writes, then $0001).  tools/opl2/opl2_dump.c plays it through the model, being the
68000 too (the producer of jag_audio.c: AD_LEAD ticks ahead, once a frame, the ring starting
near its end so that it wraps at once, a run of frames that produce nothing so that the DSP
stalls, MUS_HOLD on and off), and writes down what it wrote into the ring and when; here the
same writes go into the DSP's DRAM at the same samples.  Every sample must be equal, and at
the end MUS_PLAYED, MUS_STALL and MUS_RD the model's.

Also reports what a sample costs, counted as JaguarDocs hw/06 section 12 measures the GPU
(1 clk an ALU instruction, 3 a movei, 4 more for a taken jump, 2 a load or store in local RAM,
4.3 an indexed one, ~40 an external load): an estimate, not a measurement - the hardware's is
the SND_SPIN counter in the COM8 log.  Exit 1 on the first difference.
"""
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import jrisc  # noqa: E402

PCEXE = os.path.join(REPO, 'build', 'pc', 'scumm_pc.exe')

MINGW = os.environ.get('MINGW', r'C:\msys64\mingw64\bin')   # build.bat's, else this machine's


def map_inc():
    eq = {}
    for line in open(os.path.join(REPO, 'include', 'map.inc')):
        m = re.match(r'(\w+)\s+EQU\s+\$([0-9A-Fa-f]+)', line)
        if m:
            eq[m.group(1)] = int(m.group(2), 16)
    return eq


def lst_symbols(names):
    syms = {}
    for line in open(os.path.join(REPO, 'build', 'main.lst'), errors='replace'):
        for m in re.finditer(r'(\S+) ([0-9A-F]{16})  [a-z]', line):
            if m.group(1) in names:
                syms[m.group(1)] = int(m.group(2), 16)
    missing = [n for n in names if n not in syms]
    if missing:
        raise SystemExit(f'dsp_check: {missing} not in build/main.lst')
    return syms


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def ticks_bin(room, rounds):
    """scumm_pc's OPL writes as the driver's ticks: a write printed with tick t is either the
    main loop's after tick t or the driver's tick t + 1, which jag_audio.c puts together."""
    if not os.path.isfile(PCEXE):
        raise SystemExit(f'dsp_check: {PCEXE} missing: sh tools/build_pc.sh')
    res = subprocess.run([PCEXE, os.path.join(REPO, 'MI_1'), str(rounds), '--bootparam', str(room),
                          '--soundcard', '3', '--debugmode', '1', '--opl'],
                         capture_output=True, text=True, encoding='latin-1')
    ticks = {}
    last = 0
    for ln in res.stdout.splitlines():
        if ln.startswith('OPL '):
            _, t, r, v = ln.split()
            t = int(t)
            ticks.setdefault(t, []).append(int(r, 16) << 8 | int(v, 16))
            last = max(last, t)
    out = bytearray(struct.pack('>I', last + 1))
    for t in range(last + 1):
        w = ticks.get(t, []) + [1]
        out += struct.pack('>H', len(w)) + b''.join(struct.pack('>H', x) for x in w)
    return bytes(out), last + 1, sum(len(v) for v in ticks.values())


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 0.5
    room = int(sys.argv[2]) if len(sys.argv) > 2 else 28
    eq = map_inc()
    sy = lst_symbols(['DspAudioRom', 'DspAudioRomEnd', 'dsp_emitted', 'snd_rd',
                      'dsp_ram_end', 'opl_dinc', 'dsp_spin', 'dsp_idle', 'prof_eg',
                      'prof_ch', 'prof_rhy', 'prof_out', 'seq_tick'])
    regions = sorted([(sy['dsp_idle'], 'tick+lfo'), (sy['prof_eg'], 'envelopes'),
                      (sy['prof_ch'], 'channels'), (sy['prof_rhy'], 'rhythm'),
                      (sy['prof_out'], 'out'), (sy['seq_tick'], 'slow paths')])
    prof = {}
    if sy['dsp_ram_end'] > jrisc.D_RAM + jrisc.D_RAM_SIZE:
        raise SystemExit(f"dsp_check: the DSP blob ends at ${sy['dsp_ram_end']:06X}, past Jerry's 8 KB")
    main_bin = open(os.path.join(REPO, 'build', 'main.bin'), 'rb').read()
    code = main_bin[sy['DspAudioRom']:sy['DspAudioRomEnd']]

    fs = None
    for line in open(os.path.join(REPO, 'build', 'opl2_tab.h')):
        if line.startswith('#define OPL_FS '):
            fs = float(line.split()[2])
    n = int(secs * fs)
    tb, nticks, nwrites = ticks_bin(room, int(secs * 60 / 4) + 20)   # ~4 jiffies a round
    tick_file = os.path.join(REPO, 'build', f'dsp_check_ticks_{os.getpid()}.bin')
    open(tick_file, 'wb').write(tb)
    ring_bytes = eq['AD_RING_WORDS'] * 2
    start = ring_bytes - 400                    # the first wrap within a few ticks
    frames = n // 187
    late_from, late_to = frames // 3, frames // 3 + 4          # four frames with nothing
    at_hold, at_free = (6 * n // 10, 7 * n // 10)

    # The model, and the producer's writes.
    exe = os.path.join(REPO, 'build', f'opl2_dump_{os.getpid()}.exe')
    out = os.path.join(REPO, 'build', f'opl2_dump_{os.getpid()}.bin')
    sched = os.path.join(REPO, 'build', f'opl2_sched_{os.getpid()}.bin')
    env = dict(os.environ, PATH=MINGW + os.pathsep + os.environ['PATH'])
    subprocess.run([os.path.join(MINGW, 'gcc.exe'), '-std=c11', '-O2', '-Wall', '-Wextra',
                    '-I' + os.path.join(REPO, 'build'), '-o', exe,
                    os.path.join(HERE, 'opl2_dump.c'), os.path.join(HERE, 'opl2_model.c')],
                   check=True, env=env)
    subprocess.run([exe, tick_file, str(n), str(start), str(late_from), str(late_to),
                    str(at_hold), str(at_free), out, sched], check=True, env=env)
    want = struct.unpack('<%dh' % n, open(out, 'rb').read())
    sb = open(sched, 'rb').read()
    writes, o = {}, 0
    while True:
        smp = struct.unpack_from('<I', sb, o)[0]
        o += 4
        if smp == 0xFFFFFFFF:
            m_played, m_stall, m_rd = struct.unpack_from('<III', sb, o)
            break
        cnt = struct.unpack_from('<I', sb, o)[0]
        o += 4
        pairs = []
        for _ in range(cnt):
            off, w = struct.unpack_from('<IH', sb, o)
            o += 6
            pairs.append((off, w))
        wr = struct.unpack_from('<I', sb, o)[0]
        o += 4
        writes[smp] = (pairs, wr)
    wraps = sum(1 for pairs, _wr in writes.values() for _o, w in pairs if w == 0xFF00)

    # The Jaguar: DRAM with the slow tables and the cells, as InitDspAudio leaves them.
    dram = bytearray(0x200000)
    tabs = main_bin[sy['opl_dinc']:sy['opl_dinc'] + 640]
    dram[eq['OPL_TABS']:eq['OPL_TABS'] + 640] = tabs
    ring = eq['AD_RING']

    def cell(name, v):
        dram[eq[name]:eq[name] + 4] = struct.pack('>I', v & 0xFFFFFFFF)

    cell('MUS_ADR', ring + start)
    cell('MUS_LOOP', ring)
    cell('MUS_WR', ring + start)
    cell('MUS_RD', ring + start)
    cell('MUS_SEQ', 1)
    dsp = jrisc.Dsp(dram, code)
    emitted, rd_at = sy['dsp_emitted'], sy['snd_rd'] - jrisc.D_RAM

    t0 = time.time()
    costs = []
    for i in range(n):
        if i in writes:
            pairs, wr = writes[i]
            for off, w in pairs:
                dram[ring + off:ring + off + 2] = struct.pack('>H', w)
            cell('MUS_WR', ring + wr)
        if i == at_hold:
            cell('MUS_HOLD', 1)
        if i == at_free:
            cell('MUS_HOLD', 0)
        clk = prev_clk = 0.0
        limit = dsp.steps + 200000
        while True:
            pc = dsp.pc
            w = dsp.word(pc)
            op = w >> 10
            if op == 38:
                clk += 3
            elif op in (43, 44, 49, 50, 58, 59, 60, 61):
                clk += 4.3
            elif op in (41, 47):
                a = dsp.r[(w >> 5) & 31]
                clk += 40 if (op == 41 and jrisc.Dsp.external(a)) else 2
            elif op in (52, 53):
                clk += 1 + (4 if dsp.cond(w & 31) else 0)
            else:
                clk += 1
            reg = 'init'
            for a, name in regions:
                if pc >= a:
                    reg = name
            prof[reg] = prof.get(reg, 0) + (clk - prev_clk)
            prev_clk = clk
            try:
                dsp.step()
            except jrisc.JriscError as e:
                raise SystemExit(f'dsp_check: sample {i}: {e}')
            if dsp.pc == emitted:
                break
            if dsp.pc == sy['dsp_spin']:
                raise SystemExit(f'dsp_check: sample {i}: the loop thinks the ring is full')
            if dsp.steps > limit:
                raise SystemExit(f'dsp_check: sample {i}: no sample after 200,000 instructions '
                                 f'(pc ${dsp.pc:06X})')
        got = dsp.r[24] - (1 << 32) if dsp.r[24] >> 31 else dsp.r[24]
        if got != want[i]:
            raise SystemExit(f'dsp_check: sample {i} (t = {i / fs:.4f} s): DSP {got}, model {want[i]}')
        costs.append(clk)
        # the I2S, instantly: the ring is empty again
        dsp.local[rd_at:rd_at + 4] = struct.pack('>I', dsp.r[23])
    dt = time.time() - t0
    srt = sorted(costs)
    budget = 26590906 / fs - 54
    d_played, d_stall, d_rd = (be32(dram, eq['MUS_PLAYED']), be32(dram, eq['MUS_STALL']),
                               be32(dram, eq['MUS_RD']) - ring)
    if (d_played, d_stall, d_rd) != (m_played, m_stall, m_rd):
        raise SystemExit(f'dsp_check: played/stall/rd: DSP {d_played} {d_stall} {d_rd}, '
                         f'model {m_played} {m_stall} {m_rd}')
    print(f'dsp_check: {n} samples equal to the model ({secs:.2f} s of room {room}: {nwrites} '
          f'writes in {nticks} ticks; the ring wrapped {wraps} times, {d_stall} stalls, held and resumed; '
          f'{d_played} ticks played, as the model; {dt:.0f} s)')
    print(f'  per sample ~{sum(costs) / n:.0f} clk on average, 99% under '
          f'{srt[int(0.99 * n)]:.0f}, worst {srt[-1]:.0f} (budget {budget:.0f})')
    print('  per sample: ' + ', '.join(f'{k} {v / n:.0f}' for k, v in sorted(prof.items(), key=lambda x: -x[1])))
    # What starves the I2S is not one dear sample (the 64-sample ring takes
    # it) but a run of them: the worst average over 64 consecutive samples.
    # From 0.05 s: past the instruments' set-up burst at the song's start.
    m0 = min(int(0.05 * fs), n - 64)
    acc = sum(costs[m0:m0 + 64])
    worst64 = acc
    for i in range(m0 + 64, n):
        acc += costs[i] - costs[i - 64]
        worst64 = max(worst64, acc)
    print(f'  music only: average {sum(costs[m0:]) / max(1, n - m0):.0f} clk, '
          f'worst 64-sample average {worst64 / 64:.0f} clk: fits SCLK >= '
          f'{max(0, int((worst64 / 64 + 54) / 64 + 0.999) - 1)}')
    # The ring itself, which is what decides: the loop produces at the
    # estimated costs (stretched by the ISR's ~54 clk a sample), waits while
    # 64 samples are ready, and the I2S takes one every P clk from the moment
    # the ring is first full; an interrupt that finds it empty is a starve
    # (the ISR repeats the last sample and takes nothing).
    period = 26590906 / fs
    stretch = period / budget
    # DSP_SLOW=1.17: the hardware's cost over this estimate (session 9, the bar: ~18 % idle
    # measured from SND_SPIN against ~30 % estimated)
    stretch *= float(os.environ.get('DSP_SLOW', '1'))
    t, rd, irq, starve, low = 0.0, 0, None, 0, 64
    for k, c in enumerate(costs):
        while k - rd >= 64:                     # full: spin to the next interrupt
            if irq is None:
                irq = t + period
            t = max(t, irq)
            rd += 1
            irq += period
        t += c * stretch
        while irq is not None and irq <= t:
            if rd < k:
                rd += 1
            else:
                starve += 1
            irq += period
        if irq is not None:
            low = min(low, k + 1 - rd)
    print(f'  the ring at SCLK {round(26590906 / fs / 64) - 1}, costs x {os.environ.get("DSP_SLOW", "1")}: {starve} starved interrupts, '
          f'fewest samples ready {low}')
    for p in (tick_file, exe, out, sched):
        try:
            os.remove(p)
        except OSError:
            pass
    if starve:
        raise SystemExit('dsp_check: the I2S would starve')


if __name__ == '__main__':
    main()
