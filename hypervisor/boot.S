/*
 * nativecall armstub for Raspberry Pi 4 (BCM2711) running Emu68.
 *
 * Based on armstub8.S from https://github.com/raspberrypi/tools (armstubs/):
 *
 * Copyright (c) 2016-2019 Raspberry Pi (Trading) Ltd.
 * Copyright (c) 2016 Stephen Warren <swarren@wwwdotorg.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * * Neither the name of the copyright holder nor the names of its contributors
 *   may be used to endorse or promote products derived from this software
 *   without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Changes for nativecall:
 * - Secondary cores spin at EL2 as before, then go through hv_secondary (hypervisor + 4): core 2
 *   enters the kernel unchanged, cores 1 and 3 become RUNMP helpers (hv_config.h). Without a
 *   hypervisor they enter the kernel directly.
 * - The primary core takes HV_RESERVE bytes off the top of the last RAM block in the device tree
 *   (hv_fdt.c), copies the resident hypervisor (hv.bin, big-endian data) there and jumps to it at
 *   EL2. The hypervisor enters the kernel at EL1 (see hv_entry.S). If the device tree is refused
 *   the kernel starts as with the stock armstub: no native calls, but it boots.
 * - The EL3 setup moved behind the firmware-defined words at 0xd8..0xff.
 * - setup_gic makes PPI 26 (the EL2 timer) Group 0 and lets the GIC signal Group 0 as FIQ: the
 *   hypervisor's time limit of native code (hv_config.h). Secure settings, so Emu68's own
 *   Non-secure GIC setup cannot undo them.
 *
 * This file is little-endian, like every armstub: the firmware writes dtb_ptr32 and
 * kernel_entry32, Emu68 writes the spin_cpuN words with LE64().
 */

#include "hv_config.h"

#define BIT(x) (1 << (x))

#ifdef HIGH_PERI
#define LOCAL_CONTROL		0x4c0000000
#define LOCAL_PRESCALER		0x4c0000008
#define GIC_DISTB		0x4c0041000
#define GIC_CPUB		0x4c0042000
#else
#define LOCAL_CONTROL		0xff800000
#define LOCAL_PRESCALER		0xff800008
#define GIC_DISTB		0xff841000
#define GIC_CPUB		0xff842000
#endif

#define OSC_FREQ		54000000

#define SCR_RW			BIT(10)
#define SCR_HCE			BIT(8)
#define SCR_SMD			BIT(7)
#define SCR_RES1_5		BIT(5)
#define SCR_RES1_4		BIT(4)
#define SCR_NS			BIT(0)
#define SCR_VAL \
    (SCR_RW | SCR_HCE | SCR_SMD | SCR_RES1_5 | SCR_RES1_4 | SCR_NS)

#define ACTLR_VAL \
	(BIT(0) | BIT(1) | BIT(4) | BIT(5) | BIT(6))

#define CPUECTLR_EL1		S3_1_C15_C2_1
#define CPUECTLR_EL1_SMPEN	BIT(6)

#define SPSR_EL3_D		BIT(9)
#define SPSR_EL3_A		BIT(8)
#define SPSR_EL3_I		BIT(7)
#define SPSR_EL3_F		BIT(6)
#define SPSR_EL3_MODE_EL2H	9
#define SPSR_EL3_VAL \
    (SPSR_EL3_D | SPSR_EL3_A | SPSR_EL3_I | SPSR_EL3_F | SPSR_EL3_MODE_EL2H)

#define L2CTLR_EL1		S3_1_C11_C0_2

#define GICC_CTRLR	0x0
#define GICC_PMR	0x4
#define IT_NR		0x8	// Number of interrupt enable registers (256 total irqs)
#define GICD_CTRLR	0x0
#define GICD_IGROUPR	0x80
#define GICD_ISENABLER	0x100
#define GICD_IPRIORITYR	0x400

.section .text
.globl _start
_start:
	b	el3_setup

	/* EL2, all cores. Must stay below 0xd8. */
in_el2:
	mrs	x6, MPIDR_EL1
	and	x6, x6, #0x3
	cbz	x6, primary_cpu

	adr	x5, spin_cpu0
secondary_spin:
	wfe
	ldr	x0, [x5, x6, lsl #3]
	cbz	x0, secondary_spin
	ldr	x6, hv_base
	cbz	x6, secondary_stock
	add	x6, x6, #4
	br	x6			/* hv_secondary (hypervisor + 4), x0 = entry */
secondary_stock:
	mov	x4, x0
	mov	x0, #0
	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	br	x4

.ltorg

.org 0xd0
hv_base:			/* where core 0 put the hypervisor, 0: nowhere. Stays: the page is reserved */
	.quad 0
.org 0xd8
.globl spin_cpu0
spin_cpu0:
	.quad 0
.org 0xe0
.globl spin_cpu1
spin_cpu1:
	.quad 0
.org 0xe8
.globl spin_cpu2
spin_cpu2:
	.quad 0
.org 0xf0
.globl spin_cpu3
spin_cpu3:
	# Shared with next two symbols/.word
	# FW clears the next 8 bytes after reading the initial value, leaving
	# the location suitable for use as spin_cpu3
.org 0xf0
.globl stub_magic
stub_magic:
	.word 0x5afe570b
.org 0xf4
.globl stub_version
stub_version:
	.word 0
.org 0xf8
.globl dtb_ptr32
dtb_ptr32:
	.word 0x0
.org 0xfc
.globl kernel_entry32
kernel_entry32:
	.word 0x0

.org 0x100

el3_setup:
	/*
	 * LOCAL_CONTROL:
	 * Bit 9 clear: Increment by 1 (vs. 2).
	 * Bit 8 clear: Timer source is 19.2MHz crystal (vs. APB).
	 */
	ldr x0, =LOCAL_CONTROL
	str wzr, [x0]
	/* LOCAL_PRESCALER; divide-by (0x80000000 / register_val) == 1 */
	mov w1, 0x80000000
	str w1, [x0, #(LOCAL_PRESCALER - LOCAL_CONTROL)]

	/* Set L2 read/write cache latency to 3 */
	mrs x0, L2CTLR_EL1
	mov x1, #0x22
	orr x0, x0, x1
	msr L2CTLR_EL1, x0

	/* Set up CNTFRQ_EL0 */
	ldr x0, =OSC_FREQ
	msr CNTFRQ_EL0, x0

	/* Set up CNTVOFF_EL2 */
	msr CNTVOFF_EL2, xzr

	/* The EL2 timer off on every core: its reset value is UNKNOWN, and setup_gic makes its
	   interrupt (PPI 26) a FIQ on every core. Core 2 (the PiStorm housekeeper) never passes
	   through the hypervisor, so nothing else would ever switch it off there (NOTES.md 26.6). */
	msr CNTHP_CTL_EL2, xzr

	/* Enable FP/SIMD */
	/* Bit 10 (TFP) is set to 0 */
	msr CPTR_EL3, xzr

	/* Set up SCR */
	mov x0, #SCR_VAL
	msr SCR_EL3, x0

	/* Set up ACTLR */
	mov x0, #ACTLR_VAL
	msr ACTLR_EL3, x0

	/* Set SMPEN */
	mov x0, #CPUECTLR_EL1_SMPEN
	msr CPUECTLR_EL1, x0

#ifdef GIC
	bl	setup_gic
#endif
	/* EL1 sees the real MIDR/MPIDR (their EL2 copies reset to UNKNOWN) */
	mrs	x0, MIDR_EL1
	msr	VPIDR_EL2, x0
	mrs	x0, MPIDR_EL1
	msr	VMPIDR_EL2, x0

	/*
	 * Set up SCTLR_EL2
	 * All set bits below are res1. LE, no WXN/I/SA/C/A/M
	 */
	ldr x0, =0x30c50830
	msr SCTLR_EL2, x0

	/* Switch to EL2 */
	mov x0, #SPSR_EL3_VAL
	msr spsr_el3, x0
	adr x0, in_el2
	msr elr_el3, x0
	eret

	/* EL2, core 0: install the hypervisor and start it */
primary_cpu:
	ldr	w20, kernel_entry32
	ldr	w19, dtb_ptr32

#ifdef DEBUG_LED
	/* GPIO 42 = output, LED on for 1 s, off for 0.5 s */
	ldr	x10, =GPIO_BASE
	ldr	w11, [x10, #GPFSEL4]
	bic	w11, w11, #(7 << LED_FSEL_SHIFT)
	orr	w11, w11, #(1 << LED_FSEL_SHIFT)
	str	w11, [x10, #GPFSEL4]
	mov	w11, #LED_GPIO_BIT
	str	w11, [x10, #GPSET1]
	ldr	x13, =CNT_FREQ
	bl	led_delay
	str	w11, [x10, #GPCLR1]
	ldr	x13, =(CNT_FREQ / 2)
	bl	led_delay
#endif

	/* Free memory: the armstub ends well below, the firmware loads the kernel at 0x80000 */
	mov	x9, #0x70000
	mov	sp, x9
	sub	sp, sp, #32		/* struct hv_fdt_result */

	mov	x0, x19
	ldr	x1, =HV_RESERVE
	mov	x2, sp
	bl	hv_fdt_reserve_top	/* MMU off: reads and writes reach the RAM itself */
	cbnz	x0, no_hypervisor

	ldr	x22, [sp]		/* base */
	ldr	w21, [sp, #8]		/* gb_mask */

	adr	x5, hv_blob_start
	adr	x7, hv_blob_end
	mov	x6, x22
1:	ldp	x8, x9, [x5], #16
	stp	x8, x9, [x6], #16
	cmp	x5, x7
	b.lo	1b

	adr	x9, hv_base
	str	x22, [x9]

	dsb	sy
	ic	iallu
	dsb	sy
	isb

	mov	x0, x19			/* DTB */
	mov	x1, x20			/* kernel entry */
	mov	x3, x21			/* GBs holding RAM */
	br	x22			/* hv_entry, offset 0 of hv.bin */

	/* The device tree was refused: like the stock armstub, the kernel at EL2 */
no_hypervisor:
#ifdef DEBUG_LED
	mov	x22, x0
	ldr	x10, =GPIO_BASE
	mov	w11, #LED_GPIO_BIT
	str	w11, [x10, #GPSET1]
	ldr	x13, =CNT_FREQ
	bl	led_delay
	str	w11, [x10, #GPCLR1]
	ldr	x13, =(CNT_FREQ / 2)
	bl	led_delay
	ldr	x13, =(CNT_FREQ / 5)
2:	str	w11, [x10, #GPSET1]
	bl	led_delay
	str	w11, [x10, #GPCLR1]
	bl	led_delay
	subs	x22, x22, #1
	b.ne	2b
#endif
	mov	x0, x19
	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	br	x20

#ifdef DEBUG_LED
	/* busy wait x13 counter ticks; uses x12, x14 */
led_delay:
	mrs	x12, CNTPCT_EL0
	add	x14, x12, x13
1:	mrs	x12, CNTPCT_EL0
	cmp	x12, x14
	b.lo	1b
	ret
#endif

.ltorg

#ifdef GIC

setup_gic:				// Called from secure mode - set all interrupts to group 1 and enable.
	mrs	x0, MPIDR_EL1
	ldr	x2, =GIC_DISTB
	tst	x0, #0x3
	b.eq	2f			// primary core

	mov	w0, #3			// Enable group 0 and 1 IRQs from distributor
	str	w0, [x2, #GICD_CTRLR]
2:
	add	x1, x2, #(GIC_CPUB - GIC_DISTB)
	mov	w0, #0x1ef
	str	w0, [x1, #GICC_CTRLR]	// Enable group 0 and 1 from the CPU interface, group 0 as FIQ
	mov	w0, #0xff
	str	w0, [x1, #GICC_PMR]	// priority mask
	add	x3, x2, #GICD_IGROUPR
	mov	x0, #(IT_NR * 4)
	mov	w1, #~0			// group 1 all the things
3:
	subs	x0, x0, #4
	str	w1, [x3, x0]
	b.ne	3b
	/* ... except PPI 26, the EL2 timer: group 0 (FIQ), highest priority, enabled. Banked per
	   core, so every core does it here. The hypervisor's time limit (hv_config.h). */
	mov	w1, #~(1 << HV_TIMER_PPI)
	str	w1, [x3]		// GICD_IGROUPR0
	strb	wzr, [x2, #(GICD_IPRIORITYR + HV_TIMER_PPI)]
	mov	w1, #(1 << HV_TIMER_PPI)
	str	w1, [x2, #GICD_ISENABLER]
	ret

.ltorg

#endif

	.balign 16
hv_blob_start:
	.incbin "hv.bin"
	.balign 16
hv_blob_end:
