/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_SMP_APPLE_RVBAR_H
#define __ASM_SMP_APPLE_RVBAR_H

#include <linux/types.h>

void apple_rvbar_set_entry(unsigned int cpu, phys_addr_t entry);
void apple_rvbar_set_resume_entry(unsigned int cpu);
void __noreturn apple_rvbar_core_off(void);

#endif
