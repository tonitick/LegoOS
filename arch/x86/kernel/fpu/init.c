/*
 * Copyright (c) 2016-2019 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */


#include <lego/sched.h>
#include <lego/kernel.h>

#include <asm/asm.h>
#include <asm/tlbflush.h>
#include <asm/processor.h>
#include <asm/fpu/internal.h>

/*
 * Initialize the TS bit in CR0 according to the style of context-switches
 * we are using:
 */
static void fpu__init_cpu_ctx_switch(void)
{
	/*
	 * If the TS flag is set and the EM flag (bit 2 of CR0) is clear,
	 * a device-not-available exception (#NM) is raised prior to the
	 * execution of any x87 FPU/MMX/SSE/SSE2/SSE3/SSSE3/SSE4.
	 *
	 * The fault handler for the #NM exception can then be used to
	 * clear the TS flag (with the CLTS instruction) and save the
	 * context of the x87 FPU, XMM, and MXCSR registers.
	 *
	 *	Yizhou, 24 Jul 2017
	 */
	if (!cpu_has(X86_FEATURE_EAGER_FPU))
		stts();
	else
		clts();
}

/*
 * Initialize the registers found in all CPUs, CR0 and CR4:
 */
static void fpu__init_cpu_generic(void)
{
	unsigned long cr0;
	unsigned long cr4_mask = 0;

	if (cpu_has(X86_FEATURE_FXSR)) {
		cr4_mask |= X86_CR4_OSFXSR;
		pr_debug_once("x86/fpu: cpu has FXSR\n");
	}
	if (cpu_has(X86_FEATURE_XMM)) {
		cr4_mask |= X86_CR4_OSXMMEXCPT;
		pr_debug_once("x86/fpu: cpu has XMM\n");
	}
	if (cr4_mask)
		cr4_set_bits(cr4_mask);

	cr0 = read_cr0();
	cr0 &= ~(X86_CR0_TS|X86_CR0_EM); /* clear TS and EM */
	if (!cpu_has(X86_FEATURE_FPU))
		cr0 |= X86_CR0_EM;
	write_cr0(cr0);

	asm volatile ("fninit");
}

/*
 * Enable all supported FPU features. Called when a CPU is brought online:
 */
void fpu__init_cpu(void)
{
	fpu__init_cpu_generic();
	fpu__init_cpu_xstate();
	fpu__init_cpu_ctx_switch();
}

/*
 * The earliest FPU detection code.
 *
 * Set the X86_FEATURE_FPU CPU-capability bit based on
 * trying to execute an actual sequence of FPU instructions:
 */
static void fpu__init_system_early_generic(struct cpu_info *c)
{
	unsigned long cr0;
	u16 fsw, fcw;

	fsw = fcw = 0xffff;

	cr0 = read_cr0();
	cr0 &= ~(X86_CR0_TS | X86_CR0_EM);
	write_cr0(cr0);

	/*
	 * Lego has to use FPU
	 * If somehow this is set, bug indeed.
	 */
	BUG_ON(test_bit(X86_FEATURE_FPU, (unsigned long *)cpu_caps_cleared));

	asm volatile("fninit ; fnstsw %0 ; fnstcw %1"
		     : "+m" (fsw), "+m" (fcw));

	if (fsw == 0 && (fcw & 0x103f) == 0x003f)
		set_cpu_cap(c, X86_FEATURE_FPU);
	else
		clear_cpu_cap(c, X86_FEATURE_FPU);

	if (!cpu_has(X86_FEATURE_FPU)) {
		pr_emerg("x86/fpu: Giving up, no FPU found and no math emulation present\n");
		for (;;)
			asm volatile("hlt");
	}
}

/*
 * Boot time FPU feature detection code:
 */
unsigned int mxcsr_feature_mask __read_mostly = 0xffffffffu;

static void __init fpu__init_system_mxcsr(void)
{
	unsigned int mask = 0;

	if (cpu_has(X86_FEATURE_FXSR)) {
		/* Static because GCC does not get 16-byte stack alignment right: */
		static struct fxregs_state fxregs __initdata;

		asm volatile("fxsave %0" : "+m" (fxregs));

		mask = fxregs.mxcsr_mask;

		/*
		 * If zero then use the default features mask,
		 * which has all features set, except the
		 * denormals-are-zero feature bit:
		 */
		if (mask == 0)
			mask = 0x0000ffbf;
	}
	mxcsr_feature_mask &= mask;
}

/*
 * Once per bootup FPU initialization sequences that will run on most x86 CPUs:
 */
static void __init fpu__init_system_generic(void)
{
	/*
	 * Set up the legacy init FPU context. (xstate init might overwrite this
	 * with a more modern format, if the CPU supports it.)
	 */
	fpstate_init(&init_fpstate);

	fpu__init_system_mxcsr();
}

/*
 * Size of the FPU context state. All tasks in the system use the
 * same context size, regardless of what portion they use.
 * This is inherent to the XSAVE architecture which puts all state
 * components into a single, continuous memory block:
 */
unsigned int fpu_kernel_xstate_size;

/* Get alignment of the TYPE. */
#define TYPE_ALIGN(TYPE) offsetof(struct { char x; TYPE test; }, test)

/*
 * Enforce that 'MEMBER' is the last field of 'TYPE'.
 *
 * Align the computed size with alignment of the TYPE,
 * because that's how C aligns structs.
 */
#define CHECK_MEMBER_AT_END_OF(TYPE, MEMBER) \
	BUILD_BUG_ON(sizeof(TYPE) != ALIGN(offsetofend(TYPE, MEMBER), \
					   TYPE_ALIGN(TYPE)))

/*
 * We append the 'struct fpu' to the task_struct:
 */
static void __init fpu__init_task_struct_size(void)
{
	int task_size = sizeof(struct task_struct);

	/*
	 * Subtract off the static size of the register state.
	 * It potentially has a bunch of padding.
	 */
	task_size -= sizeof(((struct task_struct *)0)->thread.fpu.state);

	/*
	 * Add back the dynamically-calculated register state
	 * size.
	 */
	task_size += fpu_kernel_xstate_size;

	/*
	 * We dynamically size 'struct fpu', so we require that
	 * it be at the end of 'thread_struct' and that
	 * 'thread_struct' be at the end of 'task_struct'.  If
	 * you hit a compile error here, check the structure to
	 * see if something got added to the end.
	 */
	CHECK_MEMBER_AT_END_OF(struct fpu, state);
	CHECK_MEMBER_AT_END_OF(struct thread_struct, fpu);
	CHECK_MEMBER_AT_END_OF(struct task_struct, thread);

	/*
	 * According to Intel SDM, xsave must be 64-bytes aligned
	 * Any misalignment lead to general-protection (#GP) exception.
	 *
	 * Since this #GP will be hidden by fixup during runtime,
	 * if CONFIG_X86_64 is not enabled, we will not notice anything
	 * except some random crash.
	 *
	 * Thus, make sure, xsave is 64-bytes aligned to both struct fpu
	 * and struct task_struct. By doing so, we only need to make sure
	 * allocated task_struct is 64-bytes aligned at runtime.
	 */
	BUILD_BUG_ON((offsetof(struct fpu, state.xsave) & (64-1)) != 0);
	BUILD_BUG_ON((offsetof(struct task_struct, thread.fpu.state.xsave) & (64-1)) != 0);

	arch_task_struct_size = task_size;
	arch_task_struct_order = get_order(arch_task_struct_size);
}

/*
 * Set up the user and kernel xstate sizes based on the legacy FPU context size.
 *
 * We set this up first, and later it will be overwritten by
 * fpu__init_system_xstate() if the CPU knows about xstates.
 */
static void __init fpu__init_system_xstate_size_legacy(void)
{
	static int on_boot_cpu __initdata = 1;

	WARN_ON(!on_boot_cpu);
	on_boot_cpu = 0;

	/*
	 * Note that xstate sizes might be overwritten later during
	 * fpu__init_system_xstate().
	 */

	if (!cpu_has(X86_FEATURE_FPU)) {
		/*
		 * Disable xsave as we do not support it if i387
		 * emulation is enabled.
		 */
		BUG();
		setup_clear_cpu_cap(X86_FEATURE_XSAVE);
		setup_clear_cpu_cap(X86_FEATURE_XSAVEOPT);
		fpu_kernel_xstate_size = sizeof(struct swregs_state);
	} else {
		if (cpu_has(X86_FEATURE_FXSR))
			fpu_kernel_xstate_size =
				sizeof(struct fxregs_state);
		else
			fpu_kernel_xstate_size =
				sizeof(struct fregs_state);
	}

	fpu_user_xstate_size = fpu_kernel_xstate_size;
	pr_info("fpu_user_xstate_size(fpu_kernel_xstate_size): size=%u\n", fpu_user_xstate_size);
}

/*
 * FPU context switching strategies:
 *
 * Against popular belief, we don't do lazy FPU saves, due to the
 * task migration complications it brings on SMP - we only do
 * lazy FPU restores.
 *
 * 'lazy' is the traditional strategy, which is based on setting
 * CR0::TS to 1 during context-switch (instead of doing a full
 * restore of the FPU state), which causes the first FPU instruction
 * after the context switch (whenever it is executed) to fault - at
 * which point we lazily restore the FPU state into FPU registers.
 *
 * Tasks are of course under no obligation to execute FPU instructions,
 * so it can easily happen that another context-switch occurs without
 * a single FPU instruction being executed. If we eventually switch
 * back to the original task (that still owns the FPU) then we have
 * not only saved the restores along the way, but we also have the
 * FPU ready to be used for the original task.
 *
 * 'lazy' is deprecated because it's almost never a performance win
 * and it's much more complicated than 'eager'.
 *
 * 'eager' switching is by default on all CPUs, there we switch the FPU
 * state during every context switch, regardless of whether the task
 * has used FPU instructions in that time slice or not. This is done
 * because modern FPU context saving instructions are able to optimize
 * state saving and restoration in hardware: they can detect both
 * unused and untouched FPU state and optimize accordingly.
 *
 * [ Note that even in 'lazy' mode we might optimize context switches
 *   to use 'eager' restores, if we detect that a task is using the FPU
 *   frequently. See the fpu->counter logic in fpu/internal.h for that. ]
 */
static enum { ENABLE, DISABLE } eagerfpu = ENABLE;

/*
 * Find supported xfeatures based on cpu features and command-line input.
 * This must be called after fpu__init_parse_early_param() is called and
 * xfeatures_mask is enumerated.
 */
u64 __init fpu__get_supported_xfeatures_mask(void)
{
	/* Support all xfeatures known to us */
	if (eagerfpu != DISABLE)
		return XCNTXT_MASK;

	/* Warning of xfeatures being disabled for no eagerfpu mode */
	if (xfeatures_mask & XFEATURE_MASK_EAGER) {
		pr_err("x86/fpu: eagerfpu switching disabled, disabling the following xstate features: 0x%llx.\n",
			xfeatures_mask & XFEATURE_MASK_EAGER);
	}

	/* Return a mask that masks out all features requiring eagerfpu mode */
	return ~XFEATURE_MASK_EAGER;
}

/*
 * Pick the FPU context switching strategy:
 *
 * When eagerfpu is AUTO or ENABLE, we ensure it is ENABLE if either of
 * the following is true:
 *
 * (1) the cpu has xsaveopt, as it has the optimization and doing eager
 *     FPU switching has a relatively low cost compared to a plain xsave;
 * (2) the cpu has xsave features (e.g. MPX) that depend on eager FPU
 *     switching. Should the kernel boot with noxsaveopt, we support MPX
 *     with eager FPU switching at a higher cost.
 */
static void __init fpu__init_system_ctx_switch(void)
{
	static bool on_boot_cpu __initdata = 1;

	WARN_ON(!on_boot_cpu);
	on_boot_cpu = 0;

	WARN_ON(current->thread.fpu.fpstate_active);

	if (cpu_has(X86_FEATURE_XSAVEOPT) && eagerfpu != DISABLE)
		eagerfpu = ENABLE;

	if (xfeatures_mask & XFEATURE_MASK_EAGER)
		eagerfpu = ENABLE;

	if (eagerfpu == ENABLE)
		setup_force_cpu_cap(X86_FEATURE_EAGER_FPU);

	printk(KERN_INFO "x86/fpu: Using '%s' FPU context switches.\n", eagerfpu == ENABLE ? "eager" : "lazy");
}

/*
 * Called on the boot CPU once per system bootup, to set up the initial
 * FPU state that is later cloned into all processes:
 */
void __init fpu__init_system(struct cpu_info *c)
{
	fpu__init_system_early_generic(c);

	/*
	 * The FPU has to be operational for some of the
	 * later FPU init activities:
	 */
	fpu__init_cpu();

	/*
	 * But don't leave CR0::TS set yet, as some of the FPU setup
	 * methods depend on being able to execute FPU instructions
	 * that will fault on a set TS, such as the FXSAVE in
	 * fpu__init_system_mxcsr().
	 */
	clts();

	fpu__init_system_generic();
	fpu__init_system_xstate_size_legacy();
	fpu__init_system_xstate();
	fpu__init_task_struct_size();

	fpu__init_system_ctx_switch();
}
