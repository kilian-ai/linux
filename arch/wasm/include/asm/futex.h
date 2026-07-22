#ifndef _ASM_WASM_FUTEX_H
#define _ASM_WASM_FUTEX_H

#include <linux/futex.h>
#include <linux/uaccess.h>
#include <asm/errno.h>

/*
 * User memory lives in a separate wasm linear memory reachable only through
 * the JS uaccess imports, so a true atomic RMW on a user address is not
 * possible from kernel code. Emulate with plain uaccess. The races this
 * opens (vs userspace atomics on the same word) only affect FUTEX_WAKE_OP,
 * PI futexes and robust-list death handling — none of which musl-on-wasm
 * exercises; plain FUTEX_WAIT/WAKE/REQUEUE never call these helpers.
 */
static inline int arch_futex_atomic_op_inuser(int op, u32 oparg, int *oval,
					      u32 __user *uaddr)
{
	u32 oldval, newval;

	if (get_user(oldval, uaddr))
		return -EFAULT;

	switch (op) {
	case FUTEX_OP_SET:
		newval = oparg;
		break;
	case FUTEX_OP_ADD:
		newval = oldval + oparg;
		break;
	case FUTEX_OP_OR:
		newval = oldval | oparg;
		break;
	case FUTEX_OP_ANDN:
		newval = oldval & ~oparg;
		break;
	case FUTEX_OP_XOR:
		newval = oldval ^ oparg;
		break;
	default:
		return -ENOSYS;
	}

	if (put_user(newval, uaddr))
		return -EFAULT;

	*oval = oldval;
	return 0;
}

static inline int futex_atomic_cmpxchg_inatomic(u32 *uval, u32 __user *uaddr,
						u32 oldval, u32 newval)
{
	u32 val;

	if (get_user(val, uaddr))
		return -EFAULT;
	if (val == oldval && put_user(newval, uaddr))
		return -EFAULT;
	*uval = val;
	return 0;
}

#endif
