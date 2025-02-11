/*
 * Copyright (c) 2016-2019 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <lego/time.h>
#include <lego/kernel.h>
#include <lego/jiffies.h>
#include <lego/clocksource.h>

/*
 * The Jiffies based clocksource is the lowest common
 * denominator clock source which should function on
 * all systems. It has the same coarse resolution as
 * the timer interrupt frequency HZ and it suffers
 * inaccuracies caused by missed or lost timer
 * interrupts and the inability for the timer
 * interrupt hardware to accuratly tick at the
 * requested HZ value. It is also not recommended
 * for "tick-less" systems.
 */
#define NSEC_PER_JIFFY	((NSEC_PER_SEC+HZ/2)/HZ)

/* Since jiffies uses a simple NSEC_PER_JIFFY multiplier
 * conversion, the .shift value could be zero. However
 * this would make NTP adjustments impossible as they are
 * in units of 1/2^.shift. Thus we use JIFFIES_SHIFT to
 * shift both the nominator and denominator the same
 * amount, and give ntp adjustments in units of 1/2^8
 *
 * The value 8 is somewhat carefully chosen, as anything
 * larger can result in overflows. NSEC_PER_JIFFY grows as
 * HZ shrinks, so values greater than 8 overflow 32bits when
 * HZ=100.
 */
#if HZ < 34
#define JIFFIES_SHIFT	6
#elif HZ < 67
#define JIFFIES_SHIFT	7
#else
#define JIFFIES_SHIFT	8
#endif

static cycle_t jiffies_read(struct clocksource *cs)
{
	return (cycle_t) jiffies;
}

static struct clocksource clocksource_jiffies = {
	.name		= "jiffies",

	/* lowest valid rating*/
	.rating		= 1,
	.read		= jiffies_read,
	.mask		= CLOCKSOURCE_MASK(32),
	.mult		= NSEC_PER_JIFFY << JIFFIES_SHIFT,
	.shift		= JIFFIES_SHIFT,
	.max_cycles	= 10,
};

struct clocksource * __init __weak clocksource_default_clock(void)
{
	return &clocksource_jiffies;
}

struct clocksource refined_jiffies;

int register_refined_jiffies(long cycles_per_second)
{
	u64 nsec_per_tick, shift_hz;
	long cycles_per_tick;

	refined_jiffies = clocksource_jiffies;
	refined_jiffies.name = "refined-jiffies";
	refined_jiffies.rating++;

	/* Calc cycles per tick */
	cycles_per_tick = (cycles_per_second + HZ/2)/HZ;

	/* shift_hz stores hz<<8 for extra accuracy */
	shift_hz = (u64)cycles_per_second << 8;
	shift_hz += cycles_per_tick/2;
	do_div(shift_hz, cycles_per_tick);

	/* Calculate nsec_per_tick using shift_hz */
	nsec_per_tick = (u64)NSEC_PER_SEC << 8;
	nsec_per_tick += (u32)shift_hz/2;
	do_div(nsec_per_tick, (u32)shift_hz);

	refined_jiffies.mult = ((u32)nsec_per_tick) << JIFFIES_SHIFT;

	__clocksource_register(&refined_jiffies);

	return 0;
}
