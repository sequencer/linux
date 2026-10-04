/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Debug (2026-10-04): EL1 access to s3_5_c15_c6_2, entered from cpu_die with
 * x1 = the report block (physical, MMU off): +0x0 progress, +0x8 ESR_EL1 of
 * an EL1 exception, +0x10 mode (in), +0x18 c6_2 as read, +0x20 c6_2 after
 * the write. Progress: 0x22 at EL1, 0x33 after the read, 0x44 after the
 * write-back, 0x55 after setting bits 0 and 63 (mode 4); 0xee from the
 * vectors. Mode 1 stops at EL1, 2 after the read, 3 after the write-back.
 */

#include <linux/linkage.h>

	.text
	.balign	2048
SYM_CODE_START(apple_rvbar_el1_probe_vectors)
	.rept	16
	mrs	x9, esr_el1
	str	x9, [x1, #8]
	mov	x2, #0xee
	str	x2, [x1]
	dsb	sy
	wfi
	b	. - 8
	.balign	128
	.endr
SYM_CODE_END(apple_rvbar_el1_probe_vectors)

/*
 * WFI on T8132 may enter retention, which keeps sp and pc and clears every
 * other general purpose register (XNU arm64_retention_wfi, fffffe000bc31680;
 * apple-cpuidle-t8132). The callee-saved registers go on the stack around it.
 */
SYM_FUNC_START(apple_rvbar_retention_wfi)
	str	x30, [sp, #-16]!
	stp	x28, x29, [sp, #-16]!
	stp	x26, x27, [sp, #-16]!
	stp	x24, x25, [sp, #-16]!
	stp	x22, x23, [sp, #-16]!
	stp	x20, x21, [sp, #-16]!
	stp	x18, x19, [sp, #-16]!
	dsb	sy
	isb
	wfi
	ldp	x18, x19, [sp], #16
	ldp	x20, x21, [sp], #16
	ldp	x22, x23, [sp], #16
	ldp	x24, x25, [sp], #16
	ldp	x26, x27, [sp], #16
	ldp	x28, x29, [sp], #16
	ldr	x30, [sp], #16
	ret
SYM_FUNC_END(apple_rvbar_retention_wfi)

SYM_CODE_START(apple_rvbar_el1_probe)
	ldr	x10, [x1, #0x10]
	mov	x2, #0x22
	str	x2, [x1]
	cmp	x10, #1
	b.eq	1f
	mrs	x8, s3_5_c15_c6_2
	str	x8, [x1, #0x18]
	mov	x2, #0x33
	str	x2, [x1]
	cmp	x10, #2
	b.eq	1f
	msr	s3_5_c15_c6_2, x8
	isb
	mov	x2, #0x44
	str	x2, [x1]
	cmp	x10, #3
	b.eq	1f
	orr	x8, x8, #1
	orr	x8, x8, #(1 << 63)
	msr	s3_5_c15_c6_2, x8
	isb
	mrs	x8, s3_5_c15_c6_2
	str	x8, [x1, #0x20]
	mov	x2, #0x55
	str	x2, [x1]
1:	dsb	sy
	wfi
	b	1b
SYM_CODE_END(apple_rvbar_el1_probe)
