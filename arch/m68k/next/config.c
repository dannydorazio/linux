// SPDX-License-Identifier: GPL-2.0
/*
 * NeXT machine setup
 */

#include <linux/export.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/memblock.h>
#include <linux/platform_data/nextdimension.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/seq_file.h>
#include <linux/string.h>

#include <asm/bootinfo.h>
#include <asm/bootinfo-next.h>
#include <asm/byteorder.h>
#include <asm/config.h>
#include <asm/hwtest.h>
#include <asm/machdep.h>
#include <asm/nexthw.h>
#include <asm/setup.h>

struct next_booter_data next_bi_data = {
	.model = ~0U,
};

static const char *next_model_name(u32 model)
{
	switch (model) {
	case NEXT_MODEL_CUBE:
		return "Computer";
	case NEXT_MODEL_WARP9:
		return "station";
	case NEXT_MODEL_X15:
		return "cube";
	case NEXT_MODEL_WARP9C:
		return "station Color";
	case NEXT_MODEL_TURBO_MONO:
		return "station Turbo";
	case NEXT_MODEL_TURBO_COLOR:
		return "station Turbo Color";
	case NEXT_MODEL_CUBE_TURBO:
		return "cube Turbo";
	default:
		return NULL;
	}
}

static void next_get_model(char *str)
{
	const char *name = next_model_name(next_bi_data.model);

	/* str is 80 characters long */
	if (name)
		sprintf(str, "NeXT%s", name);
	else
		sprintf(str, "NeXT (machine type %u)", next_bi_data.model);
}

static bool next_is_color(void)
{
	return next_bi_data.model == NEXT_MODEL_WARP9C ||
	       next_bi_data.model == NEXT_MODEL_TURBO_COLOR;
}

/*
 * NeXTBus boards, in a cube's slots 2, 4 and 6.  The NBIC of a board shows
 * its ID in the last bytes of the slot's 16 MB of slot space, one byte to a
 * word; an empty slot bus errors.  A board's own 256 MB of board space
 * starts at the slot number times 256 MB.
 */
#define NEXT_SLOT_PADDR(slot)		(0xf0000000 | (slot) << 24)
#define NEXT_BOARD_PADDR(slot)		((resource_size_t)(slot) << 28)
#define NEXT_NBIC_ID_OFFSET		0x00fffff0
#define NEXT_NBIC_ID_DIMENSION		0xc0000001

/* NeXTdimension: where its parts are, in its board and slot space */
#define NEXT_ND_VRAM_OFFSET		0x0e000000
#define NEXT_ND_VRAM_SIZE		0x00400000
#define NEXT_ND_MC_OFFSET		0x00800000
#define NEXT_ND_MC_SIZE			0x00004000
#define NEXT_ND_DAC_OFFSET		0x00200000
#define NEXT_ND_DAC_SIZE		0x00000010
#define NEXT_ND_DRAM_OFFSET		0x08000000
#define NEXT_ND_DRAM_STEP		0x00400000	/* the least a bank holds */
#define NEXT_ND_DRAM_PROBE		0x00100000	/* clear of the driver's */

/* What's in slots 2, 4 and 6, for /proc/hardware */
static struct {
	u32 id;
	/* a NeXTdimension's */
	unsigned int dram_mb;	/* sized at boot */
	u32 epsr;		/* from its driver, once the i860 runs */
} next_slots[3];

/*
 * The NeXTdimension driver passes on the i860's extended processor status
 * register, which its program reads: the processor type in bits 7-0 (1 is
 * the i860XR, 2 the i860XP) and the stepping in bits 12-8.
 */
#define NEXT_EPSR_TYPE(epsr)		((epsr) & 0xff)
#define NEXT_EPSR_STEPPING(epsr)	(((epsr) >> 8) & 0x1f)

void next_nd_set_epsr(unsigned int slot, u32 epsr)
{
	if (slot >= 2 && slot <= 6 && !(slot & 1))
		WRITE_ONCE(next_slots[slot / 2 - 1].epsr, epsr);
}
EXPORT_SYMBOL_GPL(next_nd_set_epsr);

static void next_nd_show(struct seq_file *m, unsigned int slot)
{
	u32 epsr = READ_ONCE(next_slots[slot / 2 - 1].epsr);

	seq_printf(m, "Slot %u:\tNeXTdimension: i860", slot);
	switch (NEXT_EPSR_TYPE(epsr)) {
	case 0:		/* the driver hasn't run the i860 */
		break;
	case 1:
		seq_printf(m, "XR stepping %u", NEXT_EPSR_STEPPING(epsr));
		break;
	case 2:
		seq_printf(m, "XP stepping %u", NEXT_EPSR_STEPPING(epsr));
		break;
	default:
		seq_printf(m, " type %u stepping %u", NEXT_EPSR_TYPE(epsr),
			   NEXT_EPSR_STEPPING(epsr));
	}
	/*
	 * The clock, the video memory and the display are the same on every
	 * board: nominal, not detected.
	 */
	seq_printf(m, " at 33 MHz, %u MB memory, %u MB video memory, 1120x832 32-bit color display\n",
		   next_slots[slot / 2 - 1].dram_mb, NEXT_ND_VRAM_SIZE >> 20);
}

/* The rest of /proc/hardware, after the model and memory size */
static void next_get_hardware_list(struct seq_file *m)
{
	int i;

	seq_printf(m, "Board Revision:\t%u\n", next_bi_data.board_rev);
	/* the nominal clock: /proc/cpuinfo has a measured one */
	seq_printf(m, "CPU Clock:\t%u MHz\n", next_is_turbo() ? 33 : 25);
	for (i = 0; i < m68k_num_memory; i++)
		seq_printf(m, "\t%3lu MB at 0x%08lx\n",
			   m68k_memory[i].size >> 20, m68k_memory[i].addr);
	seq_printf(m, "Display:\t1120x832, %s\n",
		   next_is_color() ? "12-bit color" : "2-bit grayscale");
	for (i = 0; i < ARRAY_SIZE(next_slots); i++) {
		if (next_slots[i].id == NEXT_NBIC_ID_DIMENSION)
			next_nd_show(m, 2 * i + 2);
		else if (next_slots[i].id)
			seq_printf(m, "Slot %d:\tboard %08x\n", 2 * i + 2,
				   next_slots[i].id);
	}
	seq_printf(m, "Ethernet:\t%pM\n", next_bi_data.ether_addr);

	seq_puts(m, "On board:\n");
	seq_printf(m, "\tSCSI controller %s\n",
		   CPU_IS_030 ? "NCR53C90" : "NCR53C90A");
	seq_printf(m, "\tEthernet controller %s\n",
		   next_is_turbo() ? "AT&T 7213" : "MB8795");
	seq_puts(m, "\tSerial communications controller Z85C30\n");
	seq_puts(m, "\tDSP56001 digital signal processor\n");
	seq_printf(m, "\tClock chip %s\n", next_rtc_chip());
}

/*
 * Parse a NeXT-specific record in the bootinfo
 */

int __init next_parse_bootinfo(const struct bi_record *record)
{
	int unknown = 0;
	const void *data = record->data;

	switch (be16_to_cpu(record->tag)) {
	case BI_NEXT_MODEL:
		next_bi_data.model = be32_to_cpup(data);
		break;
	case BI_NEXT_BOARD_REV:
		next_bi_data.board_rev = be32_to_cpup(data);
		break;
	case BI_NEXT_ETHER_ADDR:
		memcpy(next_bi_data.ether_addr, data,
		       sizeof(next_bi_data.ether_addr));
		break;
	case BI_NEXT_ROM_MG:
		next_bi_data.rom_mg = be32_to_cpup(data);
		break;
	case BI_NEXT_ROM_VBR:
		next_bi_data.rom_vbr = be32_to_cpup(data);
		break;
	default:
		unknown = 1;
		break;
	}
	return unknown;
}

/*
 * The two Z85C30 serial ports, for the pmac_zilog driver, which expects
 * these names.  Each port has its control and its data register as memory
 * resources; both share the SCC interrupt.
 */
static struct resource scc_a_rsrcs[] = {
	DEFINE_RES_MEM(0, 1),		/* control */
	DEFINE_RES_MEM(0, 1),		/* data */
	DEFINE_RES_IRQ(NEXT_IRQ_SCC),
};

static struct resource scc_b_rsrcs[] = {
	DEFINE_RES_MEM(0, 1),
	DEFINE_RES_MEM(0, 1),
	DEFINE_RES_IRQ(NEXT_IRQ_SCC),
};

#ifdef CONFIG_MAC
/* A kernel that also supports Macs gets these from mac/config.c. */
extern struct platform_device scc_a_pdev, scc_b_pdev;
#else
struct platform_device scc_a_pdev = {
	.name	= "scc",
	.id	= 0,
};
EXPORT_SYMBOL(scc_a_pdev);

struct platform_device scc_b_pdev = {
	.name	= "scc",
	.id	= 1,
};
EXPORT_SYMBOL(scc_b_pdev);
#endif

/*
 * The ROM boots over the network or from disk and may leave the ethernet
 * receiver and DMA channels running; a frame arriving later would then be
 * written into memory the kernel owns.  Stop them before anything else.
 */
static void __init next_quiesce_devices(void)
{
	static const unsigned int dma_csrs[] __initconst = {
		NEXT_DMA_SCSI_CSR, NEXT_DMA_SOUNDOUT_CSR, NEXT_DMA_SOUNDIN_CSR,
		NEXT_DMA_PRINTER_CSR, NEXT_DMA_DSP_CSR, NEXT_DMA_ENETX_CSR,
		NEXT_DMA_ENETR_CSR,
		/* not on Turbo machines, where they bus error */
		NEXT_DMA_DISK_CSR, NEXT_DMA_SCC_CSR, NEXT_DMA_R2M_CSR,
		NEXT_DMA_M2R_CSR,
	};
	unsigned int i, n = next_is_turbo() ? 7 : ARRAY_SIZE(dma_csrs);

	out_8(NEXT_BMAP_IO(NEXT_ENET_OFFSET) + NEXT_ENET_RESET,
	      NEXT_ENET_RESET_MODE);

	for (i = 0; i < n; i++) {
		out_be32(NEXT_IO(dma_csrs[i]),
			 NEXT_DMACSR_RESET | NEXT_DMACSR_CLRCOMPLETE);
		out_be32(NEXT_IO(dma_csrs[i]), 0);
	}
}

static void __init next_scc_setup(void)
{
	resource_size_t base = NEXT_IO_PADDR + NEXT_SCC_OFFSET;

	if (CPU_IS_040_OR_060)
		base += NEXT_BMAP_OFFSET;

	scc_a_rsrcs[0].start = scc_a_rsrcs[0].end = base + NEXT_SCC_CTRL_A;
	scc_a_rsrcs[1].start = scc_a_rsrcs[1].end = base + NEXT_SCC_DATA_A;
	scc_a_pdev.resource = scc_a_rsrcs;
	scc_a_pdev.num_resources = ARRAY_SIZE(scc_a_rsrcs);

	scc_b_rsrcs[0].start = scc_b_rsrcs[0].end = base + NEXT_SCC_CTRL_B;
	scc_b_rsrcs[1].start = scc_b_rsrcs[1].end = base + NEXT_SCC_DATA_B;
	scc_b_pdev.resource = scc_b_rsrcs;
	scc_b_pdev.num_resources = ARRAY_SIZE(scc_b_rsrcs);
}

/* SCSI controller registers, followed by its DMA interface control */
static struct resource next_esp_rsrcs[] = {
	DEFINE_RES_MEM(0, 0x22),
	DEFINE_RES_IRQ(NEXT_IRQ_SCSI),
};

/* Ethernet controller; the DMA channels interrupt for it */
static struct resource next_enet_rsrcs[] = {
	DEFINE_RES_MEM(0, NEXT_ENET_SIZE),
	DEFINE_RES_IRQ(NEXT_IRQ_ENETX_DMA),
	DEFINE_RES_IRQ(NEXT_IRQ_ENETR_DMA),
};

/* The keyboard and mouse share their interface with sound. */
static struct resource next_kbd_rsrcs[] = {
	DEFINE_RES_MEM(NEXT_IO_PADDR + NEXT_KMS_OFFSET, NEXT_KMS_SIZE),
	DEFINE_RES_IRQ(NEXT_IRQ_KYBD_MOUSE),
};

static bool next_has_slots(void)
{
	return next_bi_data.model == NEXT_MODEL_CUBE ||
	       next_bi_data.model == NEXT_MODEL_X15 ||
	       next_bi_data.model == NEXT_MODEL_CUBE_TURBO;
}

static u32 __init next_nbic_id(unsigned int slot)
{
	void __iomem *nbic;
	u32 id = 0;
	int i;

	nbic = ioremap(NEXT_SLOT_PADDR(slot) + NEXT_NBIC_ID_OFFSET, 16);
	if (!nbic)
		return 0;
	if (hwreg_present(nbic))
		for (i = 0; i < 4; i++)
			id = id << 8 | readb(nbic + 4 * i);
	iounmap(nbic);
	return id;
}

/*
 * A NeXTdimension bank of DRAM holds 4 or 16 MB, or nothing, in 16 MB of
 * the board's space; a smaller bank repeats through it.  Write a mark at
 * each 4 MB, the first one last, and count those that are still there.
 * The board's ROM may be using the memory, so put it back afterwards.
 */
static unsigned int __init next_nd_bank_mb(resource_size_t bank)
{
	enum { STEPS = NEXTDIM_DRAM_BANK_SIZE / NEXT_ND_DRAM_STEP };
	void __iomem *mark[STEPS] = {};
	u16 saved[STEPS];
	unsigned int i, n = 0;

	for (i = 0; i < STEPS; i++) {
		mark[i] = ioremap(bank + i * NEXT_ND_DRAM_STEP +
				  NEXT_ND_DRAM_PROBE, 2);
		if (!mark[i] || !hwreg_present(mark[i]))
			goto out;
		saved[i] = __raw_readw(mark[i]);
	}
	for (i = STEPS; i-- > 0; )
		__raw_writew(0x4e00 | i, mark[i]);
	for (i = 0; i < STEPS; i++)
		if (__raw_readw(mark[i]) == (0x4e00 | i))
			n++;
	for (i = STEPS; i-- > 0; )
		__raw_writew(saved[i], mark[i]);
out:
	for (i = 0; i < STEPS; i++)
		if (mark[i])
			iounmap(mark[i]);
	return n * (NEXT_ND_DRAM_STEP >> 20);
}

static void __init next_slots_init(void)
{
	unsigned int slot;

	if (!next_has_slots())
		return;

	for (slot = 2; slot <= 6; slot += 2) {
		u32 id = next_nbic_id(slot);

		next_slots[slot / 2 - 1].id = id;
		if (id == NEXT_NBIC_ID_DIMENSION) {
			struct resource rsrcs[] = {
				DEFINE_RES_MEM(NEXT_BOARD_PADDR(slot) +
					       NEXT_ND_VRAM_OFFSET,
					       NEXT_ND_VRAM_SIZE),
				DEFINE_RES_MEM(NEXT_SLOT_PADDR(slot) +
					       NEXT_ND_MC_OFFSET,
					       NEXT_ND_MC_SIZE),
				DEFINE_RES_MEM(NEXT_SLOT_PADDR(slot) +
					       NEXT_ND_DAC_OFFSET,
					       NEXT_ND_DAC_SIZE),
				DEFINE_RES_MEM(NEXT_BOARD_PADDR(slot) +
					       NEXT_ND_DRAM_OFFSET,
					       NEXTDIM_DRAM_BANKS *
					       NEXTDIM_DRAM_BANK_SIZE),
			};
			struct nextdim_platform_data pdata;
			unsigned int bank, mb = 0;

			for (bank = 0; bank < NEXTDIM_DRAM_BANKS; bank++) {
				pdata.bank_mb[bank] = next_nd_bank_mb(
					NEXT_BOARD_PADDR(slot) +
					NEXT_ND_DRAM_OFFSET +
					bank * NEXTDIM_DRAM_BANK_SIZE);
				mb += pdata.bank_mb[bank];
			}
			next_slots[slot / 2 - 1].dram_mb = mb;
			pr_info("NeXT: NeXTdimension in slot %u, %u MB\n", slot,
				mb);
			platform_device_register_resndata(NULL,
							  "next-dimension",
							  slot, rsrcs,
							  ARRAY_SIZE(rsrcs),
							  &pdata,
							  sizeof(pdata));
		} else if (id) {
			pr_info("NeXT: board %08x in slot %u\n", id, slot);
		}
	}
}

static int __init next_platform_init(void)
{
	resource_size_t bmap = CPU_IS_040_OR_060 ? NEXT_BMAP_OFFSET : 0;

	if (!MACH_IS_NEXT)
		return -ENODEV;

	platform_device_register(&scc_a_pdev);
	platform_device_register(&scc_b_pdev);

	next_esp_rsrcs[0] = DEFINE_RES_MEM(NEXT_IO_PADDR + bmap +
					   NEXT_SCSI_OFFSET, 0x22);
	platform_device_register_simple("next_esp", -1, next_esp_rsrcs,
					ARRAY_SIZE(next_esp_rsrcs));

	next_enet_rsrcs[0] = DEFINE_RES_MEM(NEXT_IO_PADDR + bmap +
					    NEXT_ENET_OFFSET, NEXT_ENET_SIZE);
	platform_device_register_resndata(NULL, "mb8795", -1, next_enet_rsrcs,
					  ARRAY_SIZE(next_enet_rsrcs),
					  next_bi_data.ether_addr,
					  sizeof(next_bi_data.ether_addr));

	platform_device_register_simple("next-kbd", -1, next_kbd_rsrcs,
					ARRAY_SIZE(next_kbd_rsrcs));

	/*
	 * The display driver works out the line length from the size of the
	 * video memory.  On non-Turbo Color machines, it also gets the video
	 * control register.
	 */
	if (next_is_color()) {
		struct resource rsrcs[] = {
			next_is_turbo() ?
			DEFINE_RES_MEM(NEXT_TURBO_VRAM_PADDR,
				       NEXT_TURBO_COLOR_LINE_BYTES *
				       NEXT_DISPLAY_LINES) :
			DEFINE_RES_MEM(NEXT_COLOR_VRAM_PADDR,
				       NEXT_COLOR_LINE_BYTES * NEXT_DISPLAY_LINES),
			DEFINE_RES_MEM(NEXT_IO_PADDR + bmap + NEXT_C16_CSR_OFFSET,
				       1),
		};

		platform_device_register_simple("next-color-display", -1, rsrcs,
						next_is_turbo() ? 1 : 2);
	} else {
		struct resource vram = next_is_turbo() ?
			DEFINE_RES_MEM(NEXT_TURBO_VRAM_PADDR,
				       NEXT_TURBO_MONO_LINE_BYTES *
				       NEXT_DISPLAY_LINES) :
			DEFINE_RES_MEM(NEXT_MONO_VRAM_PADDR,
				       NEXT_MONO_LINE_BYTES * NEXT_DISPLAY_LINES);

		platform_device_register_simple("next-display", -1, &vram, 1);
	}

	next_slots_init();

	return 0;
}
arch_initcall(next_platform_init);

/*
 * Restart by running the ROM from its reset vector, as after power-on.
 * The MMU has to go off first, so map the RAM the kernel runs in 1:1 with
 * a transparent translation register and continue at the physical address
 * of this code.
 */
static void next_reset(void)
{
	unsigned long phys = virt_to_phys(next_reset);
	unsigned long offset = phys - (unsigned long)next_reset;
	unsigned long rom = NEXT_ROM_PADDR;

	local_irq_disable();

	if (CPU_IS_040_OR_060) {
		/* 16 MB at phys, any function code, cache inhibited */
		unsigned long ttr = (phys & 0xff000000) | 0xc040;

		asm volatile(".chip 68040\n\t"
			     "movec %0,%%itt0\n\t"
			     "movec %0,%%dtt0\n\t"
			     "lea %%pc@(1f),%%a0\n\t"
			     "addl %1,%%a0\n\t"
			     "jmp %%a0@\n"
			     "1:\tmoveq #0,%%d0\n\t"
			     "nop\n\t"
			     "cpusha %%bc\n\t"
			     "nop\n\t"
			     "movec %%d0,%%cacr\n\t"
			     "movec %%d0,%%tc\n\t"
			     "pflusha\n\t"
			     "movec %%d0,%%itt0\n\t"
			     "movec %%d0,%%itt1\n\t"
			     "movec %%d0,%%dtt0\n\t"
			     "movec %%d0,%%dtt1\n\t"
			     "movec %%d0,%%vbr\n\t"
			     "movel %2@(0),%%sp\n\t"
			     "movel %2@(4),%%a0\n\t"
			     "reset\n\t"
			     "jmp %%a0@\n\t"
			     ".chip 68k"
			     : : "d" (ttr), "d" (offset), "a" (rom)
			     : "d0", "a0", "memory");
	} else {
		/* 16 MB at phys, read and write, any function code */
		unsigned long ttr = (phys & 0xff000000) | 0x8777;
		unsigned long zero = 0;

		asm volatile(".chip 68030\n\t"
			     "pmove %0,%%tt0\n\t"
			     "lea %%pc@(1f),%%a0\n\t"
			     "addl %1,%%a0\n\t"
			     "addl %1,%3\n\t"
			     "jmp %%a0@\n"
			     "1:\tmovel #0x0808,%%d0\n\t"
			     "movec %%d0,%%cacr\n\t"
			     "pmove %3@,%%tc\n\t"
			     "pmove %3@,%%tt0\n\t"
			     "pmove %3@,%%tt1\n\t"
			     "pflusha\n\t"
			     "moveq #0,%%d0\n\t"
			     "movec %%d0,%%vbr\n\t"
			     "movel %2@(0),%%sp\n\t"
			     "movel %2@(4),%%a0\n\t"
			     "reset\n\t"
			     "jmp %%a0@\n\t"
			     ".chip 68k"
			     : : "m" (ttr), "d" (offset), "a" (rom), "a" (&zero)
			     : "d0", "a0", "memory");
	}

	pr_crit("Restart failed, please restart manually.\n");
	for (;;)
		;
}

void __init config_next(void)
{
	struct m68k_mem_info *top = &m68k_memory[m68k_num_memory - 1];
	char model[80];

	/*
	 * The ROM keeps its data (the monitor globals, its stack and
	 * vectors) in the top 8 KB of memory.  The loader passes whole
	 * banks, as the '030 mapping code needs sizes in 256 KB steps.
	 */
	memblock_reserve(top->addr + top->size - NEXT_ROM_RESERVED,
			 NEXT_ROM_RESERVED);

	next_quiesce_devices();

	next_get_model(model);
	pr_info("Detected %s, board revision %u, ethernet address %pM\n",
		model, next_bi_data.board_rev, next_bi_data.ether_addr);

	/* The serial console comes up before the platform devices. */
	next_scc_setup();

	next_rtc_init();

	mach_init_IRQ = next_init_IRQ;
	mach_sched_init = next_sched_init;
	mach_get_model = next_get_model;
	mach_get_hardware_list = next_get_hardware_list;
	mach_hwclk = next_hwclk;
	mach_reset = next_reset;
	register_platform_power_off(next_poweroff);
}
