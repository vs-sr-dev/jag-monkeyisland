#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
"""The ROM in BigPEmu, its log saved as if it had come from COM8.

    python tools/emu_log.py [ROM] [--timeout S]

Without a GameDrive the C writes its log lines into the EMU_LOG region of DRAM
(csrc/jag/jag_main.c log_line) and sets EMU_END when the run is over (back_to_stub).  This
writes a BigPEmu script that waits for EMU_END, saves the lines and closes the emulator; the
lines land in build/logs/emu_HHMMSS.log, which the COM8 tools read as they are
(tools/pal_crc.py LOG --bb -- ..., tools/trace_crc.py LOG REF).

BigPEmu is the oracle of the logic, not of time, the bus or the interrupts (JaguarDocs
emu/bigpemu.md): the trace, the palette and the back buffer are checked here; the time
phase's numbers mean nothing.  Structure from Prowl's translate/pack.py --emu.

Another BigPEmuDev already running is left alone: the tool refuses to start.
"""
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import localcfg  # noqa: E402
BIGPEMU = localcfg.get('BIGPEMU', '')   # BigPEmu's folder (local.ini)
NAME = 'jmi_log'

SCRIPT = r'''//__BIGPEMU_SCRIPT_MODULE__
//__BIGPEMU_META_DESC__		"JaguarMI: save the EMU_LOG lines once EMU_END is set, then close BigPEmu"
//__BIGPEMU_META_AUTHOR__	"JaguarMI tools/emu_log.py"
// (the first line is required: without it BigPEmuDev does not compile the script)
#include "bigpcrt.h"

static uint8_t sBuf[0x%(size)X];
static uint32_t sDone = 0;

static uint32_t rd32(const uint32_t a)
{
	uint8_t b[4];
	bigpemu_jag_sysmemread(b, a, 4);
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static uint32_t on_emu_frame(const int32_t eventHandle, void *pEventData)
{
	const uint64_t f = bigpemu_jag_get_frame_count();
	if (sDone || f < 120)
		return 0;
	if (rd32(0x%(end)X) != 0x%(magic)X && f < %(maxframes)d)
		return 0;
	uint32_t n = rd32(0x%(logn)X);
	if (n > 0x%(size)X)
		n = 0x%(size)X;
	bigpemu_jag_sysmemread(sBuf, 0x%(log)X, n);
	const uint64_t fh = fs_open("%(name)s.txt", 1);
	if (fh)
	{
		fs_write(sBuf, n, fh);
		fs_close(fh);
	}
	const uint64_t dh = fs_open("%(name)s_done.txt", 1);
	if (dh)
	{
		char line[32];
		const int k = sprintf(line, "%%d", (int)f);
		fs_write(line, k, dh);
		fs_close(dh);
	}
	sDone = 1;
	EBigPEmuWindowPlatform plat;
	const uint64_t w = bigpemu_native_window_handle(&plat);
	if (w)
		bigpemu_send_native_window_message(w, 0x0010, 0, 0);   // WM_CLOSE
	return 0;
}

void bigp_init()
{
	void *pMod = bigpemu_get_module_handle();
	bigpemu_register_event_emu_thread_frame(pMod, on_emu_frame);
}

void bigp_shutdown()
{
}
'''


def map_inc():
    sym = {}
    for ln in open(os.path.join(REPO, 'include', 'map.inc')):
        m = re.match(r'(\w+)\s+EQU\s+\$([0-9A-F]+)', ln)
        if m:
            sym[m.group(1)] = int(m.group(2), 16)
    return sym


def running():
    r = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq BigPEmuDev.exe'], capture_output=True, text=True)
    return 'BigPEmuDev.exe' in r.stdout


def main():
    args = sys.argv[1:]
    timeout = 900
    if '--timeout' in args:
        i = args.index('--timeout')
        timeout = int(args[i + 1])
        del args[i:i + 2]
    rom = os.path.abspath(args[0] if args else os.path.join(REPO, 'build', 'jaguarmi.j64'))
    if running():
        print('emu_log: another BigPEmuDev is running; close it first')
        return 1
    s = map_inc()
    exe = os.path.join(BIGPEMU, 'BigPEmuDev.exe')
    scripts = os.path.join(BIGPEMU, 'Scripts')
    src = os.path.join(scripts, NAME + '.c')
    vm = os.path.join(scripts, NAME + '.bigpcvm')
    with open(src, 'w', newline='\n') as f:
        f.write(SCRIPT % dict(size=s['EMU_LOG_SIZE'], end=s['EMU_END'], magic=s['EMU_END_MAGIC'],
                              logn=s['EMU_LOG_N'], log=s['EMU_LOG'], name=NAME,
                              maxframes=timeout * 60))
    # BigPEmuDev compiles every script at start-up
    if os.path.exists(vm):
        os.remove(vm)
    p = subprocess.Popen([exe])
    t0 = time.time()
    while not os.path.exists(vm):
        if time.time() - t0 > 30:
            p.kill()
            print('emu_log: BigPEmuDev did not compile', src)
            return 1
        time.sleep(0.3)
    time.sleep(0.5)
    p.kill()
    p.wait()
    time.sleep(0.6)
    shutil.copy(vm, os.path.splitext(rom)[0] + '.bigpcvm')
    out_txt = os.path.join(scripts, NAME + '.txt')
    done = os.path.join(scripts, NAME + '_done.txt')
    for q in (out_txt, done):
        if os.path.exists(q):
            os.remove(q)
    p = subprocess.Popen([exe, rom])
    t0 = time.time()
    while not os.path.exists(done):
        if time.time() - t0 > timeout + 60 or p.poll() is not None:
            p.kill()
            print('emu_log: no log within the time (or BigPEmu closed)')
            return 1
        time.sleep(0.5)
    try:
        p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill()
    os.makedirs(os.path.join(REPO, 'build', 'logs'), exist_ok=True)
    log = os.path.join(REPO, 'build', 'logs', time.strftime('emu_%H%M%S.log'))
    shutil.move(out_txt, log)
    frame = open(done).read().strip()
    lines = open(log, encoding='latin-1').read().splitlines()
    print(f'emu_log: {len(lines)} lines, frame {frame}, {time.time() - t0:.0f} s -> {log}')
    for ln in lines[-3:]:
        print('  ' + ln)
    return 0


if __name__ == '__main__':
    sys.exit(main())
