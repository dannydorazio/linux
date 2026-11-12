// SPDX-License-Identifier: GPL-2.0
/*
 * NeXT interrupt controller
 *
 * All interrupt sources are collected in one 32-bit status register with a
 * matching mask register.  Each source is wired to a fixed CPU interrupt
 * level and all of them are autovectored, so each level is demultiplexed
 * by reading the status register.
 *
 * On the non-Turbo machines several sources (the SCC, SCSI, Ethernet and
 * DSP among them) cannot be masked in the mask register.  Only sources that
 * have been enabled are dispatched; devices without a driver have to be
 * kept quiet at the device itself.
 */

#include <linux/bits.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/sched/debug.h>

#include <asm/irq.h>
#include <asm/irq_regs.h>
#include <asm/nexthw.h>

/* Sources on each CPU interrupt level, as status register bits */
static const u32 next_level_sources[8] = {
	[1] = 0x00000001,
	[2] = 0x00000002,
	[3] = 0x00003ffc,
	[4] = 0x00004000,
	[5] = 0x00038000,
	[6] = 0x3ffc0000,
	[7] = 0xc0000000,
};

/*
 * Sources the mask register can't turn off on non-Turbo machines: NMI, SCC,
 * DSP (levels 3 and 4), disk, SCSI and the Ethernet chip.
 */
#define NEXT_IRQ_UNMASKABLE	0x80027640

static u32 next_irq_enabled;

static void next_irq_write_mask(void)
{
	out_be32(NEXT_IO(NEXT_INTRMASK_OFFSET), next_irq_enabled);
}

static void next_irq_enable(struct irq_data *data)
{
	next_irq_enabled |= BIT(data->irq - NEXT_IRQ_BASE);
	next_irq_write_mask();
}

static void next_irq_disable(struct irq_data *data)
{
	next_irq_enabled &= ~BIT(data->irq - NEXT_IRQ_BASE);
	next_irq_write_mask();
}

static unsigned int next_irq_startup(struct irq_data *data)
{
	next_irq_enable(data);
	return 0;
}

static struct irq_chip next_irq_chip = {
	.name		= "NeXT",
	.irq_enable	= next_irq_enable,
	.irq_disable	= next_irq_disable,
	.irq_startup	= next_irq_startup,
	.irq_shutdown	= next_irq_disable,
};

static void next_irq_demux(struct irq_desc *desc)
{
	unsigned int level = irq_desc_get_irq(desc);
	u32 status = in_be32(NEXT_IO(NEXT_INTRSTAT_OFFSET));
	u32 pending = status & next_irq_enabled & next_level_sources[level];

	if (!pending) {
		/*
		 * Usually the source has been serviced already.  An interrupt
		 * that arrives before this one has called irq_enter() can run
		 * softirqs on its way out, with the interrupt mask lowered, and
		 * a second interrupt on this level then gets there first.  A
		 * source that can interrupt but has no handler, on the other
		 * hand, stays asserted.
		 */
		u32 live = next_irq_enabled |
			   (next_is_turbo() ? 0 : NEXT_IRQ_UNMASKABLE);

		if (status & live & next_level_sources[level]) {
			atomic_inc(&irq_err_count);
			pr_warn_ratelimited("NeXT: unhandled level %u interrupt, status %08x\n",
					    level, status);
		}
		return;
	}

	while (pending) {
		unsigned int bit = __fls(pending);

		pending &= ~BIT(bit);
		generic_handle_irq(NEXT_IRQ(bit));
	}
}

static irqreturn_t next_nmi_handler(int irq, void *dev_id)
{
	static int in_nmi;

	if (READ_ONCE(in_nmi))
		return IRQ_HANDLED;
	WRITE_ONCE(in_nmi, 1);

	pr_warn("Non-Maskable Interrupt\n");
	show_registers(get_irq_regs());

	/*
	 * The keyboard's NMI key combination raises it.  Clear it at the
	 * source: level 7 is edge triggered, so as long as it stays asserted
	 * no further NMI gets through.
	 */
	out_8(NEXT_IO(NEXT_KM_CTRL_OFFSET), NEXT_KM_NMI_RECEIVED);

	WRITE_ONCE(in_nmi, 0);
	return IRQ_HANDLED;
}

void __init next_init_IRQ(void)
{
	void __iomem *scc = NEXT_BMAP_IO(NEXT_SCC_OFFSET);
	unsigned int level;

	/* Start with every source masked; the ROM leaves some enabled. */
	next_irq_enabled = 0;
	next_irq_write_mask();

	/*
	 * The SCC interrupt cannot be masked above, so switch it off in the
	 * SCC until the serial driver takes over: clear MIE in WR9.
	 */
	out_8(scc + NEXT_SCC_CTRL_A, 9);
	out_8(scc + NEXT_SCC_CTRL_A, 0);

	m68k_setup_irq_controller(&next_irq_chip, handle_simple_irq,
				  NEXT_IRQ_BASE, NEXT_NUM_IRQS);

	for (level = 1; level <= 7; level++)
		irq_set_chained_handler(IRQ_AUTO_1 + level - 1, next_irq_demux);

	if (request_irq(NEXT_IRQ_NMI, next_nmi_handler, 0, "NMI",
			next_nmi_handler))
		pr_err("NeXT: couldn't register NMI handler\n");
}
