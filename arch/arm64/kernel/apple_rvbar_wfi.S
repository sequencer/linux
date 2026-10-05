/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * WFI on T8132 may enter retention, which keeps sp and pc and clears every
 * other general purpose register (XNU arm64_retention_wfi, fffffe000bc31680;
 * apple-cpuidle-t8132). The callee-saved registers go on the stack around it.
 */

#include <linux/linkage.h>

	.text
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
