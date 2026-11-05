/* SPDX-License-Identifier: GPL-2.0+ */
/* Embed the existing upstream U-Boot logo without modifying the asset. */
.section .rodata.totoro_logo,"a"
.balign 4
.global totoro_logo_bmp
totoro_logo_bmp:
.incbin "tools/logos/u-boot_logo.bmp"
.section .note.GNU-stack,"",%progbits
