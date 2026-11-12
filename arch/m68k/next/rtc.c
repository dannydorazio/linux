// SPDX-License-Identifier: GPL-2.0
/*
 * NeXT real-time clock and power control
 *
 * The clock chip hangs off three bits of system control register 2 and is
 * driven bit by bit: an address byte (bit 7 set for writes) is shifted
 * out, then a data byte is shifted in or out, MSB first.  The older
 * machines have an MC68HC68T1 with BCD calendar registers, the newer ones
 * an MCS1850 with a 32-bit seconds counter.  Either chip also switches the
 * machine off.
 */

#include <linux/bcd.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/rtc.h>
#include <linux/spinlock.h>
#include <linux/time.h>

#include <asm/nexthw.h>

#define SCR2_RTDATA		0x00000400
#define SCR2_RTCLK		0x00000200
#define SCR2_RTCE		0x00000100

#define RTC_WRITE		0x80	/* in the address byte */

/* MC68HC68T1 calendar */
#define RTC_SEC			0x20
#define RTC_MIN			0x21
#define RTC_HRS			0x22
#define  RTC_HRS_12H		0x80
#define  RTC_HRS_PM		0x20
#define RTC_DAY			0x23
#define RTC_DATE		0x24
#define RTC_MON			0x25
#define RTC_YR			0x26
#define RTC_INTRCTL		0x32
#define  RTC_INTRCTL_PDOWN	0x40

/* MCS1850 seconds counter, most significant byte first */
#define RTC_CNTR0		0x20

/* both */
#define RTC_STATUS		0x30
#define  RTC_STATUS_NEW		0x80	/* this is an MCS1850 */
#define RTC_CONTROL		0x31
#define  RTC_CONTROL_START	0x80
#define  RTC_CONTROL_PDOWN	0x40	/* MCS1850 only */

static DEFINE_SPINLOCK(next_rtc_lock);
static bool next_rtc_new;

static u32 scr2_read(void)
{
	return in_be32(NEXT_IO(NEXT_SCR2_OFFSET));
}

static void scr2_write(u32 val)
{
	out_be32(NEXT_IO(NEXT_SCR2_OFFSET), val);
	udelay(1);
}

static void rtc_shift_out(u8 val)
{
	int i;

	for (i = 0; i < 8; i++, val <<= 1) {
		u32 scr2 = scr2_read() & ~(SCR2_RTDATA | SCR2_RTCLK);

		if (val & 0x80)
			scr2 |= SCR2_RTDATA;
		scr2_write(scr2);
		scr2_write(scr2 | SCR2_RTCLK);
		scr2_write(scr2);
	}
}

static u8 rtc_shift_in(void)
{
	u8 val = 0;
	int i;

	for (i = 0; i < 8; i++) {
		u32 scr2 = scr2_read() & ~(SCR2_RTDATA | SCR2_RTCLK);

		scr2_write(scr2 | SCR2_RTCLK);
		scr2_write(scr2);
		val = (val << 1) | !!(scr2_read() & SCR2_RTDATA);
	}
	return val;
}

static u8 next_rtc_read(u8 reg)
{
	unsigned long flags;
	u8 val;

	spin_lock_irqsave(&next_rtc_lock, flags);
	scr2_write((scr2_read() & ~(SCR2_RTDATA | SCR2_RTCLK)) | SCR2_RTCE);
	rtc_shift_out(reg);
	val = rtc_shift_in();
	scr2_write(scr2_read() & ~(SCR2_RTDATA | SCR2_RTCLK | SCR2_RTCE));
	spin_unlock_irqrestore(&next_rtc_lock, flags);

	return val;
}

static void next_rtc_write(u8 reg, u8 val)
{
	unsigned long flags;

	spin_lock_irqsave(&next_rtc_lock, flags);
	scr2_write((scr2_read() & ~(SCR2_RTDATA | SCR2_RTCLK)) | SCR2_RTCE);
	rtc_shift_out(reg | RTC_WRITE);
	rtc_shift_out(val);
	scr2_write(scr2_read() & ~(SCR2_RTDATA | SCR2_RTCLK | SCR2_RTCE));
	spin_unlock_irqrestore(&next_rtc_lock, flags);
}

static void next_rtc_stop(void)
{
	next_rtc_write(RTC_CONTROL,
		       next_rtc_read(RTC_CONTROL) & ~RTC_CONTROL_START);
}

static void next_rtc_start(void)
{
	next_rtc_write(RTC_CONTROL,
		       next_rtc_read(RTC_CONTROL) | RTC_CONTROL_START);
}

static void next_rtc_read_old(struct rtc_time *t)
{
	unsigned int hour, year;
	u8 h;

	t->tm_sec = bcd2bin(next_rtc_read(RTC_SEC) & 0x7f);
	t->tm_min = bcd2bin(next_rtc_read(RTC_MIN) & 0x7f);
	h = next_rtc_read(RTC_HRS);
	t->tm_wday = bcd2bin(next_rtc_read(RTC_DAY) & 0x07);
	t->tm_mday = bcd2bin(next_rtc_read(RTC_DATE) & 0x3f);
	t->tm_mon = bcd2bin(next_rtc_read(RTC_MON) & 0x1f) - 1;
	year = bcd2bin(next_rtc_read(RTC_YR));

	if (h & RTC_HRS_12H) {
		hour = bcd2bin(h & 0x1f) % 12;
		if (h & RTC_HRS_PM)
			hour += 12;
	} else {
		hour = bcd2bin(h & 0x3f);
	}
	t->tm_hour = hour;
	/* two digits: 69..99 are 1969..1999 */
	t->tm_year = year >= 69 ? year : year + 100;
}

static void next_rtc_write_old(const struct rtc_time *t)
{
	u8 h = next_rtc_read(RTC_HRS);

	next_rtc_stop();
	next_rtc_write(RTC_SEC, bin2bcd(t->tm_sec));
	next_rtc_write(RTC_MIN, bin2bcd(t->tm_min));
	if (h & RTC_HRS_12H) {
		unsigned int hour = t->tm_hour % 12;

		h = bin2bcd(hour ? hour : 12) | RTC_HRS_12H;
		if (t->tm_hour >= 12)
			h |= RTC_HRS_PM;
	} else {
		h = bin2bcd(t->tm_hour);
	}
	next_rtc_write(RTC_HRS, h);
	next_rtc_write(RTC_DAY, bin2bcd(t->tm_wday));
	next_rtc_write(RTC_DATE, bin2bcd(t->tm_mday));
	next_rtc_write(RTC_MON, bin2bcd(t->tm_mon + 1));
	next_rtc_write(RTC_YR, bin2bcd(t->tm_year % 100));
	next_rtc_start();
}

static u32 next_rtc_read_counter(void)
{
	u32 secs = 0;
	int i;

	for (i = 0; i < 4; i++)
		secs = (secs << 8) | next_rtc_read(RTC_CNTR0 + i);
	return secs;
}

static void next_rtc_read_new(struct rtc_time *t)
{
	struct tm tm;

	time64_to_tm(next_rtc_read_counter(), 0, &tm);
	t->tm_sec = tm.tm_sec;
	t->tm_min = tm.tm_min;
	t->tm_hour = tm.tm_hour;
	t->tm_mday = tm.tm_mday;
	t->tm_mon = tm.tm_mon;
	t->tm_year = tm.tm_year;
	t->tm_wday = tm.tm_wday;
	t->tm_yday = tm.tm_yday;
}

static void next_rtc_write_new(const struct rtc_time *t)
{
	u32 secs = mktime64(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
			    t->tm_hour, t->tm_min, t->tm_sec);
	int i;

	next_rtc_stop();
	for (i = 0; i < 4; i++)
		next_rtc_write(RTC_CNTR0 + i, secs >> (24 - 8 * i));
	next_rtc_start();
}

int next_hwclk(int op, struct rtc_time *t)
{
	if (op) {
		if (next_rtc_new)
			next_rtc_write_new(t);
		else
			next_rtc_write_old(t);
		return 0;
	}

	if (next_rtc_new) {
		next_rtc_read_new(t);
	} else {
		/* read again if the seconds rolled over in between */
		int sec;

		do {
			sec = bcd2bin(next_rtc_read(RTC_SEC) & 0x7f);
			next_rtc_read_old(t);
		} while (t->tm_sec != sec);
	}
	return 0;
}

void next_poweroff(void)
{
	/* the seconds, or the low byte of the seconds counter */
	u8 tick = next_rtc_new ? RTC_CNTR0 + 3 : RTC_SEC;
	u8 sec = next_rtc_read(tick);

	local_irq_disable();

	/*
	 * NetBSD waits for the clock to tick and then a further 850 ms
	 * before powering down, apparently to work around a hardware bug.
	 */
	while (next_rtc_read(tick) == sec)
		;
	mdelay(850);

	if (next_rtc_new)
		next_rtc_write(RTC_CONTROL,
			       next_rtc_read(RTC_CONTROL) | RTC_CONTROL_PDOWN);
	else
		next_rtc_write(RTC_INTRCTL,
			       next_rtc_read(RTC_INTRCTL) | RTC_INTRCTL_PDOWN);
}

const char *next_rtc_chip(void)
{
	return next_rtc_new ? "MCS1850" : "MC68HC68T1";
}

void __init next_rtc_init(void)
{
	next_rtc_new = next_rtc_read(RTC_STATUS) & RTC_STATUS_NEW;
	pr_info("NeXT: %s real-time clock\n", next_rtc_chip());
}
