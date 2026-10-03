// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple T8132 CPU start and stop through m1n1's RVBAR mailbox
 * (enable-method "apple,rvbar").
 *
 * iBoot locks every core's RVBAR to m1n1's vectors. m1n1 keeps its vectors,
 * a reset dispatcher and a mailbox in a page it reserves for us: a core that
 * comes out of reset finds its {MPIDR | valid, entry} slot and jumps to the
 * entry at EL2 with the MMU off. The same mailbox brings the boot CPU back
 * from S2R.
 *
 * Core power follows macOS 27.0 (26A428): ApplePMGR::configMiscCores
 * (kernelcache fffffe0009b947f4) powers a core on by writing its bit to the
 * CPU start block at +0x4 (1 << (4 * cluster + core)) and +0x8 + 4 * cluster
 * (1 << core), and off by writing 1 << (4 * cluster + core) to +0x0. The
 * dying core then runs arm64_prepare_for_sleep (fffffe000bc31600).
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/types.h>

#include <asm/cpu_ops.h>
#include <asm/smp_apple_rvbar.h>
#include <asm/smp_plat.h>
#include <asm/sysreg.h>

#define RVBAR_MAILBOX_MAGIC	0x4b414c3252564241ULL
#define RVBAR_MAILBOX_SLOTS	24
#define RVBAR_SLOT_VALID	BIT_ULL(63)

struct rvbar_slot {
	u64 mpidr;
	u64 entry;
};

struct rvbar_mailbox {
	u64 magic;
	u64 reserved;
	struct rvbar_slot slots[RVBAR_MAILBOX_SLOTS];
};

#define CPU_START_STOP		0x0
#define CPU_START_SYS		0x4
#define CPU_START_CORE(cl)	(0x8 + 4 * (cl))

/* cpu-impl-reg + 0x100: non-zero low byte while the core is powered (m1n1 smp_stop_cpu). */
#define CPU_IMPL_PWR		0x100
#define CPU_IMPL_PWR_ON		GENMASK(7, 0)

/* arm64_prepare_for_sleep's system registers. */
#define SYS_APL_SIQ_CFG_EL1	sys_reg(3, 4, 15, 10, 4)
#define SYS_APL_CORE_PD_EL1	sys_reg(3, 1, 15, 7, 4)
#define SYS_APL_SLEEP_EL1	sys_reg(3, 5, 15, 6, 2)
#define SYS_APL_IPI_SR_EL1	sys_reg(3, 5, 15, 1, 1)

static phys_addr_t rvbar_mailbox_pa, rvbar_cpu_start_pa;
static struct rvbar_mailbox __iomem *rvbar_mailbox;
static void __iomem *rvbar_cpu_start;

static struct {
	u32 cluster;
	u32 core;
	phys_addr_t impl_pa;
	void __iomem *impl;
} rvbar_cpus[NR_CPUS];

static int __init apple_rvbar_cpu_init(unsigned int cpu)
{
	struct device_node *dn, *mb;
	u64 impl;
	int ret;

	if (!rvbar_mailbox_pa) {
		mb = of_find_compatible_node(NULL, NULL, "apple,rvbar-mailbox");
		if (!mb)
			return -ENODEV;
		ret = of_property_read_u64(mb, "apple,mailbox", &rvbar_mailbox_pa) ?:
		      of_property_read_u64(mb, "apple,cpu-start", &rvbar_cpu_start_pa);
		of_node_put(mb);
		if (ret)
			return ret;
	}

	dn = of_get_cpu_node(cpu, NULL);
	if (!dn)
		return -ENODEV;
	ret = of_property_read_u32_index(dn, "apple,pmgr-cpu", 0, &rvbar_cpus[cpu].cluster) ?:
	      of_property_read_u32_index(dn, "apple,pmgr-cpu", 1, &rvbar_cpus[cpu].core) ?:
	      of_property_read_u64(dn, "apple,cpu-impl-reg", &impl);
	of_node_put(dn);
	rvbar_cpus[cpu].impl_pa = impl;
	return ret;
}

static int apple_rvbar_cpu_prepare(unsigned int cpu)
{
	if (!rvbar_mailbox) {
		rvbar_mailbox = ioremap(rvbar_mailbox_pa, sizeof(*rvbar_mailbox));
		rvbar_cpu_start = ioremap(rvbar_cpu_start_pa, CPU_START_CORE(8));
		if (!rvbar_mailbox || !rvbar_cpu_start)
			return -ENOMEM;
	}
	rvbar_cpus[cpu].impl = ioremap(rvbar_cpus[cpu].impl_pa, CPU_IMPL_PWR + 4);
	return rvbar_cpus[cpu].impl ? 0 : -ENOMEM;
}

/* Point @cpu's mailbox slot at @entry for its next reset. */
void apple_rvbar_set_entry(unsigned int cpu, phys_addr_t entry)
{
	struct rvbar_slot __iomem *slot = &rvbar_mailbox->slots[cpu];

	writeq_relaxed(entry, &slot->entry);
	writeq_relaxed(cpu_logical_map(cpu) | RVBAR_SLOT_VALID, &slot->mpidr);
	writeq(RVBAR_MAILBOX_MAGIC, &rvbar_mailbox->magic);
}

static int apple_rvbar_cpu_boot(unsigned int cpu)
{
	u32 cluster = rvbar_cpus[cpu].cluster, core = rvbar_cpus[cpu].core;

	apple_rvbar_set_entry(cpu, __pa_symbol(secondary_entry));

	writel(BIT(4 * cluster + core), rvbar_cpu_start + CPU_START_SYS);
	writel(BIT(core), rvbar_cpu_start + CPU_START_CORE(cluster));
	return 0;
}

/*
 * arm64_prepare_for_sleep: a core power-down sets SIQ_CFG_EL1[1:0] to 3 and
 * clears bit 0 of s3_1_c15_c7_4, system sleep (@deep) sets bits 0 and 63 of
 * s3_5_c15_c6_2; then WFI until the power goes, acknowledging the fast IPI
 * after each spurious wake.
 */
void __noreturn apple_rvbar_core_off(bool deep)
{
	if (deep) {
		sysreg_clear_set_s(SYS_APL_SLEEP_EL1, 0, BIT(0) | BIT(63));
	} else {
		sysreg_clear_set_s(SYS_APL_SIQ_CFG_EL1, 3, 3);
		isb();
		sysreg_clear_set_s(SYS_APL_CORE_PD_EL1, BIT(0), 0);
		isb();
	}

	for (;;) {
		dsb(sy);
		isb();
		wfi();
		write_sysreg_s(1, SYS_APL_IPI_SR_EL1);
	}
}

#ifdef CONFIG_HOTPLUG_CPU
static bool apple_rvbar_cpu_can_disable(unsigned int cpu)
{
	return true;
}

static int apple_rvbar_cpu_disable(unsigned int cpu)
{
	return 0;
}

/* AppleARMCPU::quiesceCPU -> ApplePMGR disableCPUCore, then arm64_prepare_for_sleep. */
static void apple_rvbar_cpu_die(unsigned int cpu)
{
	u32 cluster = rvbar_cpus[cpu].cluster, core = rvbar_cpus[cpu].core;

	writel(BIT(4 * cluster + core), rvbar_cpu_start + CPU_START_STOP);
	apple_rvbar_core_off(false);
}

static int apple_rvbar_cpu_kill(unsigned int cpu)
{
	u32 pwr;

	return readl_poll_timeout(rvbar_cpus[cpu].impl + CPU_IMPL_PWR, pwr,
				  !(pwr & CPU_IMPL_PWR_ON), 100, 50000);
}
#endif

const struct cpu_operations smp_apple_rvbar_ops = {
	.name		= "apple,rvbar",
	.cpu_init	= apple_rvbar_cpu_init,
	.cpu_prepare	= apple_rvbar_cpu_prepare,
	.cpu_boot	= apple_rvbar_cpu_boot,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_can_disable = apple_rvbar_cpu_can_disable,
	.cpu_disable	= apple_rvbar_cpu_disable,
	.cpu_die	= apple_rvbar_cpu_die,
	.cpu_kill	= apple_rvbar_cpu_kill,
#endif
};
