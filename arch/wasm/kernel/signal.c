#include <asm/wasm_imports.h>
#include <linux/syscalls.h>

/*
 * Signal delivery + syscall restart for wasm.
 *
 * A syscall that blocks and is interrupted by a signal returns one of the
 * -ERESTART* codes. These must never reach userspace: they are an internal
 * contract between the syscall and signal-delivery code. On a conventional
 * arch the return-to-user path either rewinds the PC (auto-restart) or
 * overwrites the return register with -EINTR. wasm cannot rewind userspace,
 * so the trampoline (wasm_syscall) re-runs the syscall when we set
 * regs->syscall_restart; here we decide restart vs -EINTR the same way the
 * generic signal code does, and rewrite regs->syscall_ret for the -EINTR case.
 */
void arch_do_signal_or_restart(struct pt_regs *regs)
{
	struct ksignal ksig;
	bool has_handler = get_signal(&ksig);
	long ret = regs->syscall_ret;
	bool restart = false;

	switch (ret) {
	case -ERESTARTNOHAND:
		/* restart only if no handler ran */
		if (has_handler)
			regs->syscall_ret = -EINTR;
		else
			restart = true;
		break;
	case -ERESTARTSYS:
		/* restart iff no handler, or handler opted in with SA_RESTART */
		if (has_handler && !(ksig.ka.sa.sa_flags & SA_RESTART))
			regs->syscall_ret = -EINTR;
		else
			restart = true;
		break;
	case -ERESTARTNOINTR:
		/* always restart */
		restart = true;
		break;
	case -ERESTART_RESTARTBLOCK:
		/* re-enter via sys_restart_syscall (uses the saved restart_block) */
		if (has_handler) {
			regs->syscall_ret = -EINTR;
		} else {
			regs->syscall_nr = __NR_restart_syscall;
			restart = true;
		}
		break;
	}

	regs->syscall_restart = restart;

	if (has_handler) {
		struct sigaction *sa = &ksig.ka.sa;
		if (sa->sa_flags & SA_SIGINFO)
			pr_warn("TODO: SA_SIGINFO in signal handler\n");

		/*
		 * The handler runs user code that may make NESTED syscalls
		 * (e.g. a SIGCHLD handler calling waitpid). Unlike a normal
		 * arch, where the handler runs in userspace with its own
		 * kernel-stack pt_regs per entry, wasm's current_pt_regs() is a
		 * SINGLE per-task struct that wasm_syscall reuses on every
		 * (re)entry. So a nested syscall clobbers the restart/return
		 * state we just computed for the INTERRUPTED syscall:
		 * zsh's SIGCHLD handler ends its reap loop with waitpid()
		 * returning -ECHILD, which overwrote regs->syscall_ret and
		 * cleared regs->syscall_restart -> the interrupted read()
		 * returned -ECHILD instead of -EINTR/restart, and zsh's
		 * command-substitution readoutput() (which only retries on
		 * EINTR) broke, losing the captured output. Save the
		 * interrupted syscall's regs across the handler and restore it.
		 */
		long saved_nr = regs->syscall_nr;
		long saved_ret = regs->syscall_ret;
		int saved_restart = regs->syscall_restart;
		unsigned long saved_args[6];
		int i;
		for (i = 0; i < 6; i++)
			saved_args[i] = regs->syscall_args[i];

		wasm_user_call_signal_handler((uintptr_t)sa->sa_handler, ksig.sig);

		regs->syscall_nr = saved_nr;
		regs->syscall_ret = saved_ret;
		regs->syscall_restart = saved_restart;
		for (i = 0; i < 6; i++)
			regs->syscall_args[i] = saved_args[i];
	}
}
