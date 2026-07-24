#include <linux/entry-common.h>
#include <linux/syscalls.h>

#undef __SYSCALL
#define __SYSCALL(nr, sym) asmlinkage long sym(const struct pt_regs *regs);
#include <asm/unistd.h>

typedef asmlinkage long (*syscall_handler_t)(const struct pt_regs *regs);

#undef __SYSCALL
#define __SYSCALL(nr, sym) [nr] = (syscall_handler_t)sym,

syscall_handler_t syscall_table[__NR_syscalls] = {
	[0 ... __NR_syscalls - 1] = (syscall_handler_t)sys_ni_syscall,
#include <asm/unistd.h>
};

__attribute__((export_name("syscall"))) long
wasm_syscall(long nr, unsigned long arg0, unsigned long arg1,
	     unsigned long arg2, unsigned long arg3, unsigned long arg4,
	     unsigned long arg5)
{
	struct pt_regs *regs = current_pt_regs();

	regs->user_mode = 0;
	nr = syscall_enter_from_user_mode(regs, nr);

	if (nr < 0 || nr >= ARRAY_SIZE(syscall_table)) {
		regs->user_mode = 1;
		return -ENOSYS;
	}

	regs->syscall_nr = nr;
	regs->syscall_args[0] = arg0;
	regs->syscall_args[1] = arg1;
	regs->syscall_args[2] = arg2;
	regs->syscall_args[3] = arg3;
	regs->syscall_args[4] = arg4;
	regs->syscall_args[5] = arg5;

	/*
	 * Automatic syscall restart. A blocking syscall (e.g. init's read() on
	 * the console) that is interrupted by a signal returns -ERESTARTSYS.
	 * On a normal arch, signal delivery rewinds the userspace PC so the
	 * syscall instruction re-executes. wasm has no such PC to rewind, so
	 * instead arch_do_signal_or_restart() (run inside syscall_exit_to_user_
	 * mode via the pending-signal path) sets regs->syscall_restart, and we
	 * re-run the same syscall here. Without this, -ERESTARTSYS leaks to
	 * userspace: busybox's init shell reads it as a fatal console error and
	 * exit(0)s, killing pid 1 -> "Attempted to kill init" panic. This bit
	 * the guest reliably whenever an orphaned sshd child reparented to init
	 * and its SIGCHLD interrupted init's console read.
	 */
	do {
		regs->syscall_restart = 0;
		regs->syscall_ret = syscall_table[regs->syscall_nr](regs);
		syscall_exit_to_user_mode(regs);
	} while (regs->syscall_restart);

	regs->user_mode = 1;

	return regs->syscall_ret;
}

SYSCALL_DEFINE1(set_thread_area, unsigned long, addr)
{
	struct thread_info *ti = task_thread_info(current);
	ti->tp_value = addr;
	return 0;
}

__attribute__((export_name("get_thread_area"))) unsigned long
wasm_get_thread_area(void)
{
	struct thread_info *ti = task_thread_info(current);
	return ti->tp_value;
}
