// SPDX-License-Identifier: GPL-2.0
/*
 * ESP front-end for NeXT computers
 *
 * The NeXT machines have an NCR 53C90A behind a small DMA interface whose
 * control register sits right after the chip's registers.  Data phases
 * whose length is a multiple of 16 bytes (the DMA burst size) go through
 * the SCSI channel of the NeXT DMA controller, using a bounce buffer so
 * that start and end are suitably aligned.  Everything else uses
 * programmed I/O through the FIFO.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include <scsi/scsi_host.h>

#include <asm/nexthw.h>

#include "esp_scsi.h"

#define DRV_MODULE_NAME		"next_esp"

/* DMA interface control register */
#define NEXT_ESP_DCTL		0x20
#define  DCTL_CLK_16MHZ		0xc0	/* clock select */
#define  DCTL_INTENB		0x20	/* pass ESP interrupts on */
#define  DCTL_DMAMODE		0x10	/* DMA instead of PIO */
#define  DCTL_DMAREAD		0x08	/* DMA from SCSI to memory */
#define  DCTL_FLUSH		0x04	/* flush the DMA FIFO */
#define  DCTL_RESET		0x02	/* reset the ESP */

#define NEXT_ESP_CLOCK		20000000

#define NEXT_ESP_DMA_ALIGN	16
#define NEXT_ESP_BOUNCE_SIZE	(64 * 1024)
/* the transfer counter has 16 bits */
#define NEXT_ESP_DMA_MAX	(NEXT_ESP_BOUNCE_SIZE - NEXT_ESP_DMA_ALIGN)

struct next_esp {
	struct esp	esp;
	void		*bounce;	/* NULL: PIO only */
	dma_addr_t	bounce_dma;
	void		*read_dst;	/* where to copy DMA-ed data to */
	u32		read_len;
	bool		dma_active;
};

static struct next_esp *to_next_esp(struct esp *esp)
{
	return container_of(esp, struct next_esp, esp);
}

static void next_esp_write8(struct esp *esp, u8 val, unsigned long reg)
{
	writeb(val, esp->regs + reg);
}

static u8 next_esp_read8(struct esp *esp, unsigned long reg)
{
	return readb(esp->regs + reg);
}

static int next_esp_irq_pending(struct esp *esp)
{
	return !!(next_esp_read8(esp, ESP_STATUS) & ESP_STAT_INTR);
}

static u32 next_esp_dma_length_limit(struct esp *esp, u32 dma_addr,
				     u32 dma_len)
{
	if (to_next_esp(esp)->bounce)
		return min_t(u32, dma_len, NEXT_ESP_DMA_MAX);
	return min_t(u32, dma_len, 0xffff);
}

static void next_esp_dma_stop(struct esp *esp)
{
	struct next_esp *nesp = to_next_esp(esp);

	next_dma_write(NEXT_DMA_SCSI_CSR, 0,
		       NEXT_DMACSR_RESET | NEXT_DMACSR_CLRCOMPLETE);
	next_dma_write(NEXT_DMA_SCSI_CSR, 0, 0);
	writeb(DCTL_CLK_16MHZ | DCTL_INTENB, esp->regs + NEXT_ESP_DCTL);
	nesp->dma_active = false;
	nesp->read_dst = NULL;
}

static void next_esp_reset_dma(struct esp *esp)
{
	writeb(DCTL_CLK_16MHZ | DCTL_INTENB | DCTL_FLUSH,
	       esp->regs + NEXT_ESP_DCTL);
	next_esp_dma_stop(esp);
}

static void next_esp_send_dma_cmd(struct esp *esp, u32 addr, u32 esp_count,
				  u32 dma_count, int write, u8 cmd)
{
	struct next_esp *nesp = to_next_esp(esp);
	u32 dir = write ? NEXT_DMACSR_SETREAD : 0;

	/*
	 * Only data transfers whose length is a whole number of bursts go
	 * through the DMA controller; the rest, including selection with
	 * the command bytes, uses the FIFO.
	 */
	if (!nesp->bounce || (cmd & ~ESP_CMD_DMA) != ESP_CMD_TI ||
	    !esp_count || esp_count % NEXT_ESP_DMA_ALIGN ||
	    esp_count > NEXT_ESP_DMA_MAX) {
		esp_send_pio_cmd(esp, addr, esp_count, dma_count, write, cmd);
		return;
	}

	/* "write" means from the device into memory */
	if (write) {
		nesp->read_dst = (void *)addr;
		nesp->read_len = esp_count;
	} else {
		memcpy(nesp->bounce, (void *)addr, esp_count);
	}
	esp->send_cmd_error = 0;
	nesp->dma_active = true;

	next_dma_write(NEXT_DMA_SCSI_CSR, 0,
		       NEXT_DMACSR_RESET | next_dma_initbuf() | dir);
	next_dma_write(NEXT_DMA_SCSI_CSR, NEXT_DMA_NEXT, nesp->bounce_dma);
	next_dma_write(NEXT_DMA_SCSI_CSR, NEXT_DMA_LIMIT,
		       nesp->bounce_dma + esp_count);
	next_dma_write(NEXT_DMA_SCSI_CSR, 0, NEXT_DMACSR_SETENABLE | dir);

	writeb(DCTL_CLK_16MHZ | DCTL_INTENB | DCTL_DMAMODE |
	       (write ? DCTL_DMAREAD : 0), esp->regs + NEXT_ESP_DCTL);

	next_esp_write8(esp, esp_count & 0xff, ESP_TCLOW);
	next_esp_write8(esp, (esp_count >> 8) & 0xff, ESP_TCMED);
	scsi_esp_cmd(esp, cmd);
}

static void next_esp_dma_drain(struct esp *esp)
{
	struct next_esp *nesp = to_next_esp(esp);

	if (nesp->dma_active && nesp->read_dst)
		memcpy(nesp->read_dst, nesp->bounce, nesp->read_len);
}

static void next_esp_dma_invalidate(struct esp *esp)
{
	if (to_next_esp(esp)->dma_active)
		next_esp_dma_stop(esp);
}

static int next_esp_dma_error(struct esp *esp)
{
	if (to_next_esp(esp)->dma_active &&
	    (next_dma_read(NEXT_DMA_SCSI_CSR, 0) & NEXT_DMACSR_BUSEXC))
		return 1;
	return esp->send_cmd_error;
}

static const struct esp_driver_ops next_esp_ops = {
	.esp_write8		= next_esp_write8,
	.esp_read8		= next_esp_read8,
	.irq_pending		= next_esp_irq_pending,
	.dma_length_limit	= next_esp_dma_length_limit,
	.reset_dma		= next_esp_reset_dma,
	.dma_drain		= next_esp_dma_drain,
	.dma_invalidate		= next_esp_dma_invalidate,
	.send_dma_cmd		= next_esp_send_dma_cmd,
	.dma_error		= next_esp_dma_error,
};

static int next_esp_probe(struct platform_device *pdev)
{
	const struct scsi_host_template *tpnt = &scsi_esp_template;
	struct Scsi_Host *host;
	struct next_esp *nesp;
	struct resource *res;
	struct esp *esp;
	int irq, err;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	host = scsi_host_alloc(tpnt, sizeof(struct next_esp));
	if (!host)
		return -ENOMEM;

	host->max_id = 8;
	host->dma_boundary = PAGE_SIZE - 1;
	/*
	 * The DMA engine moves less than 64 KB at a time anyway.  Larger
	 * commands would also make the block layer read ahead twice the
	 * largest command (8 MB) from these disks, which report no optimal
	 * size: more than a 32 MB machine can spare.
	 */
	host->max_sectors = SZ_64K >> SECTOR_SHIFT;
	nesp = shost_priv(host);
	esp = &nesp->esp;
	esp->host = host;
	esp->dev = &pdev->dev;

	err = -ENOMEM;
	esp->command_block = kzalloc(16, GFP_KERNEL);
	if (!esp->command_block)
		goto fail_put_host;
	esp->command_block_dma = (dma_addr_t)esp->command_block;

	esp->regs = ioremap(res->start, resource_size(res));
	if (!esp->regs)
		goto fail_free_command_block;
	esp->fifo_reg = esp->regs + ESP_FDATA;

	esp->scsi_id = 7;
	host->this_id = esp->scsi_id;
	esp->scsi_id_mask = 1 << esp->scsi_id;
	esp->cfreq = NEXT_ESP_CLOCK;

	esp->ops = &next_esp_ops;
	/* the core hands us CPU addresses; DMA goes via the bounce buffer */
	esp->flags = ESP_FLAG_NO_DMA_MAP | ESP_FLAG_DISABLE_SYNC;

	if (!dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32)))
		nesp->bounce = dma_alloc_coherent(&pdev->dev,
						  NEXT_ESP_BOUNCE_SIZE,
						  &nesp->bounce_dma,
						  GFP_KERNEL);
	dev_info(&pdev->dev, "using %s\n", nesp->bounce ? "DMA" : "PIO");

	/* Reset the chip through the DMA interface, then enable interrupts */
	writeb(DCTL_CLK_16MHZ | DCTL_RESET, esp->regs + NEXT_ESP_DCTL);
	udelay(10);
	next_esp_reset_dma(esp);
	next_esp_write8(esp, 0, ESP_TCLOW);
	next_esp_write8(esp, 0, ESP_TCMED);

	host->irq = irq;
	err = request_irq(irq, scsi_esp_intr, IRQF_SHARED, "ESP", esp);
	if (err)
		goto fail_unmap;

	platform_set_drvdata(pdev, esp);

	err = scsi_esp_register(esp);
	if (err)
		goto fail_free_irq;

	return 0;

fail_free_irq:
	free_irq(irq, esp);
fail_unmap:
	if (nesp->bounce)
		dma_free_coherent(&pdev->dev, NEXT_ESP_BOUNCE_SIZE,
				  nesp->bounce, nesp->bounce_dma);
	iounmap(esp->regs);
fail_free_command_block:
	kfree(esp->command_block);
fail_put_host:
	scsi_host_put(host);
	return err;
}

static void next_esp_remove(struct platform_device *pdev)
{
	struct esp *esp = platform_get_drvdata(pdev);
	struct next_esp *nesp = to_next_esp(esp);

	scsi_esp_unregister(esp);
	next_esp_dma_stop(esp);
	writeb(DCTL_CLK_16MHZ, esp->regs + NEXT_ESP_DCTL);
	free_irq(esp->host->irq, esp);
	if (nesp->bounce)
		dma_free_coherent(&pdev->dev, NEXT_ESP_BOUNCE_SIZE,
				  nesp->bounce, nesp->bounce_dma);
	iounmap(esp->regs);
	kfree(esp->command_block);
	scsi_host_put(esp->host);
}

static struct platform_driver next_esp_driver = {
	.probe	= next_esp_probe,
	.remove	= next_esp_remove,
	.driver	= {
		.name	= DRV_MODULE_NAME,
	},
};
module_platform_driver(next_esp_driver);

MODULE_DESCRIPTION("ESP SCSI driver for NeXT computers");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_MODULE_NAME);
