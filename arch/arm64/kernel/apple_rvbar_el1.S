/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arm64_prepare_for_sleep(1) (macOS 27.0 26A428 kernel, fffffe000bc31640..
 * bc31664), entered at EL1 with the MMU off by apple_rvbar_core_off(): the
 * s3_5_c15_c6_2 write UNDEFs at EL2 on T8132 (2026-10-04), and a core taking
 * this path never returns to the kernel; it next runs from RVBAR at EL2.
 */

#include <linux/linkage.h>

	.text
	.balign	2048
SYM_CODE_START(apple_rvbar_el1_vectors)
	/* Any exception at EL1 (the write trapping here too) parks the core. */
	.rept	16
	dsb	sy
	wfi
	b	. - 4
	.balign	128
	.endr
SYM_CODE_END(apple_rvbar_el1_vectors)

SYM_CODE_START(apple_rvbar_el1_sleep)
	mrs	x0, s3_5_c15_c6_2
	orr	x0, x0, #1
	orr	x0, x0, #(1 << 63)
	msr	s3_5_c15_c6_2, x0
1:	dsb	sy
	isb
	wfi
	mov	x1, #1
	msr	s3_5_c15_c1_1, x1	/* IPI_SR: acknowledge a fast IPI */
	b	1b
SYM_CODE_END(apple_rvbar_el1_sleep)
