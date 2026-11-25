| SPDX-License-Identifier: MIT
| Copyright (c) 2026 Samuele Voltan.  See LICENSE.
| JaguarMI - the C blob's entry table.
|
| RMAC calls the blob through FIXED offsets (C_ENTRY_INIT, C_ENTRY_MAIN in
| include/map.inc), six bytes each: it does not read the linker's symbols.
| New entries go at the END, never in the middle.  (JagGOT's.)
|
| ColdBoot copies the whole image (code, rodata, data) to DBLOB; .bss lives
| in CBSS, which the boot's DRAM clear has already zeroed.

	.section .entry,"ax"
	.global entry_table
entry_table:
	jmp	c_init		| C_ENTRY_INIT: returns C_MAGIC in d0
	jmp	c_main		| C_ENTRY_MAIN: runs the interpreter, never returns
	jmp	c_audio		| C_ENTRY_AUDIO: the AdLib driver, from the video interrupt
