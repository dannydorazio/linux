// SPDX-License-Identifier: GPL-2.0
/*
 * NeXT system timer and event counter
 *
 * The system timer counts a 16-bit latch value down in microseconds,
 * interrupts on level 6 when it expires and reloads itself from the latch.
 * The event counter is a free-running 20-bit microsecond counter and serves
 * as the clocksource.
 */

#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>

#include <asm/nexthw.h>

#define TIMER_MSB		0
#define TIMER_LSB		1
#define TIMER_CSR		4
#define  TIMER_CSR_ENABLE	0x80
#define  TIMER_CSR_UPDATE	0x40	/* load the counter from the latch */
#define TIMER_MAX		0xffff

#define NEXT_TIMER_RATE		1000000	/* both count microseconds */
#define EVENTC_BITS		20

static void __iomem *timer;
static void __iomem *eventc;

static u64 next_eventc_read(struct clocksource *cs)
{
	return in_be32(eventc);
}

static struct clocksource next_eventc = {
	.name	= "eventc",
	.rating	= 250,
	.read	= next_eventc_read,
	.mask	= CLOCKSOURCE_MASK(EVENTC_BITS),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static void next_timer_start(unsigned int count)
{
	out_8(timer + TIMER_CSR, 0);
	out_8(timer + TIMER_MSB, count >> 8);
	out_8(timer + TIMER_LSB, count & 0xff);
	out_8(timer + TIMER_CSR, TIMER_CSR_ENABLE | TIMER_CSR_UPDATE);
}

static int next_timer_shutdown(struct clock_event_device *evt)
{
	out_8(timer + TIMER_CSR, 0);
	return 0;
}

static int next_timer_set_periodic(struct clock_event_device *evt)
{
	next_timer_start(DIV_ROUND_CLOSEST(NEXT_TIMER_RATE, HZ));
	return 0;
}

static int next_timer_set_next_event(unsigned long delta,
				     struct clock_event_device *evt)
{
	next_timer_start(delta);
	return 0;
}

static struct clock_event_device next_timer = {
	.name			= "next-timer",
	.features		= CLOCK_EVT_FEAT_PERIODIC |
				  CLOCK_EVT_FEAT_ONESHOT,
	.rating			= 200,
	.set_state_shutdown	= next_timer_shutdown,
	.set_state_periodic	= next_timer_set_periodic,
	.set_state_oneshot	= next_timer_shutdown,
	.set_next_event		= next_timer_set_next_event,
};

static irqreturn_t next_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	/*
	 * Any write to the control register acknowledges the interrupt.  In
	 * periodic mode the timer has already reloaded itself; a one-shot
	 * event stops it.
	 */
	if (clockevent_state_periodic(evt))
		out_8(timer + TIMER_CSR, TIMER_CSR_ENABLE);
	else
		out_8(timer + TIMER_CSR, 0);

	evt->event_handler(evt);
	return IRQ_HANDLED;
}

void __init next_sched_init(void)
{
	timer = NEXT_BMAP_IO(NEXT_TIMER_OFFSET);
	eventc = NEXT_BMAP_IO(NEXT_EVENTC_OFFSET);

	out_8(timer + TIMER_CSR, 0);
	if (request_irq(NEXT_IRQ_TIMER, next_timer_interrupt, IRQF_TIMER,
			"timer", &next_timer))
		pr_err("NeXT: couldn't register timer interrupt\n");

	clocksource_register_hz(&next_eventc, NEXT_TIMER_RATE);

	next_timer.cpumask = cpumask_of(0);
	clockevents_config_and_register(&next_timer, NEXT_TIMER_RATE,
					1, TIMER_MAX);
}
