// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple T8132 CPU start and stop through m1n1's RVBAR mailbox
 * (enable-method "apple,rvbar").
 *
 * iBoot locks every core's RVBAR to m1n1's vectors. m1n1 keeps its vectors,
 * a reset dispatcher and a mailbox in a page it reserves for us: a core that
 * comes out of reset finds its {MPIDR | valid, entry} slot and jumps to the
 * entry at EL2 with the MMU off. A secondary's first start goes through
 * m1n1's spin table (cpu-release-addr), where it is parked from boot; later
 * starts, after the OS powered it off, and the boot CPU's wake from S2R go
 * through the mailbox. Linux sets the mailbox magic when it maps it, so a
 * core that resets without a slot (powered off, woken early) waits in the
 * dispatcher's WFE instead of entering m1n1, whose memory Linux now owns.
 *
 * Core power follows macOS 27.0 (26A428). The "Core" platform function
 * (ADT function-enable_core, mask 1 << cpu-id) reaches
 * ApplePMGR::configMiscCores (kernelcache fffffe0009b947f4). On J713 the pmgr
 * "clusters" property is {6, 4} with no shifts, so the core bits are packed
 * per cluster with a stride of the largest cluster (initDriver
 * fffffe0009b7bf7c..bfa8): a core's bit is cluster * stride + core
 * (fffffe0009b94b88..bcc). Power on writes the packed bit to +0x4 and
 * 1 << core to +0x8 + 4 * cluster (fffffe0009b94bd0..c84); power off writes
 * the packed bit to +0x0 (fffffe0009b94c8c..ecc).
 *
 * AppleARMCPU::quiesceCPU (fffffe0008c6eafc) disables the core in PMGR and
 * tail-calls ml_arm_sleep (fffffe000be07d68), which unlocks the core's
 * CoreSight debug block (LAR +0xfb0 = 0xc5acce55) and clears EDPRCR (+0x310),
 * then calls arm64_prepare_for_sleep(1) (fffffe000be07e80..e98).
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/suspend.h>
#include <linux/types.h>

#include <asm/cacheflush.h>
#include <asm/cpu_ops.h>
#include <asm/kexec.h>
#include <asm/mmu_context.h>
#include <asm/smp_apple_rvbar.h>
#include <asm/smp_plat.h>
#include <asm/suspend.h>
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

/* CoreSight external debug: lock access and EDPRCR (ml_arm_sleep). */
#define CORESIGHT_LAR		0xfb0
#define CORESIGHT_LAR_KEY	0xc5acce55
#define CORESIGHT_EDPRCR	0x310

/* cpu-impl-reg + 0x100: non-zero low byte while the core is powered (m1n1 smp_stop_cpu). */
#define CPU_IMPL_PWR		0x100
#define CPU_IMPL_PWR_ON		GENMASK(7, 0)

/* arm64_prepare_for_sleep's system registers. */
#define SYS_APL_SIQ_CFG_EL1	sys_reg(3, 4, 15, 10, 4)
#define SYS_APL_CORE_OFF_EL1	sys_reg(3, 1, 15, 7, 4)
#define SYS_APL_SLEEP_EL1	sys_reg(3, 5, 15, 6, 2)
#define SYS_APL_IPI_SR_EL1	sys_reg(3, 5, 15, 1, 1)
/* Fast IPI request registers (irq-apple-aic). */
#define SYS_APL_IPI_RR_LOCAL_EL1	sys_reg(3, 5, 15, 0, 0)
#define SYS_APL_IPI_RR_GLOBAL_EL1	sys_reg(3, 5, 15, 0, 1)
#define IPI_RR_CPU		GENMASK(7, 0)
#define IPI_RR_CLUSTER		GENMASK(23, 16)

static phys_addr_t rvbar_mailbox_pa, rvbar_cpu_start_pa;
static struct rvbar_mailbox __iomem *rvbar_mailbox;
static void __iomem *rvbar_cpu_start;
/* Bit stride of a cluster in the packed start/stop mask: its largest core count. */
static u32 rvbar_cluster_stride;

static struct {
	u32 cluster;
	u32 core;
	phys_addr_t impl_pa;
	void __iomem *impl;
	phys_addr_t coresight_pa;
	void __iomem *coresight;
	u64 release_addr;
	bool powered_off;
} rvbar_cpus[NR_CPUS];

static int __init apple_rvbar_cpu_init(unsigned int cpu)
{
	struct device_node *dn, *mb;
	u64 impl, coresight;
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
	      of_property_read_u64(dn, "apple,cpu-impl-reg", &impl) ?:
	      of_property_read_u64(dn, "apple,coresight-reg", &coresight);
	of_property_read_u64(dn, "cpu-release-addr", &rvbar_cpus[cpu].release_addr);
	of_node_put(dn);
	rvbar_cpus[cpu].impl_pa = impl;
	rvbar_cpus[cpu].coresight_pa = coresight;
	rvbar_cluster_stride = max(rvbar_cluster_stride, rvbar_cpus[cpu].core + 1);
	return ret;
}

static int apple_rvbar_cpu_prepare(unsigned int cpu)
{
	if (!rvbar_mailbox) {
		rvbar_mailbox = ioremap(rvbar_mailbox_pa, sizeof(*rvbar_mailbox));
		rvbar_cpu_start = ioremap(rvbar_cpu_start_pa, CPU_START_CORE(8));
		if (!rvbar_mailbox || !rvbar_cpu_start)
			return -ENOMEM;
		/* From here every reset is the OS's: m1n1's memory belongs to Linux. */
		writeq(RVBAR_MAILBOX_MAGIC, &rvbar_mailbox->magic);
	}
	rvbar_cpus[cpu].impl = ioremap(rvbar_cpus[cpu].impl_pa, CPU_IMPL_PWR + 4);
	rvbar_cpus[cpu].coresight = ioremap(rvbar_cpus[cpu].coresight_pa, CORESIGHT_LAR + 4);
	return rvbar_cpus[cpu].impl && rvbar_cpus[cpu].coresight ? 0 : -ENOMEM;
}

/* Point @cpu's mailbox slot at @entry for its next reset. */
void apple_rvbar_set_entry(unsigned int cpu, phys_addr_t entry)
{
	struct rvbar_slot __iomem *slot = &rvbar_mailbox->slots[cpu];

	writeq_relaxed(entry, &slot->entry);
	writeq_relaxed(cpu_logical_map(cpu) | RVBAR_SLOT_VALID, &slot->mpidr);
	writeq(RVBAR_MAILBOX_MAGIC, &rvbar_mailbox->magic);
}
EXPORT_SYMBOL_GPL(apple_rvbar_set_entry);

/* Make @cpu come back from its next reset in cpu_resume (S2R). */
void apple_rvbar_set_resume_entry(unsigned int cpu)
{
	apple_rvbar_set_entry(cpu, __pa_symbol(cpu_resume));
}
EXPORT_SYMBOL_GPL(apple_rvbar_set_resume_entry);

/* First start: release the core from m1n1's spin table like spin-table does. */
static int apple_rvbar_spin_release(unsigned int cpu)
{
	__le64 __iomem *release = ioremap_cache(rvbar_cpus[cpu].release_addr, sizeof(*release));

	if (!release)
		return -ENOMEM;
	writeq_relaxed(__pa_symbol(secondary_entry), release);
	dcache_clean_inval_poc((__force unsigned long)release,
			       (__force unsigned long)release + sizeof(*release));
	iounmap(release);
	sev();
	return 0;
}

/* The core's bit in the packed start (+0x4) and stop (+0x0) masks. */
static u32 apple_rvbar_core_bit(unsigned int cpu)
{
	return BIT(rvbar_cpus[cpu].cluster * rvbar_cluster_stride + rvbar_cpus[cpu].core);
}

static int apple_rvbar_cpu_boot(unsigned int cpu)
{
	u32 cluster = rvbar_cpus[cpu].cluster, core = rvbar_cpus[cpu].core;

	if (!rvbar_cpus[cpu].powered_off)
		return apple_rvbar_spin_release(cpu);

	apple_rvbar_set_entry(cpu, __pa_symbol(secondary_entry));
	/* A core that already woke waits in the dispatcher's WFE for its slot. */
	dsb(sy);
	sev();

	writel(apple_rvbar_core_bit(cpu), rvbar_cpu_start + CPU_START_SYS);
	writel(BIT(core), rvbar_cpu_start + CPU_START_CORE(cluster));

	/*
	 * A core that went down through arm64_prepare_for_sleep(0) is power-gated
	 * like an idle core and comes back through RVBAR on an interrupt; an
	 * immediate fast IPI wakes it (inferred from the idle wake path: PMGR start
	 * alone left E0 down on J713, 2026-10-03).
	 */
	if (MPIDR_AFFINITY_LEVEL(read_cpuid_mpidr(), 1) ==
	    MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1))
		write_sysreg_s(FIELD_PREP(IPI_RR_CPU, MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 0)),
			       SYS_APL_IPI_RR_LOCAL_EL1);
	else
		write_sysreg_s(FIELD_PREP(IPI_RR_CPU, MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 0)) |
			       FIELD_PREP(IPI_RR_CLUSTER, MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 1)),
			       SYS_APL_IPI_RR_GLOBAL_EL1);
	isb();
	return 0;
}

/*
 * A powered-down core does not always lose power on J713 (impl +0x100 stays
 * active, cpu_kill -ETIMEDOUT, 2026-10-03). Woken by cpu_boot's IPI with its
 * slot filled, it then takes the reset path itself: MMU off through the idmap
 * (cpu_soft_restart, as kexec) and into the slot's entry at EL2.
 */
static void apple_rvbar_restart_if_slot(void)
{
	struct rvbar_slot __iomem *slot = &rvbar_mailbox->slots[smp_processor_id()];
	typeof(cpu_soft_restart) *restart;
	u64 entry;

	if (!(readq_relaxed(&slot->mpidr) & RVBAR_SLOT_VALID))
		return;
	entry = readq_relaxed(&slot->entry);
	cpu_install_idmap();
	restart = (void *)__pa_symbol(cpu_soft_restart);
	restart(0, entry, 0, 0, 0);
}

/*
 * arm64_prepare_for_sleep (fffffe000bc31600): a core power-down sets
 * SIQ_CFG_EL1[1:0] to 3 and clears bit 0 of s3_1_c15_c7_4, each followed by
 * an isb; system sleep sets bits 0 and 63 of s3_5_c15_c6_2 instead; then WFI
 * until the power goes, acknowledging the fast IPI after each spurious wake.
 * Under Linux (EL2, VHE) s3_1_c15_c7_4 reads 0x6 on J713, bit 0 already
 * clear, so the clear writes nothing. The system-sleep write is UNDEFINED at
 * EL2 (ESR EC 0; a debugfs write-back of its value 0, 2026-10-04): the Apple
 * system registers XNU writes there are locked for EL2 on T8132, as
 * CYC_OVRD is for m1n1. Every core, the boot CPU in S2R included, therefore
 * takes the power-down variant.
 */
void __noreturn apple_rvbar_core_off(void)
{
	void __iomem *coresight = rvbar_cpus[smp_processor_id()].coresight;

	/* ml_arm_sleep (fffffe000be07e78..e90) before arm64_prepare_for_sleep. */
	writel_relaxed(CORESIGHT_LAR_KEY, coresight + CORESIGHT_LAR);
	writel_relaxed(0, coresight + CORESIGHT_EDPRCR);

	sysreg_clear_set_s(SYS_APL_SIQ_CFG_EL1, 3, 3);
	isb();
	sysreg_clear_set_s(SYS_APL_CORE_OFF_EL1, BIT(0), 0);
	isb();

	for (;;) {
		dsb(sy);
		isb();
		wfi();
		write_sysreg_s(1, SYS_APL_IPI_SR_EL1);
		apple_rvbar_restart_if_slot();
	}
}
EXPORT_SYMBOL_GPL(apple_rvbar_core_off);

#ifdef CONFIG_HOTPLUG_CPU
static bool apple_rvbar_cpu_can_disable(unsigned int cpu)
{
	return true;
}

static int apple_rvbar_cpu_disable(unsigned int cpu)
{
	return 0;
}

/* AppleARMCPU::quiesceCPU -> ApplePMGR disableCPUCore, then ml_arm_sleep. */
static void apple_rvbar_cpu_die(unsigned int cpu)
{
	rvbar_cpus[cpu].powered_off = true;
	/* Any reset before the next cpu_boot parks in the dispatcher. */
	writeq(0, &rvbar_mailbox->slots[cpu].mpidr);
	dsb(sy);
	writel(apple_rvbar_core_bit(cpu), rvbar_cpu_start + CPU_START_STOP);
	/*
	 * ml_arm_sleep (K:be07e4c..be07e98): the secondary goes down at once; the
	 * hold it checks (K:ca47dfc) is only set on a debugger stop path
	 * (K:be05634), and the boot CPU waits for the secondaries' sleep tokens
	 * before its own sleep. XNU's system-sleep variant is not writable at
	 * EL2 (apple_rvbar_core_off): taking it here left a P core on an
	 * undefined instruction (pm_test=core, 2026-10-04).
	 */
	apple_rvbar_core_off();
}

static int apple_rvbar_cpu_kill(unsigned int cpu)
{
	u32 pwr;

	/*
	 * Also in S2R: returning at once there left CPU0 with an SError right
	 * after CPU1 went down (S2R, 2026-10-04), while hotplugging CPU1..6 one by
	 * one with this poll (each -ETIMEDOUT) runs clean.
	 */
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
