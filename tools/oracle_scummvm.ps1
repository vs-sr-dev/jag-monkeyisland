# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
# The oracle: ScummVM's own opcode trace of the game, for tools/trace_cmp.py.
#
#   powershell -File tools\oracle_scummvm.ps1 [-Secs 90] [-Tag boot] [-GameDir MI_1] [-Seed 1234] [-Shots "12,24"] [-Music adlib] [-BootParam 1] [-Clicks "8:240,120;20:60,110"] [-Keys "12:13;16:49"] [-CopyProtection] [-Offscreen] [-Pixels]
#
# Writes build\oracle\<Tag>.out (ScummVM's stdout: the trace) and optional window shots.
# ScummVM runs with its OWN config file, build\oracle\<Tag>.ini (the user's configuration is never
# touched): enhancements off, copy protection skipped (as the game's later releases did),
# music_driver=null, --random-seed, and debug level 11.  Two facts found the hard way:
#   - the channel switch (--debugflags) does not take effect from the command line; level 11
#     turns every channel on, and trace_cmp.py filters the lines;
#   - ScummVM must be CLOSED, not killed, or its buffered stdout is lost.
# -Music adlib plays the music (VAR_SOUNDCARD 3, as in the port) through ScummVM's iMUSE, not
# the original driver the port uses (csrc/scumm/player_ad.c), and on real time: compare the
# ORDER of events then, not the lines.
# -CopyProtection: the Dial-A-Pirate wheel runs (copy_protection=true; scumm_pc --copyprot 1):
# with -BootParam 0 the game starts with it, then the intro.
# -BootParam 1 (any non-zero value): MI1's boot script skips the intro and starts the game in
# room 38, the lookout; with music_driver=null that path is deterministic, line for line.
# A debug level makes ScummVM set VAR_DEBUGMODE = 1 (_debugMode = gDebugLevel > 0), which the
# boot script reads: compare with scumm_pc --debugmode 1 --soundcard 0.
param(
    [int]$Secs = 90,
    [string]$Tag = "boot",
    [string]$GameDir = "MI_1",
    [int]$Seed = 1234,
    [string]$Shots = "",
    [string]$Music = "null",
    [int]$BootParam = 0,
    [string]$ScummVM = "",          # default: SCUMMVM_EXE in local.ini, else scummvm.exe
    [string]$Clicks = "",
    [string]$Keys = "",
    [switch]$CopyProtection,
    [string]$Burst = "",
    [switch]$Offscreen,
    [switch]$Pixels
)
$ErrorActionPreference = "Stop"
function Get-Local($key, $default) {
    $v = [Environment]::GetEnvironmentVariable($key)
    if ($v) { return $v }
    $ini = Join-Path (Split-Path -Parent (Split-Path -Parent $PSCommandPath)) "local.ini"
    if (Test-Path $ini) {
        foreach ($l in Get-Content $ini) {
            if ($l -match "^\s*$key\s*=\s*(.*)$") { return $Matches[1].Trim() }
        }
    }
    return $default
}
if (-not $ScummVM) { $ScummVM = Get-Local "SCUMMVM_EXE" "scummvm.exe" }
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$dir = Join-Path $repo "build\oracle"
New-Item -ItemType Directory -Force $dir | Out-Null
$game = (Resolve-Path (Join-Path $repo $GameDir)).Path
# -Pixels: the window shows exactly the game's 320x200 pixels, so a shot is the screen ScummVM
# drew, pixel for pixel, for tools/shot_cmp.py: OpenGL (the CLUT8 screen goes out as RGBA8888,
# the palette's bytes exact), the 1x "normal" scaler, stretch mode "center" (one game pixel, one
# window pixel, centred), no aspect correction, no filtering, a window asked at 320x200.
# Not the SDL surface: its hardware screen is 16-bit (SDL_SetVideoMode(..., 16, ...)), and RGB565
# loses the palette's low bits.  Without -Pixels: OpenGL with aspect correction, shown at 4:3.
if ($CopyProtection) { $cp = "true" } else { $cp = "false" }
if ($Pixels) {
    $gfx = "gfx_mode=opengl`r`nscaler=normal`r`nscale_factor=1`r`nstretch_mode=center`r`naspect_ratio=false`r`nfiltering=false`r`nlast_window_width=320`r`nlast_window_height=200"
} else {
    $gfx = "gfx_mode=opengl"
}
@"
[scummvm]
$gfx
fullscreen=false
enable_enhancements=false
enhancements=0
copy_protection=$cp
music_driver=$Music
subtitles=true
updates_check=0

[oracle]
engineid=scumm
gameid=monkey
extra=VGA
description=oracle
path=$game
enhancements=0
copy_protection=$cp
"@ | Set-Content -Encoding ascii (Join-Path $dir "$Tag.ini")

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class OraclePW {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref P p);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
    public struct P { public int X, Y; }
    public struct R { public int L, T, Rt, B; }
}
"@
$out = Join-Path $dir "$Tag.out"
$args = @("--config=`"$dir\$Tag.ini`"", "--logfile=`"$dir\$Tag.log`"", "--random-seed=$Seed",
          "--debuglevel=11", "--boot-param=$BootParam", "--no-console", "oracle")
# The game reads the mouse (VAR_VIRT_MOUSE_X/Y, findObject under it), and SDL hands ScummVM
# the Windows cursor whenever it lies over the window.  So the cursor is pinned, as soon as the
# window exists and again every wait step, on game pixel (104, 56): ScummVM's start position,
# which scumm_pc keeps.  The picture is 320x200 shown at 4:3 (aspect correction; 320x200 as it
# is with -Pixels), centred in the client area.  Do not touch the mouse while the oracle runs.
$script:pinX = 104; $script:pinY = 56
if ($Pixels) { $script:aspH = 200 } else { $script:aspH = 240 }
function Pin-Cursor($proc) {
    $proc.Refresh(); $h = $proc.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return }
    $r = New-Object OraclePW+R; [void][OraclePW]::GetClientRect($h, [ref]$r)
    if ($r.Rt -le 0 -or $r.B -le 0) { return }
    $scale = [Math]::Min($r.Rt / 320.0, $r.B / $script:aspH)
    if ($Pixels) { $scale = 1.0 }
    $ox = ($r.Rt - 320 * $scale) / 2; $oy = ($r.B - $script:aspH * $scale) / 2
    $pt = New-Object OraclePW+P
    # Floor, not [int]: PowerShell's [int] rounds half to even, so the middle of an odd pixel
    # (y + 0.5 at scale 1) landed on the next one.
    $pt.X = [int][Math]::Floor($ox + ($script:pinX + 0.5) * $scale)
    $pt.Y = [int][Math]::Floor($oy + ($script:pinY + 0.5) * ($script:aspH / 200.0) * $scale)
    [void][OraclePW]::ClientToScreen($h, [ref]$pt)
    [void][OraclePW]::SetCursorPos($pt.X, $pt.Y)
}
# The cursor starts parked off the window (right of it, half-way down; not in a corner, which
# Windows 11 uses for "show desktop"), and stays there until the first click: SDL sends no
# motion while it is off the window, so ScummVM keeps its start position (104, 56), as
# scumm_pc does.  Pinning from the start loses a race: the first frames run before the window
# can be found.
$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
if ($Offscreen -and ($Clicks -or $Keys -or $Burst -or $Shots)) { throw "-Offscreen is for runs without input or shots" }
# Also with -Offscreen: the window first appears centred, and if it appears under the cursor,
# moving it away makes SDL clamp the mouse to the window's edge (room 85 read x = 319).
[void][OraclePW]::SetCursorPos($vs.Right - 100, [int](($vs.Top + $vs.Bottom) / 2))
$script:pinning = $false
# -Offscreen (runs without input only): the window is moved under the taskbar as soon as it
# exists, and again whenever ScummVM recreates or moves it, so the real cursor can never be
# over it: after the jump at the start the mouse is free for whoever sits at the PC, and
# several runs can go at once.
# Not wholly off the desktop: a window no monitor shows blocks ScummVM before the game starts
# (the swap waits).  Clicks cannot be posted instead: SDL2 answers a posted WM_MOUSEMOVE with
# TrackMouseEvent, Windows replies WM_MOUSELEAVE at once (the real cursor is elsewhere), and
# SDL then moves the mouse to the real cursor clamped to the window's edge, in the same frame.
$prim = [System.Windows.Forms.Screen]::PrimaryScreen
$script:hideX = $prim.Bounds.Left + 200; $script:hideY = $prim.WorkingArea.Bottom + 4
function Hide-Window($proc) {
    $proc.Refresh(); $h = $proc.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { return }
    $r = New-Object OraclePW+R; [void][OraclePW]::GetWindowRect($h, [ref]$r)
    if ($r.L -ne $script:hideX -or $r.T -ne $script:hideY) {
        # SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
        [void][OraclePW]::SetWindowPos($h, [IntPtr]::Zero, $script:hideX, $script:hideY, 0, 0, 0x15)
    }
}
$p = Start-Process -FilePath $ScummVM -ArgumentList $args -PassThru `
        -RedirectStandardOutput $out -RedirectStandardError (Join-Path $dir "$Tag.err")
$start = Get-Date
# -Clicks "T:x,y;...": at T seconds the pinned cursor moves to game pixel (x, y); 0.4 s later the
# left button goes down, 0.1 s after that up.  Real input lands on whatever frame it lands on:
# scumm_pc --input replays it at frames found by matching the traces (docs/todo.md).
$script:clickList = @()
if ($Clicks) {
    foreach ($c in $Clicks.Split(";")) {
        $t, $xy = $c.Split(":"); $x, $y = $xy.Split(",")
        $script:clickList += ,@([double]$t, [int]$x, [int]$y)
    }
}
$script:ci = 0; $script:phase = 0
# -Keys "T:A;...": at T seconds the key whose ASCII is A (decimal) goes down, 0.1 s later up,
# as a real keyboard (keybd_event: ScummVM's window must be the foreground one, as for the
# clicks; it is brought there first).  Digits, letters, Return 13, Backspace 8, ESC 27,
# '.' 46, space 32; 1-26 are Ctrl + the letter (23: Ctrl-W), 1001-1026 Ctrl + Alt + the letter
# (1004: ScummVM's debugger, whose commands are typed as keys too).  scumm_pc replays it as "FRAME
# key A" (input_fit.py finds the frames).
$script:keyList = @()
if ($Keys) {
    foreach ($k in $Keys.Split(";")) {
        $t, $a = $k.Split(":")
        $script:keyList += ,@([double]$t, [int]$a)
    }
}
$script:ki = 0; $script:kphase = 0
function Key-Vk($a) {
    if ($a -eq 46) { return 0xBE }               # VK_OEM_PERIOD
    if ($a -ge 97 -and $a -le 122) { return $a - 32 }
    if ($a -ge 1 -and $a -le 26) { return $a + 64 }  # with Ctrl held
    if ($a -ge 1001 -and $a -le 1026) { return $a - 1000 + 64 }  # with Ctrl and Alt held
    return $a                                    # digits, upper case, 8, 9, 13, 27, 32
}
function Step-Once {
    $now = ((Get-Date) - $start).TotalSeconds
    if ($script:ki -lt $script:keyList.Count) {
        $k = $script:keyList[$script:ki]
        $vk = [byte](Key-Vk $k[1]); $sc = [byte][OraclePW]::MapVirtualKey($vk, 0)
        if ($script:kphase -eq 0 -and $now -ge $k[0]) {
            $p.Refresh(); [void][OraclePW]::SetForegroundWindow($p.MainWindowHandle)
            if (($k[1] -ge 1 -and $k[1] -le 26) -or ($k[1] -ge 1001 -and $k[1] -le 1026)) { [OraclePW]::keybd_event(0x11, 0x1D, 0, [IntPtr]::Zero) }
            if ($k[1] -ge 1001 -and $k[1] -le 1026) { [OraclePW]::keybd_event(0x12, 0x38, 0, [IntPtr]::Zero) }
            [OraclePW]::keybd_event($vk, $sc, 0, [IntPtr]::Zero); $script:kphase = 1
        } elseif ($script:kphase -eq 1 -and $now -ge $k[0] + 0.1) {
            [OraclePW]::keybd_event($vk, $sc, 2, [IntPtr]::Zero); $script:kphase = 0; $script:ki++
            if ($k[1] -ge 1001 -and $k[1] -le 1026) { [OraclePW]::keybd_event(0x12, 0x38, 2, [IntPtr]::Zero) }
            if (($k[1] -ge 1 -and $k[1] -le 26) -or ($k[1] -ge 1001 -and $k[1] -le 1026)) { [OraclePW]::keybd_event(0x11, 0x1D, 2, [IntPtr]::Zero) }
            Write-Host ("key {0} at {1:N1} s" -f $k[1], $now)
        }
    }
    if ($script:ci -lt $script:clickList.Count) {
        $c = $script:clickList[$script:ci]
        if ($script:phase -eq 0 -and $now -ge $c[0]) {
            $script:pinX = $c[1]; $script:pinY = $c[2]; $script:phase = 1; $script:pinning = $true
        } elseif ($script:phase -eq 1 -and $now -ge $c[0] + 0.4) {
            Pin-Cursor $p; [OraclePW]::mouse_event(2, 0, 0, 0, [IntPtr]::Zero); $script:phase = 2
        } elseif ($script:phase -eq 2 -and $now -ge $c[0] + 0.5) {
            [OraclePW]::mouse_event(4, 0, 0, 0, [IntPtr]::Zero); $script:phase = 0; $script:ci++
            Write-Host ("click at ({0},{1}), {2:N1} s" -f $c[1], $c[2], $now)
        }
    }
    if ($script:pinning) { Pin-Cursor $p }
    if ($Offscreen) { Hide-Window $p }
}
function Step-Until($t) {
    while (((Get-Date) - $start).TotalSeconds -lt $t) { Step-Once; Start-Sleep -Milliseconds 20 }
}
# The game's 320x200 in the client area with -Pixels: stretch mode "center" places it at
# ((W - 320) / 2, (H - 200) / 2), integer division (windowed.h, populateDisplayAreaDrawRect).
function Game-Rect($r) {
    if ($r.Rt -lt 320 -or $r.B -lt 200) { throw "client area $($r.Rt)x$($r.B) is smaller than 320x200" }
    New-Object System.Drawing.Rectangle([int][Math]::Floor(($r.Rt - 320) / 2), [int][Math]::Floor(($r.B - 200) / 2), 320, 200)
}
$list = @(); if ($Shots) { $list = $Shots.Split(",") | ForEach-Object { [double]$_ } }
foreach ($s in $list) {
    Step-Until $s
    $p.Refresh(); $h = $p.MainWindowHandle
    if ($h -eq [IntPtr]::Zero) { Write-Host "no window at $s s"; continue }
    $r = New-Object OraclePW+R; [void][OraclePW]::GetClientRect($h, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap($r.Rt, $r.B); $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc(); [void][OraclePW]::PrintWindow($h, $hdc, 3); $g.ReleaseHdc($hdc)
    $keep = $bmp; if ($Pixels) { $keep = $bmp.Clone((Game-Rect $r), $bmp.PixelFormat) }
    $keep.Save((Join-Path $dir ("{0}_{1}s.png" -f $Tag, $s)), [System.Drawing.Imaging.ImageFormat]::Png)
    if ($Pixels) { $keep.Dispose() }
    $g.Dispose(); $bmp.Dispose()
}
# -Burst "a,b": from a to b seconds, the window as fast as it can be captured, reduced to the
# game's 320x240 (nearest neighbour), named by milliseconds: positions frame by frame, where
# the trace has none.  With -Pixels each capture is the client area as it is (320x200, the
# game's own pixels), and a capture equal to the one before is not saved: what is left is the
# sequence of distinct screens ScummVM showed, for tools/shot_cmp.py.
if ($Burst) {
    $a, $b = $Burst.Split(",") | ForEach-Object { [double]$_ }
    Step-Until $a
    $p.Refresh(); $h = $p.MainWindowHandle
    $r = New-Object OraclePW+R; [void][OraclePW]::GetClientRect($h, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap($r.Rt, $r.B); $g = [System.Drawing.Graphics]::FromImage($bmp)
    $small = New-Object System.Drawing.Bitmap(320, 240); $gs = [System.Drawing.Graphics]::FromImage($small)
    $gs.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $gs.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $scale = [Math]::Min($r.Rt / 320.0, $r.B / 240.0)
    $src = New-Object System.Drawing.RectangleF((($r.Rt - 320 * $scale) / 2), (($r.B - 240 * $scale) / 2), (320 * $scale), (240 * $scale))
    $dst = New-Object System.Drawing.RectangleF(0, 0, 320, 240)
    New-Item -ItemType Directory -Force (Join-Path $dir "$Tag.burst") | Out-Null
    Remove-Item (Join-Path $dir "$Tag.burst\*.png") -ErrorAction SilentlyContinue
    if ($Pixels) { $grect = Game-Rect $r }
    $prev = $null
    while (((Get-Date) - $start).TotalSeconds -lt $b) {
        Step-Once
        $ms = [int](((Get-Date) - $start).TotalMilliseconds)
        $hdc = $g.GetHdc(); [void][OraclePW]::PrintWindow($h, $hdc, 3); $g.ReleaseHdc($hdc)
        if ($Pixels) {
            $crop = $bmp.Clone($grect, $bmp.PixelFormat)
            $ms0 = New-Object System.IO.MemoryStream
            $crop.Save($ms0, [System.Drawing.Imaging.ImageFormat]::Bmp)
            $bytes = $ms0.ToArray(); $ms0.Dispose()
            if ($prev -ne $null -and [System.Linq.Enumerable]::SequenceEqual([byte[]]$prev, [byte[]]$bytes)) { $crop.Dispose(); continue }
            $prev = $bytes
            $crop.Save((Join-Path $dir ("$Tag.burst\{0:D6}.png" -f $ms)), [System.Drawing.Imaging.ImageFormat]::Png)
            $crop.Dispose()
        } else {
            $gs.DrawImage($bmp, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel)
            $small.Save((Join-Path $dir ("$Tag.burst\{0:D6}.png" -f $ms)), [System.Drawing.Imaging.ImageFormat]::Png)
        }
    }
    $g.Dispose(); $bmp.Dispose(); $gs.Dispose(); $small.Dispose()
}
Step-Until $Secs
[void]$p.CloseMainWindow()
if (-not $p.WaitForExit(8000)) { Write-Host "ScummVM did not close: killed (trace may be cut)"; Stop-Process -Id $p.Id -Force }
$n = (Select-String -Path $out -Pattern '^Script ' | Measure-Object).Count
Write-Host "oracle: $out, $n opcode lines"
