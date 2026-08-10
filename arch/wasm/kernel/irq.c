#include <asm/smp.h>
#include <asm/timex.h>
#include <linux/cpu.h>
#include <linux/bitops.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/processor.h>

static DEFINE_PER_CPU(unsigned long, irqflags);
static DEFINE_PER_CPU(atomic64_t, irq_pending);
static DEFINE_PER_CPU(u64, timer_deadline_ns);

/* ── wedge-diagnosis telemetry ────────────────────────────────────────────
 * Lock-free counters + event ring the page (JS) can read from the shared
 * memory while the guest is wedged. Writers run both in kernel context
 * (arch_cpu_idle, __switch_to via wasm_kdiag_switch) and on the browser
 * main thread's vmlinux instance (trigger_irq_for_cpu), which has NO usable
 * shadow stack — so every helper here must stay leaf: atomic builtins and
 * address arithmetic only, no spilled locals, no calls except wasm imports.
 * JS side: addr = instance.exports.get_kdiag(); read u64s with Atomics on a
 * BigUint64Array view of the shared memory.
 */
#define KDIAG_RING_SZ 128 /* power of two; 4 u64s per event */
#define KDIAG_HDR 16

enum {
	KD_IDLE_SEQ = 0,   /* idle-wait entries */
	KD_IDLE_STATE,     /* 0=not idle-waiting, 1=parked in wait64, 2=woke */
	KD_IDLE_DEADLINE,  /* deadline at entry */
	KD_IDLE_TIMEOUT,   /* computed timeout (~0 = infinite) */
	KD_IDLE_NOW,       /* raw ns at entry */
	KD_IDLE_PENDING,   /* irq_pending at entry */
	KD_TRIG_COUNT,     /* trigger_irq_for_cpu calls */
	KD_TRIG_WOKEN,     /* cumulative waiters woken by trigger's notify */
	KD_SW_COUNT,       /* __switch_to count */
	KD_SW_FROM,        /* last switch: from pid */
	KD_SW_TO,          /* last switch: to pid */
	KD_SETDL_COUNT,    /* wasm_set_timer_deadline calls */
	KD_SETDL_LAST,     /* last programmed deadline (0 = shutdown) */
	KD_RELAX_COUNT,    /* cpu_relax calls */
	KD_RING_IDX,       /* monotonic next-slot counter */
};

/* ring event types (slot 0), payload a/b/c in slots 1..3:
 * 1 idle-enter   a=deadline b=timeout          c=now
 * 2 idle-wake    a=wait ret b=pending after    c=now
 * 3 trigger-irq  a=irq|(woken<<8) b=pending after c=now
 * 4 set-deadline a=deadline b=now              c=0
 * 5 switch       a=from pid b=to pid           c=now
 * 6 idle-early   a=deadline b=now              c=0 (deadline already past)
 */
static u64 kdiag[KDIAG_HDR + KDIAG_RING_SZ * 4];

__attribute__((export_name("get_kdiag"))) u64 *wasm_get_kdiag(void)
{
	return kdiag;
}

static __always_inline void kd_set(int i, u64 v)
{
	__atomic_store_n(&kdiag[i], v, __ATOMIC_SEQ_CST);
}

static __always_inline u64 kd_inc(int i, u64 v)
{
	return __atomic_fetch_add(&kdiag[i], v, __ATOMIC_SEQ_CST);
}

static __always_inline void kd_ev(u64 type, u64 a, u64 b, u64 c)
{
	u64 *e = &kdiag[KDIAG_HDR +
			(kd_inc(KD_RING_IDX, 1) % KDIAG_RING_SZ) * 4];
	__atomic_store_n(&e[1], a, __ATOMIC_SEQ_CST);
	__atomic_store_n(&e[2], b, __ATOMIC_SEQ_CST);
	__atomic_store_n(&e[3], c, __ATOMIC_SEQ_CST);
	__atomic_store_n(&e[0], type, __ATOMIC_SEQ_CST); /* type last = valid */
}

void wasm_kdiag_switch(int from_pid, int to_pid)
{
	kd_inc(KD_SW_COUNT, 1);
	kd_set(KD_SW_FROM, from_pid);
	kd_set(KD_SW_TO, to_pid);
	kd_ev(5, from_pid, to_pid, wasm_kernel_get_now_nsec());
}

/* Bare address getters (leaf) so JS can inspect/kick the raw words. */
__attribute__((export_name("get_irq_pending_ptr"))) void *
wasm_get_irq_pending_ptr(unsigned int cpu)
{
	return per_cpu_ptr(&irq_pending, cpu);
}

__attribute__((export_name("get_timer_deadline_ptr"))) void *
wasm_get_timer_deadline_ptr(unsigned int cpu)
{
	return per_cpu_ptr(&timer_deadline_ns, cpu);
}

void wasm_set_timer_deadline(u64 deadline_ns)
{
	__this_cpu_write(timer_deadline_ns, deadline_ns);
	kd_inc(KD_SETDL_COUNT, 1);
	kd_set(KD_SETDL_LAST, deadline_ns);
	kd_ev(4, deadline_ns, wasm_kernel_get_now_nsec(), 0);
}

u64 wasm_get_timer_deadline(void)
{
	return __this_cpu_read(timer_deadline_ns);
}

extern int wasm_sched_trace;
void wasm_ktrace(const char *s, int n);

void __cpuidle arch_cpu_idle(void)
{
	atomic64_t *pending = this_cpu_ptr(&irq_pending);
	u64 deadline = __this_cpu_read(timer_deadline_ns);
	u64 now = wasm_kernel_get_now_nsec();
	s64 timeout_ns;
	int ret;

	if (wasm_sched_trace) {
		char b[80];
		int n = snprintf(b, sizeof(b), "@K@IDLE cpu=%d\n",
				 raw_smp_processor_id());
		if (n > 0)
			wasm_ktrace(b, n);
	}

	if (deadline == 0) {
		timeout_ns = -1; // forever
	} else {
		if ((s64)(deadline - now) <= 0) {
			__this_cpu_write(timer_deadline_ns, 0);
			atomic64_or(1 << TIMER_IRQ, pending);
			kd_ev(6, deadline, now, 0);
			raw_local_irq_enable();
			return;
		}
		timeout_ns = deadline - now;
	}

	kd_inc(KD_IDLE_SEQ, 1);
	kd_set(KD_IDLE_DEADLINE, deadline);
	kd_set(KD_IDLE_TIMEOUT, (u64)timeout_ns);
	kd_set(KD_IDLE_NOW, now);
	kd_set(KD_IDLE_PENDING, (u64)atomic64_read(pending));
	kd_set(KD_IDLE_STATE, 1);
	kd_ev(1, deadline, (u64)timeout_ns, now);

	ret = __builtin_wasm_memory_atomic_wait64(&pending->counter, 0,
						  timeout_ns);

	kd_set(KD_IDLE_STATE, 2);
	kd_ev(2, (u64)ret, (u64)atomic64_read(pending),
	      wasm_kernel_get_now_nsec());

	if (ret == 2 /* timeout reached */) {
		__this_cpu_write(timer_deadline_ns, 0);
		atomic64_or(1 << TIMER_IRQ, pending);
	}

	raw_local_irq_enable();
	kd_set(KD_IDLE_STATE, 0);
}

void cpu_relax(void)
{
	unsigned long flags;
	atomic64_t *pending;
	kd_inc(KD_RELAX_COUNT, 1);
	local_irq_save(flags);
	pending = this_cpu_ptr(&irq_pending);
	__builtin_wasm_memory_atomic_wait64(&pending->counter, 0,
					    10 * 1000 * 1000);
	local_irq_restore(flags);
}

static void run_irq(irq_hw_number_t hwirq)
{
	static struct pt_regs dummy;
	unsigned long flags;
	struct pt_regs *old_regs = set_irq_regs((struct pt_regs *)&dummy);

	/* interrupt handlers need to run with interrupts disabled */
	local_irq_save(flags);
	irq_enter();
	generic_handle_domain_irq(NULL, hwirq);
	irq_exit();
	set_irq_regs(old_regs);
	local_irq_restore(flags);
}

unsigned long arch_local_save_flags(void)
{
	return __this_cpu_read(irqflags);
}

static void run_irqs(void)
{
	int irq;
	u64 pending = atomic64_xchg(this_cpu_ptr(&irq_pending), 0);

	for_each_irq_nr(irq)
		if (pending & (1 << irq))
			run_irq(irq);
}

/* NOTE: runs on the browser MAIN THREAD's vmlinux instance (no usable
 * shadow stack) — keep it leaf: atomics, builtins, wasm imports only. */
__attribute__((export_name("trigger_irq_for_cpu"))) void
trigger_irq_for_cpu(unsigned int cpu, irq_hw_number_t irq)
{
	atomic64_t *pending = per_cpu_ptr(&irq_pending, cpu);
	u64 newval;
	u32 woken;

	newval = (u64)atomic64_fetch_or(1 << irq, pending) | (1ULL << irq);

	woken = __builtin_wasm_memory_atomic_notify((void *)&pending->counter,
						    /* at most, wake up: */ 1);

	kd_inc(KD_TRIG_COUNT, 1);
	kd_inc(KD_TRIG_WOKEN, woken);
	kd_ev(3, (u64)irq | ((u64)woken << 8), newval,
	      wasm_kernel_get_now_nsec());
}

void arch_local_irq_restore(unsigned long flags)
{
	if (flags == ARCH_IRQ_ENABLED && !in_interrupt())
		run_irqs();
	__this_cpu_write(irqflags, flags);
}

static int wasm_irq_map(struct irq_domain *d, unsigned int irq,
			irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_percpu_irq);

	return 0;
}

static const struct irq_domain_ops wasm_irq_ops = {
	.xlate = irq_domain_xlate_onecell,
	.map = wasm_irq_map,
};

void __init init_IRQ(void)
{
	struct irq_domain *root_domain;

	root_domain = irq_domain_add_linear(NULL, NR_IRQS, &wasm_irq_ops, NULL);
	if (!root_domain)
		panic("root irq domain not available\n");

	irq_set_default_host(root_domain);

#ifdef CONFIG_SMP
	irq_create_mapping(root_domain, IPI_IRQ);
	setup_smp_ipi();
#endif

	pr_info("IRQs enabled\n");
}

static DECLARE_BITMAP(irqalloc, NR_IRQS);
// TODO: wrap request_irq and free_irq instead expecting the caller to call these then them
int wasm_alloc_irq(void)
{
	for (int i = FIRST_EXT_IRQ; i < NR_IRQS; i++) {
		if (!test_and_set_bit(i, irqalloc))
			return irq_create_mapping(NULL, i);
	}
	return -ENOSPC;
}
void wasm_free_irq(int irq)
{
	WARN(!test_and_clear_bit(irq, irqalloc),
	     "irq %d not allocated. double free?\n", irq);
}
