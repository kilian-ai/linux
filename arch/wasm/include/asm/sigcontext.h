#ifndef _WASM_SIGCONTEXT_H
#define _WASM_SIGCONTEXT_H

struct pt_regs {
	long syscall_nr;
	unsigned long syscall_args[6];
	int user_mode;
	long syscall_ret;	/* return value; lives in regs so signal delivery
				 * can rewrite it (-ERESTARTSYS -> -EINTR). */
	int syscall_restart;	/* set by arch_do_signal_or_restart to ask the
				 * trampoline to re-run the interrupted syscall
				 * (wasm has no PC to rewind for auto-restart). */
};

struct sigcontext {
	struct pt_regs regs;
};

#endif
