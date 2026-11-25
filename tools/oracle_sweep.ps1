# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Samuele Voltan.  See LICENSE.
# Idle oracle runs of many rooms at once: boot param N jumps to room N (MI1's debug start), and
# with -Offscreen nobody's mouse is involved, so the runs go in parallel.
#
#   powershell -File tools\oracle_sweep.ps1 -Rooms "1-99" [-Secs 25] [-Jobs 4]
#
# Each room writes build\oracle\room<N>.out; tools/sweep_cmp.py compares them with scumm_pc.
param([string]$Rooms = "1-99", [int]$Secs = 25, [int]$Jobs = 4)
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$list = @()
foreach ($part in $Rooms.Split(",")) {
    if ($part -match '^(\d+)-(\d+)$') { $list += [int]$Matches[1]..[int]$Matches[2] } else { $list += [int]$part }
}
$running = @()
foreach ($n in $list) {
    while (($running | Where-Object { -not $_.HasExited }).Count -ge $Jobs) { Start-Sleep -Milliseconds 300 }
    $running += Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @(
        "-ExecutionPolicy", "Bypass", "-File", "`"$here\oracle_scummvm.ps1`"",
        "-Secs", $Secs, "-Tag", "room$n", "-BootParam", $n, "-Offscreen")
    Write-Host "room $n started"
}
$running | ForEach-Object { $_.WaitForExit() }
Write-Host "sweep done"
