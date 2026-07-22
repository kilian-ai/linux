#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/cpuhotplug.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <asm/irq.h>
#include <asm/wasm_imports.h>
#include <asm/param.h>
#include <asm/timex.h>
#include <asm/processor.h>

extern unsigned long loops_per_jiffy;
extern void wasm_set_timer_deadline(u64 deadline_ns);
extern u64 wasm_get_timer_deadline(void);

static int timer_irq;
void calibrate_delay(void)
{
	loops_per_jiffy = 1000000000 / HZ;
}

void __delay(unsigned long cycles)
{
	static int zero = 0;
	int ret = __builtin_wasm_memory_atomic_wait32(&zero, 0, cycles);
	BUG_ON(ret != 2); // 2 means timeout
}

void __udelay(unsigned long usecs)
{
	__delay(usecs * 1000);
}
void __ndelay(unsigned long nsecs)
{
	__delay(nsecs);
}
void __const_udelay(unsigned long xloops)
{
	__delay(xloops / 0x10c7ul); /* 2**32 / 1000000 (rounded up) */
}

unsigned long long sched_clock(void)
{
	static u64 origin = 0;
	if (!origin)
		origin = wasm_kernel_get_now_nsec();
	return wasm_kernel_get_now_nsec() - origin;
}

/*
 * The guest wall clock is deliberately NOT set from the kernel. Setting it
 * via do_settimeofday64() in an initcall empirically breaks inbound TCP:
 * injected connections stall in SYN-RECV (the guest retransmits SYN-ACK and
 * ignores the final ACK). A runtime settimeofday from userspace is fine, so
 * /etc/init.d/rcS sets the clock from the lot_epoch= kernel cmdline value
 * (written by wasm.html at boot). The JS syscall shim keeps
 * wasm_clock_origins[1] in sync when userspace sets the clock.
 */
extern u64 wasm_clock_origins[2];

static int __init wasm_publish_clock_origins(void)
{
	u64 raw = wasm_kernel_get_now_nsec();

	wasm_clock_origins[0] = raw - ktime_get_ns();
	wasm_clock_origins[1] = raw - ktime_get_real_ns();
	return 0;
}
/*
 * device_initcall, not late_initcall: this port's do_initcalls() only runs
 * levels 0-6 (init/main.c iterates ARRAY_SIZE(initcall_level_names) - 1
 * over exactly 8 levels), so level 7 "late" initcalls never execute.
 */
device_initcall(wasm_publish_clock_origins);

/*
 * Exported for the JS syscall shim: userspace clock_gettime is serviced in
 * JS (see worker.js nr=403), and absolute deadlines passed back into the
 * kernel (clock_nanosleep TIMER_ABSTIME, futex timed waits) are only
 * meaningful if userspace sees the kernel's own clocks. Safe to call from
 * any worker's vmlinux instance: ktime readers take the timekeeping seqlock.
 *
 * NOTE: only call these from a worker whose kernel instance is inside a
 * proper task context. pthread-clone child workers never enter the kernel
 * (switch_entry runs user code directly), so their instance's shadow stack
 * pointer still points at init_stack — running any non-leaf kernel C from
 * there scribbles over CPU 0's idle stack. For those contexts the JS shim
 * instead reads wasm_clock_origins[] below straight out of kernel memory.
 */
__attribute__((export_name("get_monotonic_ns"))) u64
wasm_get_monotonic_ns(void)
{
	return ktime_get_ns();
}

__attribute__((export_name("get_real_ns"))) u64
wasm_get_real_ns(void)
{
	return ktime_get_real_ns();
}

/*
 * Clock origins for stack-free clock recovery from JS:
 *   [0] = raw - CLOCK_MONOTONIC    [1] = raw - CLOCK_REALTIME
 * where raw = wasm_kernel_get_now_nsec() (the clocksource, 1:1 ns), so
 *   CLOCK_x(now) = raw(now) - origin[x]
 * holds for the whole boot (no NTP here to perturb the clocksource mult).
 * JS computes raw itself (performance.timeOrigin + performance.now() —
 * identical across workers) and only reads this array from kernel memory.
 * The address-getter below compiles to a bare i32.const: safe to call from
 * any instance, including ones with no usable shadow stack.
 */
u64 wasm_clock_origins[2];

__attribute__((export_name("get_clock_origins"))) u64 *
wasm_get_clock_origins(void)
{
	return wasm_clock_origins;
}

static u64 clock_read(struct clocksource *cs)
{
	return sched_clock();
}

static struct clocksource clocksource = {
	.name = "wasm",
	.rating = 499,
	.read = clock_read,
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
	.mask = CLOCKSOURCE_MASK(64),
};

static DEFINE_PER_CPU(struct clock_event_device, clockevent);

static irqreturn_t timer_interrupt(int irq, void *dev)
{
	struct clock_event_device *evt = this_cpu_ptr(&clockevent);

	if (evt->event_handler)
		evt->event_handler(evt);

	return IRQ_HANDLED;
}

static int timer_set_next_event(unsigned long delta,
				struct clock_event_device *evt)
{
	u64 now = wasm_kernel_get_now_nsec();
	u64 deadline = now + delta;
	wasm_set_timer_deadline(deadline);
	return 0;
}

static int timer_set_oneshot(struct clock_event_device *evt)
{
	return 0;
}

static int timer_shutdown(struct clock_event_device *evt)
{
	wasm_set_timer_deadline(0);
	return 0;
}

static int timer_starting_cpu(unsigned int cpu)
{
	struct clock_event_device *evt = this_cpu_ptr(&clockevent);

	evt->name = "wasm-timer";
	evt->features = CLOCK_EVT_FEAT_ONESHOT;
	evt->rating = 300;
	evt->set_next_event = timer_set_next_event;
	evt->set_state_oneshot = timer_set_oneshot;
	evt->set_state_shutdown = timer_shutdown;
	evt->cpumask = cpumask_of(cpu);
	evt->irq = timer_irq;

	clockevents_config_and_register(evt, NSEC_PER_SEC, 1000, LONG_MAX);

	return 0;
}

void __init time_init(void)
{
	int ret;

	if (clocksource_register_khz(&clocksource, 1000 * 1000))
		panic("unable to register clocksource\n");

	timer_irq = irq_create_mapping(NULL, TIMER_IRQ);
	if (!timer_irq)
		panic("unable to create IRQ mapping for timer\n");

	if (request_irq(timer_irq, timer_interrupt, IRQF_TIMER, "timer", NULL))
		panic("unable to request timer IRQ\n");

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "wasm/timer:online",
				timer_starting_cpu, NULL);
	if (ret < 0)
		panic("unable to setup CPU hotplug state\n");
}
