// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM driver for the NeXTdimension board
 *
 * The NeXTdimension is a NeXTBus board for the NeXTcube with an i860
 * processor, its own memory and a 32-bit color display of 1120x832.  Its
 * video memory holds a pixel in each big-endian word as red, green, blue
 * and an unused byte, 1152 pixels to a line; the board scans it out through
 * a Bt463 RAMDAC, whose palette is loaded with a linear ramp.  The host
 * reaches the video memory, the board's DRAM and its registers directly.
 *
 * Framebuffers are kept in system memory and copied to the video memory as
 * they change.  The native format is XBGR8888 (bytes R, G, B, X); XRGB8888
 * in either byte order is converted.
 *
 * The console draws with the i860.  At probe, the driver resets it into
 * its ROM, loads a small program (nextdimension_fw.h, from
 * nd-firmware/ndfw.s in the next68k-linux tree) at the start of the
 * board's DRAM and interrupts it, upon which the ROM jumps there.  The
 * program takes fill, copy and glyph commands from a ring in DRAM.  fbdev
 * then draws straight into the video memory through it, so scrolling is a
 * copy on the board rather than redrawing the screen over the NeXTBus.  If
 * the program doesn't start, fbdev emulation keeps its framebuffer in
 * system memory like any other client, in big-endian XRGB8888.
 */

#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/console.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/clients/drm_fbdev_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_vblank_helper.h>

#ifdef CONFIG_NEXT
#include <asm/nexthw.h>
#endif

#define NEXTDIM_WIDTH		1120
#define NEXTDIM_HEIGHT		832
#define NEXTDIM_PITCH		(1152 * 4)

/* Memory controller registers */
#define NEXTDIM_MC_CSR0		0x0000
#define  NEXTDIM_CSR0_RESET	0x00000001	/* restart the i860 */
#define  NEXTDIM_CSR0_CS8	0x00000002	/* the i860 boots from its 8-bit ROM */
#define  NEXTDIM_CSR0_IMASK	0x00000004	/* the i860 takes interrupts */
#define  NEXTDIM_CSR0_INT860	0x00000008	/* interrupt the i860 */
#define NEXTDIM_MC_VRAM_TIMING	0x2000
#define  NEXTDIM_VRAM_UNBLANK	0x00000001

/* Bt463 RAMDAC: an address register in two halves, then data ports */
#define NEXTDIM_DAC_ADDR_LO	0x0
#define NEXTDIM_DAC_ADDR_HI	0x4
#define NEXTDIM_DAC_REG		0x8	/* the control register addressed */
#define NEXTDIM_DAC_PALETTE	0xc	/* red, green, blue; auto-increments */

/* The i860 program's shared area and command ring, in DRAM */
#define NEXTDIM_FW_READY	0x8000
#define  NEXTDIM_FW_MAGIC	0x4e44464c	/* "NDFL" */
#define NEXTDIM_FW_HEAD		0x8004
#define NEXTDIM_FW_TAIL		0x8008
#define NEXTDIM_FW_EPSR		0x800c	/* the i860's, for /proc/hardware */
#define NEXTDIM_RING		0x10000
#define NEXTDIM_RING_WORDS	0x4000

#define NEXTDIM_OP_FILL		1	/* dst w h color */
#define NEXTDIM_OP_COPY		2	/* src dst w h */
#define NEXTDIM_OP_IMAGE	3	/* dst w h fg bg stride, bits */
#define NEXTDIM_OP_WRAP		4

/*
 * How long the i860 may go without taking a command before it's given up
 * on.  A screenful of scrolling can keep it busy for a while, and in an
 * emulator that runs it apart from the CPU, longer.
 */
#define NEXTDIM_TIMEOUT_US	10000000

#include "nextdimension_fw.h"

struct nextdim {
	struct drm_device dev;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_display_mode mode;

	void __iomem *vram;
	void __iomem *mc;
	void __iomem *dac;
	void __iomem *dram;
	phys_addr_t vram_phys;

	bool accel;		/* the i860 draws the console */
	unsigned int head;	/* where the next command goes in the ring */

	/*
	 * Another framebuffer was shown in the video memory, where the
	 * console is drawn: redraw it when it's back.
	 */
	bool console_lost;
	struct work_struct redraw_work;
};

static struct nextdim *to_nextdim(struct drm_device *dev)
{
	return container_of(dev, struct nextdim, dev);
}

/*
 * RAMDAC
 */

static void nextdim_dac_addr(struct nextdim *nd, unsigned int addr)
{
	writeb(addr & 0xff, nd->dac + NEXTDIM_DAC_ADDR_LO);
	writeb(addr >> 8, nd->dac + NEXTDIM_DAC_ADDR_HI);
}

static void nextdim_dac_write(struct nextdim *nd, unsigned int reg, u8 val)
{
	nextdim_dac_addr(nd, reg);
	writeb(val, nd->dac + NEXTDIM_DAC_REG);
}

/*
 * 24-bit true color through a linear palette.  The register meanings are
 * from Brooktree's Bt463 data sheet; tgafb sets the chip up the same way,
 * except for the cursor, sync and blanking pedestal.
 */
static void nextdim_dac_init(struct nextdim *nd)
{
	unsigned int i;

	nextdim_dac_write(nd, 0x201, 0x40);	/* 4:1 multiplexing */
	nextdim_dac_write(nd, 0x202, 0x00);	/* 16 window types, no cursor */
	nextdim_dac_write(nd, 0x203, 0x80);	/* sync on green, 0 IRE */
	for (i = 0x205; i <= 0x208; i++)
		nextdim_dac_write(nd, i, 0xff);	/* read mask: all planes */
	for (i = 0x209; i <= 0x20c; i++)
		nextdim_dac_write(nd, i, 0x00);	/* blink mask: none */

	/* the window type table: 8 planes a channel, true color, everywhere */
	for (i = 0x300; i <= 0x30f; i++) {
		nextdim_dac_addr(nd, i);
		writeb(0x00, nd->dac + NEXTDIM_DAC_REG);
		writeb(0x01, nd->dac + NEXTDIM_DAC_REG);
		writeb(0x00, nd->dac + NEXTDIM_DAC_REG);
	}

	/* both 256-entry palettes as ramps */
	for (i = 0; i < 0x200; i++) {
		nextdim_dac_addr(nd, i);
		writeb(i & 0xff, nd->dac + NEXTDIM_DAC_PALETTE);
		writeb(i & 0xff, nd->dac + NEXTDIM_DAC_PALETTE);
		writeb(i & 0xff, nd->dac + NEXTDIM_DAC_PALETTE);
	}
}

static void nextdim_unblank(struct nextdim *nd, bool on)
{
	__raw_writel(on ? NEXTDIM_VRAM_UNBLANK : 0,
		     nd->mc + NEXTDIM_MC_VRAM_TIMING);
}

/*
 * The i860
 */

static u32 nextdim_dram_read(struct nextdim *nd, unsigned int offset)
{
	return __raw_readl(nd->dram + offset);
}

static void nextdim_dram_write(struct nextdim *nd, unsigned int offset, u32 val)
{
	__raw_writel(val, nd->dram + offset);
}

/*
 * Restart the i860 into its ROM, which then runs the program.  After a
 * restart the ROM tests the start of DRAM and sets CSR0 up, the i860's
 * interrupt enable last, then waits to be interrupted: the program can only
 * be loaded, and the i860 interrupted, after that.  An interrupt sent
 * earlier is lost when the ROM writes CSR0.  (Found by experiment in
 * Previous, which runs the board's ROM.)
 */
static int nextdim_i860_start(struct nextdim *nd)
{
	unsigned int i;
	u32 csr0, ready;
	int ret;

	/* CS8 as at power-on: the ROM is 8 bits wide */
	__raw_writel(NEXTDIM_CSR0_CS8 | NEXTDIM_CSR0_RESET,
		     nd->mc + NEXTDIM_MC_CSR0);
	ret = readx_poll_timeout(__raw_readl, nd->mc + NEXTDIM_MC_CSR0, csr0,
				 csr0 & NEXTDIM_CSR0_IMASK, 1000,
				 NEXTDIM_TIMEOUT_US);
	if (ret)
		return ret;

	/* the i860 fetches instructions little-endian, a doubleword at once */
	for (i = 0; i < ARRAY_SIZE(nextdim_fw); i++)
		nextdim_dram_write(nd, (i * 4) ^ 4, nextdim_fw[i]);
	nextdim_dram_write(nd, NEXTDIM_FW_READY, 0);
	nextdim_dram_write(nd, NEXTDIM_FW_HEAD, 0);
	nd->head = 0;

	/* the ROM then jumps to the start of DRAM */
	csr0 = __raw_readl(nd->mc + NEXTDIM_MC_CSR0);
	__raw_writel(csr0 | NEXTDIM_CSR0_INT860, nd->mc + NEXTDIM_MC_CSR0);
	ret = readx_poll_timeout(__raw_readl, nd->dram + NEXTDIM_FW_READY, ready,
				 ready == NEXTDIM_FW_MAGIC, 1000,
				 NEXTDIM_TIMEOUT_US);
	csr0 = __raw_readl(nd->mc + NEXTDIM_MC_CSR0);
	__raw_writel(csr0 & ~NEXTDIM_CSR0_INT860, nd->mc + NEXTDIM_MC_CSR0);

	return ret;
}

/*
 * The i860 stopped taking commands: stop it, so it doesn't draw over the
 * CPU with what's left in the ring, and draw with the CPU from now on.
 */
static void nextdim_i860_dead(struct nextdim *nd)
{
	unsigned int tail = nextdim_dram_read(nd, NEXTDIM_FW_TAIL);
	unsigned int cmd = NEXTDIM_RING + (tail % NEXTDIM_RING_WORDS) * 4;

	drm_err(&nd->dev,
		"the i860 stopped at command %u (head %u): %08x %08x %08x %08x %08x %08x; drawing without it\n",
		tail, nd->head, nextdim_dram_read(nd, cmd),
		nextdim_dram_read(nd, cmd + 4), nextdim_dram_read(nd, cmd + 8),
		nextdim_dram_read(nd, cmd + 12), nextdim_dram_read(nd, cmd + 16),
		nextdim_dram_read(nd, cmd + 20));
	__raw_writel(NEXTDIM_CSR0_CS8 | NEXTDIM_CSR0_RESET,
		     nd->mc + NEXTDIM_MC_CSR0);
	nd->accel = false;
}

/*
 * The tail, for a wait that started at *last: it gives up once the i860
 * has gone NEXTDIM_TIMEOUT_US without taking a command.
 */
static unsigned int nextdim_wait_tail(struct nextdim *nd, unsigned int *last,
				      unsigned int *us)
{
	unsigned int tail = nextdim_dram_read(nd, NEXTDIM_FW_TAIL);

	if (tail != *last) {
		*last = tail;
		*us = 0;
	} else if (++*us >= NEXTDIM_TIMEOUT_US) {
		nextdim_i860_dead(nd);
	} else {
		udelay(1);
	}
	return tail;
}

/* Wait for the i860 to take everything queued. */
static void nextdim_sync(struct nextdim *nd)
{
	unsigned int last = ~0U, us = 0;

	while (nd->accel && nextdim_wait_tail(nd, &last, &us) != nd->head)
		;
}

/*
 * Make room for a command of len words; where it goes, or NULL if the
 * i860 is gone.  A command never wraps around the end of the ring, and
 * always leaves the ring's last word for a WRAP.  The ring is never quite
 * full either, so that a full one isn't taken for empty.
 */
static void __iomem *nextdim_ring_reserve(struct nextdim *nd, unsigned int len)
{
	unsigned int last = ~0U, us = 0, tail;

	while (nd->accel) {
		tail = nextdim_wait_tail(nd, &last, &us);
		if (nd->head >= tail) {
			if (nd->head + len < NEXTDIM_RING_WORDS)
				goto room;
			if (len < tail) {
				nextdim_dram_write(nd, NEXTDIM_RING + nd->head * 4,
						   NEXTDIM_OP_WRAP);
				nd->head = 0;
				nextdim_dram_write(nd, NEXTDIM_FW_HEAD, 0);
				goto room;
			}
		} else if (nd->head + len < tail) {
			goto room;
		}
	}
	return NULL;

room:
	return nd->dram + NEXTDIM_RING + nd->head * 4;
}

/* Hand the i860 the command of len words just written. */
static void nextdim_ring_commit(struct nextdim *nd, unsigned int len)
{
	nd->head += len;
	nextdim_dram_write(nd, NEXTDIM_FW_HEAD, nd->head);
}

/*
 * fbdev, drawing in the video memory through the i860
 */

static struct nextdim *nextdim_from_info(struct fb_info *info)
{
	struct drm_fb_helper *helper = info->par;

	return to_nextdim(helper->dev);
}

static u32 nextdim_info_color(struct fb_info *info, u32 color)
{
	if (info->fix.visual == FB_VISUAL_TRUECOLOR && color < 16)
		return ((u32 *)info->pseudo_palette)[color];
	return color;
}

static unsigned int nextdim_offset(unsigned int x, unsigned int y)
{
	return y * NEXTDIM_PITCH + x * 4;
}

static void nextdim_fillrect(struct fb_info *info,
			     const struct fb_fillrect *rect)
{
	struct nextdim *nd = nextdim_from_info(info);
	void __iomem *cmd;

	if (!rect->width || !rect->height)
		return;
	if (rect->rop == ROP_COPY && nd->accel) {
		cmd = nextdim_ring_reserve(nd, 5);
		if (cmd) {
			__raw_writel(NEXTDIM_OP_FILL, cmd);
			__raw_writel(nextdim_offset(rect->dx, rect->dy), cmd + 4);
			__raw_writel(rect->width, cmd + 8);
			__raw_writel(rect->height, cmd + 12);
			__raw_writel(nextdim_info_color(info, rect->color),
				     cmd + 16);
			nextdim_ring_commit(nd, 5);
			return;
		}
	}
	nextdim_sync(nd);
	cfb_fillrect(info, rect);
}

static void nextdim_copyarea(struct fb_info *info,
			     const struct fb_copyarea *area)
{
	struct nextdim *nd = nextdim_from_info(info);
	void __iomem *cmd;

	if (!area->width || !area->height)
		return;
	if (nd->accel) {
		cmd = nextdim_ring_reserve(nd, 5);
		if (cmd) {
			__raw_writel(NEXTDIM_OP_COPY, cmd);
			__raw_writel(nextdim_offset(area->sx, area->sy), cmd + 4);
			__raw_writel(nextdim_offset(area->dx, area->dy), cmd + 8);
			__raw_writel(area->width, cmd + 12);
			__raw_writel(area->height, cmd + 16);
			nextdim_ring_commit(nd, 5);
			return;
		}
	}
	nextdim_sync(nd);
	cfb_copyarea(info, area);
}

static void nextdim_imageblit(struct fb_info *info,
			      const struct fb_image *image)
{
	struct nextdim *nd = nextdim_from_info(info);
	unsigned int pitch = DIV_ROUND_UP(image->width, 8);
	unsigned int stride = ALIGN(pitch, 4);
	unsigned int lines_max = (NEXTDIM_RING_WORDS / 2) / (stride / 4);
	unsigned int y, lines, len, i;
	const u8 *src = image->data;
	void __iomem *cmd;

	if (!image->width || !image->height)
		return;
	if (image->depth != 1 || !nd->accel || !lines_max)
		goto cpu;

	/* in pieces the ring can take */
	for (y = 0; y < image->height; y += lines) {
		lines = min(image->height - y, lines_max);
		len = 7 + lines * stride / 4;
		cmd = nextdim_ring_reserve(nd, len);
		if (!cmd)
			goto cpu_rest;
		__raw_writel(NEXTDIM_OP_IMAGE, cmd);
		__raw_writel(nextdim_offset(image->dx, image->dy + y), cmd + 4);
		__raw_writel(image->width, cmd + 8);
		__raw_writel(lines, cmd + 12);
		__raw_writel(nextdim_info_color(info, image->fg_color), cmd + 16);
		__raw_writel(nextdim_info_color(info, image->bg_color), cmd + 20);
		__raw_writel(stride, cmd + 24);
		cmd += 28;
		for (i = 0; i < lines; i++) {
			const u8 *line = src + (y + i) * pitch;
			unsigned int b;

			for (b = 0; b < stride; b += 4) {
				u32 w = 0;
				unsigned int k;

				for (k = 0; k < 4; k++)
					w = w << 8 |
					    (b + k < pitch ? line[b + k] : 0);
				__raw_writel(w, cmd);
				cmd += 4;
			}
		}
		nextdim_ring_commit(nd, len);
	}
	return;

cpu_rest:
	{
		struct fb_image rest = *image;

		rest.dy += y;
		rest.height -= y;
		rest.data += y * pitch;
		nextdim_sync(nd);
		cfb_imageblit(info, &rest);
		return;
	}
cpu:
	nextdim_sync(nd);
	cfb_imageblit(info, image);
}

static int nextdim_fb_sync(struct fb_info *info)
{
	nextdim_sync(nextdim_from_info(info));
	return 0;
}

static ssize_t nextdim_fb_read(struct fb_info *info, char __user *buf,
			       size_t count, loff_t *ppos)
{
	nextdim_sync(nextdim_from_info(info));
	return fb_io_read(info, buf, count, ppos);
}

static ssize_t nextdim_fb_write(struct fb_info *info, const char __user *buf,
				size_t count, loff_t *ppos)
{
	nextdim_sync(nextdim_from_info(info));
	return fb_io_write(info, buf, count, ppos);
}

/* the video memory's pixels: red in the most significant byte */
static void nextdim_set_bitfields(struct fb_var_screeninfo *var)
{
	var->red = (struct fb_bitfield){ .offset = 24, .length = 8 };
	var->green = (struct fb_bitfield){ .offset = 16, .length = 8 };
	var->blue = (struct fb_bitfield){ .offset = 8, .length = 8 };
	var->transp = (struct fb_bitfield){ };
}

static int nextdim_fb_check_var(struct fb_var_screeninfo *var,
				struct fb_info *info)
{
	int ret = drm_fb_helper_check_var(var, info);

	if (!ret)
		nextdim_set_bitfields(var);
	return ret;
}

static void nextdim_fb_destroy(struct fb_info *info)
{
	struct drm_fb_helper *fb_helper = info->par;

	drm_fb_helper_fini(fb_helper);
	drm_client_buffer_delete(fb_helper->buffer);
	drm_client_release(&fb_helper->client);
}

static const struct fb_ops nextdim_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= nextdim_fb_read,
	.fb_write	= nextdim_fb_write,
	.fb_fillrect	= nextdim_fillrect,
	.fb_copyarea	= nextdim_copyarea,
	.fb_imageblit	= nextdim_imageblit,
	.fb_sync	= nextdim_fb_sync,
	.fb_mmap	= fb_io_mmap,
	.fb_check_var	= nextdim_fb_check_var,
	.fb_set_par	= drm_fb_helper_set_par,
	.fb_setcmap	= drm_fb_helper_setcmap,
	.fb_blank	= drm_fb_helper_blank,
	.fb_pan_display	= drm_fb_helper_pan_display,
	.fb_ioctl	= drm_fb_helper_ioctl,
	.fb_destroy	= nextdim_fb_destroy,
};

/* Redraw the console, which has the video memory back. */
static void nextdim_redraw_work(struct work_struct *work)
{
	struct nextdim *nd = container_of(work, struct nextdim, redraw_work);
	struct fb_info *info = nd->dev.fb_helper ? nd->dev.fb_helper->info : NULL;
	struct fb_var_screeninfo var;

	if (!info)
		return;
	console_lock();
	lock_fb_info(info);
	var = info->var;
	var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
	fb_set_var_from_user(info, &var);
	unlock_fb_info(info);
	console_unlock();
}

static void nextdim_fb_restore(struct drm_fb_helper *fb_helper)
{
	struct nextdim *nd = to_nextdim(fb_helper->dev);

	if (nd->console_lost) {
		nd->console_lost = false;
		schedule_work(&nd->redraw_work);
	}
}

/* nothing to copy: the console is drawn in place */
static const struct drm_fb_helper_funcs nextdim_fb_helper_funcs = {
	.fb_restore = nextdim_fb_restore,
};

static int nextdim_fbdev_probe(struct drm_fb_helper *fb_helper,
			       struct drm_fb_helper_surface_size *sizes)
{
	struct nextdim *nd = to_nextdim(fb_helper->dev);
	struct fb_info *info = fb_helper->info;
	struct drm_client_buffer *buffer;

	if (!nd->accel)
		return drm_fbdev_shmem_driver_fbdev_probe(fb_helper, sizes);

	/*
	 * The mode setting wants a framebuffer; the plane doesn't copy this
	 * one, whose pages are never touched.  The console is in the video
	 * memory itself.
	 */
	buffer = drm_client_buffer_create_dumb(&fb_helper->client,
					       sizes->surface_width,
					       sizes->surface_height,
					       DRM_FORMAT_BGRX8888);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);
	fb_helper->funcs = &nextdim_fb_helper_funcs;
	fb_helper->buffer = buffer;
	fb_helper->fb = buffer->fb;

	drm_fb_helper_fill_info(info, fb_helper, sizes);
	info->fbops = &nextdim_fb_ops;
	/*
	 * Not FBINFO_HWACCEL_IMAGEBLIT, though the i860 draws the glyphs
	 * too: with it fbcon scrolls by redrawing every changed character
	 * with the CPU, rather than with a copy on the board.
	 */
	info->flags |= FBINFO_HWACCEL_COPYAREA | FBINFO_HWACCEL_FILLRECT;
	info->screen_base = nd->vram;
	info->screen_size = NEXTDIM_PITCH * NEXTDIM_HEIGHT;
	info->fix.smem_start = nd->vram_phys;
	info->fix.smem_len = info->screen_size;
	info->fix.line_length = NEXTDIM_PITCH;
	info->fix.accel = FB_ACCEL_NONE;

	nextdim_set_bitfields(&info->var);

	return 0;
}

static bool nextdim_is_fbdev_fb(struct nextdim *nd,
				const struct drm_framebuffer *fb)
{
	return nd->accel && nd->dev.fb_helper && nd->dev.fb_helper->fb == fb;
}

/*
 * Pixel conversions
 */

static void nextdim_update(struct nextdim *nd, const struct drm_framebuffer *fb,
			   const struct iosys_map *map,
			   const struct drm_rect *src_rect,
			   const struct drm_rect *clip)
{
	unsigned int width = drm_rect_width(clip);
	unsigned int x = clip->x1 - src_rect->x1;
	unsigned int y = clip->y1 - src_rect->y1;
	void __iomem *dst = nd->vram + y * NEXTDIM_PITCH + x * 4;
	const u8 *src = map->vaddr + clip->y1 * fb->pitches[0] + clip->x1 * 4;
	unsigned int i;

	for (y = clip->y1; y < clip->y2; y++) {
		const u32 *s = (const u32 *)src;

		switch (fb->format->format) {
		case DRM_FORMAT_XBGR8888:	/* the hardware's */
			memcpy_toio(dst, src, width * 4);
			break;
		case DRM_FORMAT_BGRX8888:	/* XRGB8888, big-endian */
			for (i = 0; i < width; i++)
				__raw_writel(s[i] << 8, dst + i * 4);
			break;
		case DRM_FORMAT_XRGB8888:	/* little-endian */
			for (i = 0; i < width; i++)
				__raw_writel(swab32(s[i]) << 8, dst + i * 4);
			break;
		}
		src += fb->pitches[0];
		dst += NEXTDIM_PITCH;
	}
}

/*
 * Plane
 */

static const u32 nextdim_formats[] = {
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_BGRX8888,
};

static int nextdim_plane_atomic_check(struct drm_plane *plane,
				      struct drm_atomic_commit *state)
{
	struct drm_plane_state *plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state = NULL;

	if (plane_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state,
							   plane_state->crtc);

	return drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, false);
}

static void nextdim_plane_atomic_update(struct drm_plane *plane,
					struct drm_atomic_commit *state)
{
	struct nextdim *nd = to_nextdim(plane->dev);
	struct drm_plane_state *plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old_plane_state =
		drm_atomic_get_old_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(plane_state);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect src_rect, damage;
	int idx;

	/* the accelerated console is in the video memory already */
	if (!fb || !plane_state->visible || nextdim_is_fbdev_fb(nd, fb))
		return;
	nd->console_lost = true;

	if (!drm_dev_enter(&nd->dev, &idx))
		return;

	drm_rect_fp_to_int(&src_rect, &plane_state->src);
	drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &damage)
		nextdim_update(nd, fb, shadow_plane_state->data, &src_rect,
			       &damage);

	drm_dev_exit(idx);
}

static void nextdim_plane_atomic_disable(struct drm_plane *plane,
					 struct drm_atomic_commit *state)
{
	struct nextdim *nd = to_nextdim(plane->dev);
	int idx;

	if (!drm_dev_enter(&nd->dev, &idx))
		return;
	nd->console_lost = true;
	memset_io(nd->vram, 0, NEXTDIM_PITCH * NEXTDIM_HEIGHT);
	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs nextdim_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = nextdim_plane_atomic_check,
	.atomic_update = nextdim_plane_atomic_update,
	.atomic_disable = nextdim_plane_atomic_disable,
};

static const struct drm_plane_funcs nextdim_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

/*
 * CRTC
 */

static enum drm_mode_status
nextdim_crtc_mode_valid(struct drm_crtc *crtc,
			const struct drm_display_mode *mode)
{
	struct nextdim *nd = to_nextdim(crtc->dev);

	return drm_crtc_helper_mode_valid_fixed(crtc, mode, &nd->mode);
}

static int nextdim_crtc_atomic_check(struct drm_crtc *crtc,
				     struct drm_atomic_commit *state)
{
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);

	if (!crtc_state->enable)
		return 0;

	return drm_atomic_helper_check_crtc_primary_plane(crtc_state);
}

static void nextdim_crtc_atomic_enable(struct drm_crtc *crtc,
				       struct drm_atomic_commit *state)
{
	struct nextdim *nd = to_nextdim(crtc->dev);
	int idx;

	if (drm_dev_enter(&nd->dev, &idx)) {
		nextdim_unblank(nd, true);
		drm_dev_exit(idx);
	}
	drm_crtc_vblank_on(crtc);
}

static void nextdim_crtc_atomic_disable(struct drm_crtc *crtc,
					struct drm_atomic_commit *state)
{
	struct nextdim *nd = to_nextdim(crtc->dev);
	int idx;

	drm_crtc_vblank_off(crtc);
	if (drm_dev_enter(&nd->dev, &idx)) {
		nextdim_unblank(nd, false);
		drm_dev_exit(idx);
	}
}

static const struct drm_crtc_helper_funcs nextdim_crtc_helper_funcs = {
	.mode_valid = nextdim_crtc_mode_valid,
	.atomic_check = nextdim_crtc_atomic_check,
	.atomic_flush = drm_crtc_vblank_atomic_flush,
	.atomic_enable = nextdim_crtc_atomic_enable,
	.atomic_disable = nextdim_crtc_atomic_disable,
};

static const struct drm_crtc_funcs nextdim_crtc_funcs = {
	.atomic_create_state = drm_atomic_helper_crtc_create_state,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	DRM_CRTC_VBLANK_TIMER_FUNCS,
};

/*
 * Encoder and connector
 */

static const struct drm_encoder_funcs nextdim_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int nextdim_connector_get_modes(struct drm_connector *connector)
{
	struct nextdim *nd = to_nextdim(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, &nd->mode);
}

static const struct drm_connector_helper_funcs nextdim_connector_helper_funcs = {
	.get_modes = nextdim_connector_get_modes,
};

static const struct drm_connector_funcs nextdim_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs nextdim_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/*
 * Driver
 */

DEFINE_DRM_GEM_FOPS(nextdim_fops);

static const struct drm_driver nextdim_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &nextdim_fops,
	.name			= "nextdimension",
	.desc			= "NeXTdimension",
	.major			= 1,
	.minor			= 0,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.fbdev_probe		= nextdim_fbdev_probe,
};

static int nextdim_kms_init(struct nextdim *nd)
{
	struct drm_device *dev = &nd->dev;
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	dev->mode_config.min_width = NEXTDIM_WIDTH;
	dev->mode_config.max_width = NEXTDIM_WIDTH;
	dev->mode_config.min_height = NEXTDIM_HEIGHT;
	dev->mode_config.max_height = NEXTDIM_HEIGHT;
	dev->mode_config.funcs = &nextdim_mode_config_funcs;
	/* legacy ADDFB gets XRGB8888 in the CPU's byte order: BGRX8888 */
	dev->mode_config.quirk_addfb_prefer_host_byte_order = true;

	ret = drm_universal_plane_init(dev, &nd->plane, 0, &nextdim_plane_funcs,
				       nextdim_formats,
				       ARRAY_SIZE(nextdim_formats), NULL,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&nd->plane, &nextdim_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&nd->plane);

	ret = drm_crtc_init_with_planes(dev, &nd->crtc, &nd->plane, NULL,
					&nextdim_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&nd->crtc, &nextdim_crtc_helper_funcs);

	ret = drm_encoder_init(dev, &nd->encoder, &nextdim_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;
	nd->encoder.possible_crtcs = drm_crtc_mask(&nd->crtc);

	ret = drm_connector_init(dev, &nd->connector, &nextdim_connector_funcs,
				 DRM_MODE_CONNECTOR_Unknown);
	if (ret)
		return ret;
	drm_connector_helper_add(&nd->connector,
				 &nextdim_connector_helper_funcs);
	ret = drm_connector_attach_encoder(&nd->connector, &nd->encoder);
	if (ret)
		return ret;

	ret = drm_vblank_init(dev, 1);
	if (ret)
		return ret;

	drm_mode_config_reset(dev);

	return 0;
}

static int nextdim_probe(struct platform_device *pdev)
{
	/* the NeXT 21" Color Display: 92 dpi, 68 Hz */
	static const struct drm_display_mode mode = {
		DRM_MODE_INIT(68, NEXTDIM_WIDTH, NEXTDIM_HEIGHT, 310, 230)
	};
	struct nextdim *nd;
	struct drm_device *dev;
	struct resource *res;
	int ret;

	nd = devm_drm_dev_alloc(&pdev->dev, &nextdim_driver, struct nextdim,
				dev);
	if (IS_ERR(nd))
		return PTR_ERR(nd);
	dev = &nd->dev;
	nd->mode = mode;
	INIT_WORK(&nd->redraw_work, nextdim_redraw_work);

	nd->vram = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(nd->vram))
		return PTR_ERR(nd->vram);
	if (resource_size(res) < NEXTDIM_PITCH * NEXTDIM_HEIGHT)
		return -EINVAL;
	nd->vram_phys = res->start;
	nd->mc = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(nd->mc))
		return PTR_ERR(nd->mc);
	nd->dac = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(nd->dac))
		return PTR_ERR(nd->dac);

	nd->dram = devm_platform_ioremap_resource(pdev, 3);
	if (IS_ERR(nd->dram))
		return PTR_ERR(nd->dram);

	ret = nextdim_i860_start(nd);
	if (ret)
		drm_warn(dev, "the i860 didn't start (%d); drawing without it\n",
			 ret);
	nd->accel = !ret;
#ifdef CONFIG_NEXT
	if (nd->accel)
		next_nd_set_epsr(pdev->id,
				 nextdim_dram_read(nd, NEXTDIM_FW_EPSR));
#endif

	/* after the ROM, which may have touched them */
	nextdim_dac_init(nd);
	memset_io(nd->vram, 0, NEXTDIM_PITCH * NEXTDIM_HEIGHT);

	ret = nextdim_kms_init(nd);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, dev);

	ret = drm_dev_register(dev, 0);
	if (ret)
		return ret;

	drm_info(dev, "1120x832, 32-bit color, video memory at %pR%s\n", res,
		 nd->accel ? ", i860 console" : "");
	drm_client_setup_with_fourcc(dev, DRM_FORMAT_BGRX8888);

	return 0;
}

static void nextdim_remove(struct platform_device *pdev)
{
	struct drm_device *dev = platform_get_drvdata(pdev);

	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
	cancel_work_sync(&to_nextdim(dev)->redraw_work);
}

static void nextdim_shutdown(struct platform_device *pdev)
{
	drm_atomic_helper_shutdown(platform_get_drvdata(pdev));
}

static struct platform_driver nextdim_platform_driver = {
	.driver = {
		.name = "next-dimension",
	},
	.probe = nextdim_probe,
	.remove = nextdim_remove,
	.shutdown = nextdim_shutdown,
};
module_platform_driver(nextdim_platform_driver);

MODULE_DESCRIPTION("DRM driver for the NeXTdimension board");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:next-dimension");
