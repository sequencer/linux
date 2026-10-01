// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Copyright The Asahi Linux Contributors
 *
 * CPU idle support for Apple SoCs
 */

#include <linux/init.h>
#include <linux/cpuidle.h>
#include <linux/cpu_pm.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <asm/cpuidle.h>

enum idle_state {
	STATE_WFI,
	STATE_PWRDOWN,
	STATE_COUNT
};

asm(
	".pushsection .cpuidle.text, \"ax\"\n"
	".type apple_cpu_deep_wfi, @function\n"
	"apple_cpu_deep_wfi:\n"
		"str x30, [sp, #-16]!\n"
		"stp x28, x29, [sp, #-16]!\n"
		"stp x26, x27, [sp, #-16]!\n"
		"stp x24, x25, [sp, #-16]!\n"
		"stp x22, x23, [sp, #-16]!\n"
		"stp x20, x21, [sp, #-16]!\n"
		"stp x18, x19, [sp, #-16]!\n"

		"mrs x0, s3_5_c15_c5_0\n"
		"orr x0, x0, #(3L << 24)\n"
		"msr s3_5_c15_c5_0, x0\n"

	"1:\n"
		"dsb sy\n"
		"wfi\n"

		"mrs x0, ISR_EL1\n"
		"cbz x0, 1b\n"

		"mrs x0, s3_5_c15_c5_0\n"
		"bic x0, x0, #(1L << 24)\n"
		"msr s3_5_c15_c5_0, x0\n"

		"ldp x18, x19, [sp], #16\n"
		"ldp x20, x21, [sp], #16\n"
		"ldp x22, x23, [sp], #16\n"
		"ldp x24, x25, [sp], #16\n"
		"ldp x26, x27, [sp], #16\n"
		"ldp x28, x29, [sp], #16\n"
		"ldr x30, [sp], #16\n"

		"ret\n"
	".popsection\n"
);

void apple_cpu_deep_wfi(void);

/*
 * T8132: a core that executes WFI may enter the retention state, which keeps
 * sp and pc and clears every other general purpose register. CYC_OVRD, which
 * selects a retaining WFI on the older chips, is locked on this one, and XNU
 * 27.0 (26A428) does not write it either. It handles retention in software:
 *
 *  - cpu_idle (fffffe000be074e8) runs on the context saved by Idle_context
 *    and calls arm64_retention_wfi at fffffe000be0766c.
 *  - arm64_retention_wfi (fffffe000bc31680) is "wfi; cbz x30, lost; ret".
 *    A zero link register means the registers were lost, and it then calls
 *    cpu_idle_exit(false) (fffffe000be0769c) itself.
 *  - cpu_idle_exit(false) writes no system register and ends in
 *    Idle_load_context (fffffe000bc306e0).
 *
 * So the callee-saved registers go on the stack around WFI. On J713 the
 * secondary cores lost their registers on practically every WFI, the boot
 * core on none.
 */
asm(
	".pushsection .cpuidle.text, \"ax\"\n"
	".type apple_cpu_retention_wfi, @function\n"
	"apple_cpu_retention_wfi:\n"
		"str x30, [sp, #-16]!\n"
		"stp x28, x29, [sp, #-16]!\n"
		"stp x26, x27, [sp, #-16]!\n"
		"stp x24, x25, [sp, #-16]!\n"
		"stp x22, x23, [sp, #-16]!\n"
		"stp x20, x21, [sp, #-16]!\n"
		"stp x18, x19, [sp, #-16]!\n"

		"dsb sy\n"
		"wfi\n"

		"ldp x18, x19, [sp], #16\n"
		"ldp x20, x21, [sp], #16\n"
		"ldp x22, x23, [sp], #16\n"
		"ldp x24, x25, [sp], #16\n"
		"ldp x26, x27, [sp], #16\n"
		"ldp x28, x29, [sp], #16\n"
		"ldr x30, [sp], #16\n"

		"ret\n"
	".popsection\n"
);

void apple_cpu_retention_wfi(void);

static __cpuidle int apple_enter_wfi(struct cpuidle_device *dev, struct cpuidle_driver *drv, int index)
{
	cpu_do_idle();
	return index;
}

static __cpuidle int apple_enter_idle(struct cpuidle_device *dev, struct cpuidle_driver *drv, int index)
{
	/*
	 * Deep WFI will clobber FP state, among other things.
	 * The CPU PM notifier will take care of saving that and anything else
	 * that needs to be notified of the CPU powering down.
	 */
	if (cpu_pm_enter())
		return -1;

	ct_cpuidle_enter();

	switch(index) {
	case STATE_PWRDOWN:
		apple_cpu_deep_wfi();
		break;
	default:
		WARN_ON(1);
		break;
	}

	ct_cpuidle_exit();

	cpu_pm_exit();

	return index;
}

static __cpuidle int apple_enter_retention(struct cpuidle_device *dev, struct cpuidle_driver *drv, int index)
{
	/* FP/SIMD and SME state is treated as lost with the other registers. */
	if (cpu_pm_enter())
		return -1;

	ct_cpuidle_enter();
	apple_cpu_retention_wfi();
	ct_cpuidle_exit();

	cpu_pm_exit();

	return index;
}

static struct cpuidle_driver apple_idle_driver = {
	.name = "apple_idle",
	.owner = THIS_MODULE,
	.states = {
		[STATE_WFI] = {
			.enter			= apple_enter_wfi,
			.enter_s2idle		= apple_enter_wfi,
			.exit_latency		= 1,
			.target_residency	= 1,
			.power_usage            = UINT_MAX,
			.name			= "WFI",
			.desc			= "CPU clock-gated",
			.flags			= 0,
		},
		[STATE_PWRDOWN] = {
			.enter			= apple_enter_idle,
			.enter_s2idle		= apple_enter_idle,
			.exit_latency		= 10,
			.target_residency	= 10000,
			.power_usage            = 0,
			.name			= "CPU PD",
			.desc			= "CPU/cluster powered down",
			.flags			= CPUIDLE_FLAG_RCU_IDLE,
		},
	},
	.safe_state_index = STATE_WFI,
	.state_count = STATE_COUNT,
};

/*
 * Like XNU, T8132 has the one idle state. A plain WFI state cannot exist
 * there, and the kernel is booted with idle=nop so that the idle loop outside
 * cpuidle does not execute WFI either.
 *
 * exit_latency is the added wake-up lateness of a 2 ms periodic timer on
 * J713 against the spinning idle loop: 10.5 us on an efficiency core, 4.3 us
 * on a performance core, 13 us at worst.
 */
static struct cpuidle_driver apple_t8132_idle_driver = {
	.name = "apple_idle",
	.owner = THIS_MODULE,
	.states = {
		{
			.enter			= apple_enter_retention,
			.enter_s2idle		= apple_enter_retention,
			.exit_latency		= 13,
			.target_residency	= 13,
			.power_usage            = 0,
			.name			= "WFI",
			.desc			= "WFI with registers saved",
			.flags			= CPUIDLE_FLAG_RCU_IDLE,
		},
	},
	.safe_state_index = 0,
	.state_count = 1,
};

static int apple_cpuidle_probe(struct platform_device *pdev)
{
	if (of_machine_is_compatible("apple,t8132"))
		return cpuidle_register(&apple_t8132_idle_driver, NULL);

	return cpuidle_register(&apple_idle_driver, NULL);
}

static struct platform_driver apple_cpuidle_driver = {
	.driver = {
		.name = "cpuidle-apple",
	},
	.probe = apple_cpuidle_probe,
};

static int __init apple_cpuidle_init(void)
{
	struct platform_device *pdev;
	int ret;

	ret = platform_driver_register(&apple_cpuidle_driver);
	if (ret)
		return ret;

	if (!of_machine_is_compatible("apple,arm-platform"))
		return 0;

	if (!(of_machine_is_compatible("apple,t8103") ||
	      of_machine_is_compatible("apple,t8112") ||
	      of_machine_is_compatible("apple,t8122") ||
	      of_machine_is_compatible("apple,t8132") ||
	      of_machine_is_compatible("apple,t6000") ||
	      of_machine_is_compatible("apple,t6001") ||
	      of_machine_is_compatible("apple,t6002") ||
	      of_machine_is_compatible("apple,t6020") ||
	      of_machine_is_compatible("apple,t6021") ||
	      of_machine_is_compatible("apple,t6022") ||
	      of_machine_is_compatible("apple,t6030") ||
	      of_machine_is_compatible("apple,t6031") ||
	      of_machine_is_compatible("apple,t6032") ||
	      of_machine_is_compatible("apple,t6034")))
		return 0;

	pdev = platform_device_register_simple("cpuidle-apple", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		platform_driver_unregister(&apple_cpuidle_driver);
		return PTR_ERR(pdev);
	}

	return 0;
}
device_initcall(apple_cpuidle_init);
