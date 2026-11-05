/* SPDX-License-Identifier: GPL-2.0+ */
.syntax unified
.cpu arm1136jf-s
.arm
.global _start
_start:
    b entry
    .space 0x24 - (. - _start), 0
    .word 0x016f2818
    .word 0
    .word 0 /* Filled by pack-boot.py. */
entry:
    msr cpsr_c, #0xd3
    mrc p15, 0, r0, c1, c0, 0
    bic r0, r0, #1
    bic r0, r0, #4
    bic r0, r0, #0x1000
    mcr p15, 0, r0, c1, c0, 0
    mov r0, #0
    mcr p15, 0, r0, c7, c10, 4
    mcr p15, 0, r0, c7, c5, 0
    mcr p15, 0, r0, c8, c7, 0
    ldr pc, =0x81609000
