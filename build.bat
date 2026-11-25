@echo off
rem SPDX-License-Identifier: MIT
rem Copyright (c) 2026 Samuele Voltan.  See LICENSE.
rem ==========================================================================
rem JaguarMI - full build.  Unattended: exits with an errorlevel.
rem
rem   build.bat [GAME_DIR]     default: MI_1 (your own copy of the DOS VGA floppy release)
rem
rem Order: conversion first; then the memory map, which lays the cartridge
rem out over the blobs' real sizes; then the CHECKS; then whatever assembles
rem against them; the cartridge; last the oracle check and the expected frames.
rem ==========================================================================
setlocal

for /f "delims=" %%P in ('where python 2^>nul') do if not defined PY set "PY=%%P"
if not defined PY (echo PYTHON not found on PATH & exit /b 1)

rem This machine's paths: local.ini (local.ini.example lists them), unless the environment
rem already has them.
if exist local.ini for /f "usebackq eol=; tokens=1,* delims==" %%A in ("local.ini") do if not defined %%A set "%%A=%%B"
if not defined RMAC set "RMAC=rmac"
where "%RMAC%" >nul 2>nul || if not exist "%RMAC%" (echo RMAC not found: set it in local.ini & exit /b 1)
if not defined JAGINC (echo JAGINC not set: the Jaguar SDK's include folder, in local.ini & exit /b 1)

if not exist build mkdir build

echo [1/7] data conversion
"%PY%" tools\mi2jag.py %1
if errorlevel 1 (echo CONVERSION ERROR & exit /b 1)

echo [2/7] memory map, OPL2 tables
"%PY%" tools\memory_map.py
if errorlevel 1 (echo MEMORY MAP ERROR & exit /b 1)
"%PY%" tools\opl2.py
if errorlevel 1 (echo OPL2 TABLES ERROR & exit /b 1)

echo [3/7] ambiguous expressions (RMAC has no precedence)
"%PY%" tools\check_expr.py
if errorlevel 1 (echo EXPR ERROR & exit /b 1)

echo [4/7] RMAC
"%RMAC%" -fr -i%JAGINC%\ -iinclude\ -lbuild\main.lst -o build\main.bin src\main.s
if errorlevel 1 (echo ASSEMBLER ERROR & exit /b 1)

rem Jerry's OPL2: the real DSP binary in a JRISC interpreter against its C model
rem (tools/opl2/opl2_model.c), sample for sample, on half a second of the bar's music from
rem scumm_pc --opl (build/pc/scumm_pc.exe: sh tools/build_pc.sh), the ring wrapping, stalls
rem and a hold.  "python tools/opl2/dsp_check.py 5" is the long run.
echo [4b] DSP OPL2 vs its model
"%PY%" tools\opl2\dsp_check.py 0.5 28
if errorlevel 1 (echo DSP OPL2 DIFFERS FROM ITS MODEL & exit /b 1)

rem The interpreter, compiled for the 68000 (csrc/build_jag.sh).  The command goes to the
rem image as ONE string, or it exits 0 doing nothing.
echo [5/7] C blob (m68k-elf-gcc in Docker)
"%PY%" tools\gen_opcodes.py
if errorlevel 1 (echo OPCODE TABLE ERROR & exit /b 1)
rem The input events the ROM replays (JMI_INPUT=an oracle's .in file; none: idle).
"%PY%" tools\input_table.py
if errorlevel 1 (echo INPUT TABLE ERROR & exit /b 1)
docker run --rm -v "%CD%:/work" -w /work/csrc marsdev-32x:latest "sh build_jag.sh" > build\engine_build.log 2>&1
if errorlevel 1 (type build\engine_build.log & echo C BLOB ERROR & exit /b 1)
findstr /C:"engine.elf" build\engine_build.log
findstr /C:"ERROR" build\engine_build.log && (echo C BLOB ERROR & exit /b 1)

echo [6/7] cartridge
"%PY%" tools\cart_build.py
if errorlevel 1 (echo PACK ERROR & exit /b 1)

echo [7/7] oracle check and expected frames
"%PY%" tools\m0_ref.py %1
if errorlevel 1 (echo M0 REF ERROR & exit /b 1)

echo.
echo Build OK.  ROM: build\jaguarmi.j64
echo On screen: dark cyan while the interpreter runs; COM8 carries its trace and timing.
endlocal
