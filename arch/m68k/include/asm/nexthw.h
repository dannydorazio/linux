/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NeXT hardware definitions
 */

#ifndef _ASM_M68K_NEXTHW_H
#define _ASM_M68K_NEXTHW_H

/*
 * The internal device space starts at physical 0x02000000.  head.S maps
 * 16 MB of it, which covers the plain device space, the BMAP alias at
 * 0x02100000 and the Turbo memory controller at 0x02200000.
 */
#define NEXT_ROM_PADDR		0x00000000	/* boot ROM, reset vectors first */
#define NEXT_ROM_RESERVED	0x2000	/* the ROM's data, at the top of RAM */

#define NEXT_IO_PADDR		0x02000000
#define NEXT_IO_VADDR		0xf0000000
#define NEXT_IO_SIZE		0x01000000

/*
 * On the '040 machines the device registers are accessed through the
 * BMAP alias of the device space.
 */
#define NEXT_BMAP_OFFSET	0x00100000

/*
 * Offsets into the device space.  DMA channel control/status registers
 * must be written with 32-bit accesses.
 */
#define NEXT_DMA_SCSI_CSR	0x00000010
#define NEXT_DMA_SOUNDOUT_CSR	0x00000040
#define NEXT_DMA_DISK_CSR	0x00000050
#define NEXT_DMA_SOUNDIN_CSR	0x00000080
#define NEXT_DMA_PRINTER_CSR	0x00000090
#define NEXT_DMA_SCC_CSR	0x000000c0
#define NEXT_DMA_DSP_CSR	0x000000d0
#define NEXT_DMA_ENETX_CSR	0x00000110
#define NEXT_DMA_ENETR_CSR	0x00000150
#define NEXT_DMA_VIDEO_CSR	0x00000180
#define NEXT_DMA_R2M_CSR	0x000001c0
#define NEXT_DMA_M2R_CSR	0x000001d0
/* DMA CSR bits, as read */
#define  NEXT_DMACSR_ENABLE	0x01000000	/* transfer enabled */
#define  NEXT_DMACSR_SUPDATE	0x02000000	/* continue buffer loaded */
#define  NEXT_DMACSR_READ	0x04000000	/* device to memory */
#define  NEXT_DMACSR_COMPLETE	0x08000000	/* a buffer completed */
#define  NEXT_DMACSR_BUSEXC	0x10000000	/* bus error */
/* DMA CSR bits, as written */
#define  NEXT_DMACSR_SETENABLE	0x00010000
#define  NEXT_DMACSR_SETSUPDATE	0x00020000	/* continue buffer is valid */
#define  NEXT_DMACSR_SETREAD	0x00040000	/* device to memory */
#define  NEXT_DMACSR_CLRCOMPLETE 0x00080000
#define  NEXT_DMACSR_RESET	0x00100000	/* clear complete, supdate, enable */
#define  NEXT_DMACSR_INITBUF	0x00200000	/* reset the channel buffer */
/* on Turbo machines, 0x00200000 sets "complete" instead */
#define  NEXT_DMACSR_INITBUF_TURBO 0x00800000
/*
 * Per-channel DMA registers, as offsets from the channel's CSR.  A channel
 * moves data from "next" up to "limit"; with SUPDATE set it then continues
 * from "start" to "stop".  "saved limit" holds where a device-to-memory
 * transfer that ended early (an ethernet frame) stopped.
 */
#define NEXT_DMA_SAVED_NEXT	0x3ff0
#define NEXT_DMA_SAVED_LIMIT	0x3ff4
#define NEXT_DMA_SAVED_START	0x3ff8
#define NEXT_DMA_SAVED_STOP	0x3ffc
#define NEXT_DMA_NEXT		0x4000
#define NEXT_DMA_LIMIT		0x4004
#define NEXT_DMA_START		0x4008
#define NEXT_DMA_STOP		0x400c
#define NEXT_DMA_NEXT_INITBUF	0x4200	/* write "next" and reset the buffer */
/* the ethernet receive channel's saved limit on Turbo machines */
#define NEXT_DMA_ENETR_TURBO_SAVED_LIMIT 0x00004050
/* Ethernet transmit: mark the last buffer of a frame in "limit" */
#define NEXT_DMA_ENETX_EOP	0x80000000
#define NEXT_ENET_OFFSET	0x00006000	/* MB8795 ethernet (BMAP on '040) */
#define  NEXT_ENET_SIZE		0x10
#define  NEXT_ENET_RESET	6		/* reset register */
#define   NEXT_ENET_RESET_MODE	0x80
#define NEXT_INTRSTAT_OFFSET	0x00007000	/* interrupt status */
#define NEXT_INTRMASK_OFFSET	0x00007800	/* interrupt mask */
#define NEXT_SCR1_OFFSET	0x0000c000	/* system control register 1 */
#define NEXT_SCR2_OFFSET	0x0000d000	/* system control register 2 */
#define NEXT_KMS_OFFSET		0x0000e000	/* sound and keyboard/mouse */
#define  NEXT_KMS_SIZE		0x10
#define NEXT_KM_CTRL_OFFSET	0x0000e001	/* keyboard/mouse control */
#define  NEXT_KM_NMI_RECEIVED	0x10	/* write: clear the NMI */
#define NEXT_TIMER_OFFSET	0x00016000	/* system timer (BMAP on '040) */
#define NEXT_SCSI_OFFSET	0x00014000	/* NCR53C90A SCSI (BMAP on '040) */
#define NEXT_SCC_OFFSET		0x00018000	/* Z85C30 SCC (BMAP on '040) */
#define NEXT_EVENTC_OFFSET	0x0001a000	/* event counter (BMAP on '040) */

#define NEXT_C16_CSR_OFFSET	0x00018180	/* Color video control (BMAP) */
#define  NEXT_C16_CSR_CLEAR_INT	0x01
#define  NEXT_C16_CSR_ENABLE_INT 0x02
#define  NEXT_C16_CSR_UNBLANK	0x04

/*
 * Video memory: 2 bits per pixel on the monochrome machines, 16 (RGBA4444)
 * on Color.  A line of the 1120x832 display takes 1152 pixels on the
 * non-Turbo machines.  Turbo machines have either at the same address.
 */
#define NEXT_MONO_VRAM_PADDR	0x0b000000
#define NEXT_COLOR_VRAM_PADDR	0x2c000000
#define NEXT_TURBO_VRAM_PADDR	0x0c000000
#define NEXT_MONO_LINE_BYTES	(1152 / 4)
#define NEXT_COLOR_LINE_BYTES	(1152 * 2)
#define NEXT_TURBO_MONO_LINE_BYTES (1120 / 4)
#define NEXT_TURBO_COLOR_LINE_BYTES (1120 * 2)
#define NEXT_DISPLAY_LINES	832

/* Z85C30 register offsets from the SCC base; channel A is the first port */
#define NEXT_SCC_CTRL_B		0
#define NEXT_SCC_CTRL_A		1
#define NEXT_SCC_DATA_B		2
#define NEXT_SCC_DATA_A		3

#ifndef __ASSEMBLER__

#include <linux/io.h>
#include <linux/types.h>
#include <asm/bootinfo-next.h>
#include <asm/irq.h>
#include <asm/setup.h>

#define NEXT_IO(offset)		((void __iomem *)(NEXT_IO_VADDR + (offset)))

/* Devices that sit behind the BMAP on the '040 machines */
#define NEXT_BMAP_IO(offset)	NEXT_IO((offset) + \
					(CPU_IS_040 ? NEXT_BMAP_OFFSET : 0))

/*
 * Interrupt sources, numbered by their bit in the interrupt status and
 * mask registers.  The comment gives the CPU interrupt level.
 */
#define NEXT_IRQ_BASE		IRQ_USER
#define NEXT_IRQ(bit)		(NEXT_IRQ_BASE + (bit))
#define NEXT_NUM_IRQS		32

#define NEXT_IRQ_SOFTINT0	NEXT_IRQ(0)	/* 1 */
#define NEXT_IRQ_SOFTINT1	NEXT_IRQ(1)	/* 2 */
#define NEXT_IRQ_POWER		NEXT_IRQ(2)	/* 3 */
#define NEXT_IRQ_KYBD_MOUSE	NEXT_IRQ(3)	/* 3 */
#define NEXT_IRQ_MONITOR	NEXT_IRQ(4)	/* 3 */
#define NEXT_IRQ_VIDEO		NEXT_IRQ(5)	/* 3 */
#define NEXT_IRQ_DSP_L3		NEXT_IRQ(6)	/* 3 */
#define NEXT_IRQ_PHONE		NEXT_IRQ(7)	/* 3, floppy */
#define NEXT_IRQ_SOUND_OVRUN	NEXT_IRQ(8)	/* 3 */
#define NEXT_IRQ_ENETR		NEXT_IRQ(9)	/* 3 */
#define NEXT_IRQ_ENETX		NEXT_IRQ(10)	/* 3 */
#define NEXT_IRQ_PRINTER	NEXT_IRQ(11)	/* 3 */
#define NEXT_IRQ_SCSI		NEXT_IRQ(12)	/* 3 */
#define NEXT_IRQ_DISK		NEXT_IRQ(13)	/* 3, colour video on Color */
#define NEXT_IRQ_DSP_L4		NEXT_IRQ(14)	/* 4 */
#define NEXT_IRQ_BUS		NEXT_IRQ(15)	/* 5 */
#define NEXT_IRQ_REMOTE		NEXT_IRQ(16)	/* 5 */
#define NEXT_IRQ_SCC		NEXT_IRQ(17)	/* 5 */
#define NEXT_IRQ_R2M_DMA	NEXT_IRQ(18)	/* 6 */
#define NEXT_IRQ_M2R_DMA	NEXT_IRQ(19)	/* 6 */
#define NEXT_IRQ_DSP_DMA	NEXT_IRQ(20)	/* 6 */
#define NEXT_IRQ_SCC_DMA	NEXT_IRQ(21)	/* 6 */
#define NEXT_IRQ_SOUND_IN_DMA	NEXT_IRQ(22)	/* 6 */
#define NEXT_IRQ_SOUND_OUT_DMA	NEXT_IRQ(23)	/* 6 */
#define NEXT_IRQ_PRINTER_DMA	NEXT_IRQ(24)	/* 6 */
#define NEXT_IRQ_DISK_DMA	NEXT_IRQ(25)	/* 6 */
#define NEXT_IRQ_SCSI_DMA	NEXT_IRQ(26)	/* 6 */
#define NEXT_IRQ_ENETR_DMA	NEXT_IRQ(27)	/* 6 */
#define NEXT_IRQ_ENETX_DMA	NEXT_IRQ(28)	/* 6 */
#define NEXT_IRQ_TIMER		NEXT_IRQ(29)	/* 6 */
#define NEXT_IRQ_PFAIL		NEXT_IRQ(30)	/* 7 */
#define NEXT_IRQ_NMI		NEXT_IRQ(31)	/* 7 */

static inline u32 next_dma_read(unsigned int csr, unsigned int reg)
{
	return in_be32(NEXT_IO(csr + reg));
}

static inline void next_dma_write(unsigned int csr, unsigned int reg, u32 val)
{
	out_be32(NEXT_IO(csr + reg), val);
}

struct next_booter_data {
	u32 model;		/* NEXT_MODEL_* */
	u32 board_rev;
	u8 ether_addr[6];
	u32 rom_mg;		/* physical address of the ROM monitor globals */
	u32 rom_vbr;		/* ROM vector base */
};

extern struct next_booter_data next_bi_data;

/*
 * The Turbo machines have a reworked DMA controller: no "saved" registers
 * (except the ethernet receive channel's saved limit, at its own
 * address), another bit to reset a channel's buffer, and no disk, SCC
 * or memory-to-memory channels.
 */
static inline bool next_is_turbo(void)
{
	return next_bi_data.model == NEXT_MODEL_TURBO_MONO ||
	       next_bi_data.model == NEXT_MODEL_TURBO_COLOR ||
	       next_bi_data.model == NEXT_MODEL_CUBE_TURBO;
}

static inline u32 next_dma_initbuf(void)
{
	return next_is_turbo() ? NEXT_DMACSR_INITBUF_TURBO : NEXT_DMACSR_INITBUF;
}

struct rtc_time;

void next_init_IRQ(void);
void next_sched_init(void);
void next_rtc_init(void);
const char *next_rtc_chip(void);
int next_hwclk(int op, struct rtc_time *t);
void next_poweroff(void);

#endif /* __ASSEMBLER__ */

#endif /* _ASM_M68K_NEXTHW_H */
