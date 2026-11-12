// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM driver for the built-in display of the NeXT computers
 *
 * All of them show a fixed 1120x832 image.  The monochrome machines have
 * four grey levels, two bits per pixel with the leftmost pixel in the most
 * significant bits; pixel value 0 is white and 3 is black.  The Color
 * machines have 16 bits per pixel, RGBA4444 in the CPU's byte order, and
 * the display ignores alpha.  The video memory has room for 1152 pixels per
 * line on the non-Turbo machines; the line length is the size of the memory
 * resource divided by the number of lines.  The firmware has set up the
 * display.  The only register is the Color machines' video control, which
 * blanks the display.
 *
 * Framebuffers are kept in system memory and copied to the video memory as
 * they change.  The native formats are indexed ones: DRM_FORMAT_C2 on
 * monochrome machines, whose four colours (the gamma LUT) are mapped to the
 * nearest grey level, and DRM_FORMAT_C4 on Color.  fbdev emulation uses
 * them: on Color, C4 takes a quarter of the memory of a 16-bit format and
 * is quicker to draw.  XRGB8888 in either byte order, and RGB565 in the
 * CPU's on Color, are converted.
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_color_mgmt.h>
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

#define NEXTDISPLAY_WIDTH	1120
#define NEXTDISPLAY_HEIGHT	832

/* Color video control register */
#define NEXTDISPLAY_CSR_CLEAR_INT	0x01
#define NEXTDISPLAY_CSR_UNBLANK		0x04

struct nextdisplay_model {
	unsigned int bpp;		/* of the video memory */
	unsigned int lut_size;		/* colours of the indexed format */
	u32 fbdev_format;
	const u32 *formats;
	unsigned int num_formats;
	const char *desc;
};

struct nextdisplay {
	struct drm_device dev;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_display_mode mode;

	const struct nextdisplay_model *model;
	void __iomem *vram;
	void __iomem *csr;		/* Color only, and not on Turbo */
	unsigned int pitch;		/* bytes per line of video memory */

	/*
	 * A byte of indexed pixels to the hardware's, through the gamma LUT:
	 * four C2 pixels to a byte, or two C4 pixels to a word.
	 */
	union {
		u8 c2_to_grey[256];
		u32 c4_to_color[256];
	};
};

static struct nextdisplay *to_nextdisplay(struct drm_device *dev)
{
	return container_of(dev, struct nextdisplay, dev);
}

/*
 * Pixel conversions
 */

/* 8-bit RGB to the hardware's darkness, 0 (white) to 3 (black) */
static u8 nextdisplay_rgb_to_grey(u32 r, u32 g, u32 b)
{
	u32 y = (r * 77 + g * 150 + b * 29) >> 8;

	/* the nearest of the four levels, without a division */
	return 3 - ((y * 3 + 128) >> 8);
}

/* 8-bit RGB to RGBA4444, alpha 0 */
static u32 nextdisplay_rgb_to_color(u32 r, u32 g, u32 b)
{
	return (r & 0xf0) << 8 | (g & 0xf0) << 4 | (b & 0xf0);
}

static void nextdisplay_load_lut(struct nextdisplay *nd,
				 const struct drm_color_lut *lut)
{
	unsigned int size = nd->model->lut_size;
	u32 hw[16], r, g, b;
	unsigned int i;

	for (i = 0; i < size; i++) {
		if (lut) {
			r = lut[i].red >> 8;
			g = lut[i].green >> 8;
			b = lut[i].blue >> 8;
		} else {
			/* a grey ramp from black */
			r = g = b = i * 255 / (size - 1);
		}
		if (nd->model->bpp == 2)
			hw[i] = nextdisplay_rgb_to_grey(r, g, b);
		else
			hw[i] = nextdisplay_rgb_to_color(r, g, b);
	}

	for (i = 0; i < 256; i++) {
		if (nd->model->bpp == 2)
			nd->c2_to_grey[i] = hw[i >> 6] << 6 |
					    hw[(i >> 4) & 3] << 4 |
					    hw[(i >> 2) & 3] << 2 | hw[i & 3];
		else
			nd->c4_to_color[i] = hw[i >> 4] << 16 | hw[i & 15];
	}
}

/*
 * The copies work in 32-bit words of video memory: 16 pixels on monochrome
 * machines, 2 on Color.  atomic_check keeps the framebuffer aligned to
 * them.
 */

static void nextdisplay_copy_c2(struct nextdisplay *nd, void __iomem *dst,
				const u8 *src, unsigned int words)
{
	const u8 *lut = nd->c2_to_grey;

	while (words--) {
		__raw_writel((u32)lut[src[0]] << 24 | lut[src[1]] << 16 |
			     lut[src[2]] << 8 | lut[src[3]], dst);
		src += 4;
		dst += 4;
	}
}

/* 32-bit RGB, with the red, green and blue bytes at offsets r, g and b */
static void nextdisplay_copy_rgb32_grey(void __iomem *dst, const u8 *src,
					unsigned int words, unsigned int r,
					unsigned int g, unsigned int b)
{
	unsigned int i;
	u32 val;

	while (words--) {
		val = 0;
		for (i = 0; i < 16; i++, src += 4)
			val = val << 2 | nextdisplay_rgb_to_grey(src[r], src[g],
								 src[b]);
		__raw_writel(val, dst);
		dst += 4;
	}
}

static void nextdisplay_copy_c4(struct nextdisplay *nd, void __iomem *dst,
				const u8 *src, unsigned int words)
{
	const u32 *lut = nd->c4_to_color;

	while (words--) {
		__raw_writel(lut[*src++], dst);
		dst += 4;
	}
}

/* RGB565 in the CPU's byte order, two pixels at a time */
static void nextdisplay_copy_rgb565(void __iomem *dst, const u16 *src,
				    unsigned int words)
{
	u32 val;

	while (words--) {
		val = (u32)src[0] << 16 | src[1];
		/* the top four bits of each component */
		__raw_writel((val & 0xf000f000) | (val << 1 & 0x0f000f00) |
			     (val << 3 & 0x00f000f0), dst);
		src += 2;
		dst += 4;
	}
}

static void nextdisplay_copy_rgb32_color(void __iomem *dst, const u8 *src,
					 unsigned int words, unsigned int r,
					 unsigned int g, unsigned int b)
{
	while (words--) {
		__raw_writel(nextdisplay_rgb_to_color(src[r], src[g], src[b]) << 16 |
			     nextdisplay_rgb_to_color(src[4 + r], src[4 + g],
						      src[4 + b]), dst);
		src += 8;
		dst += 4;
	}
}

static void nextdisplay_update(struct nextdisplay *nd,
			       const struct drm_framebuffer *fb,
			       const struct iosys_map *map,
			       const struct drm_rect *src_rect,
			       struct drm_rect clip)
{
	unsigned int bpp = nd->model->bpp;
	unsigned int ppw = 32 / bpp;	/* pixels per word */
	bool grey = bpp == 2;
	unsigned int x, words, y;
	void __iomem *dst;
	const u8 *src;

	/* clip is in framebuffer coordinates; round it out to whole words */
	clip.x1 = ALIGN_DOWN(clip.x1, ppw);
	clip.x2 = ALIGN(clip.x2, ppw);
	words = drm_rect_width(&clip) / ppw;

	x = clip.x1 - src_rect->x1;
	y = clip.y1 - src_rect->y1;
	dst = nd->vram + y * nd->pitch + x * bpp / 8;
	src = map->vaddr + clip.y1 * fb->pitches[0] +
	      clip.x1 * drm_format_info_bpp(fb->format, 0) / 8;

	for (y = clip.y1; y < clip.y2; y++) {
		switch (fb->format->format) {
		case DRM_FORMAT_C2:
			nextdisplay_copy_c2(nd, dst, src, words);
			break;
		case DRM_FORMAT_C4:
			nextdisplay_copy_c4(nd, dst, src, words);
			break;
		case DRM_FORMAT_HOST_RGB565:
			nextdisplay_copy_rgb565(dst, (const u16 *)src, words);
			break;
		case DRM_FORMAT_XRGB8888:	/* little-endian */
			if (grey)
				nextdisplay_copy_rgb32_grey(dst, src, words,
							    2, 1, 0);
			else
				nextdisplay_copy_rgb32_color(dst, src, words,
							     2, 1, 0);
			break;
		case DRM_FORMAT_BGRX8888:	/* XRGB8888, big-endian */
			if (grey)
				nextdisplay_copy_rgb32_grey(dst, src, words,
							    1, 2, 3);
			else
				nextdisplay_copy_rgb32_color(dst, src, words,
							     1, 2, 3);
			break;
		}
		src += fb->pitches[0];
		dst += nd->pitch;
	}
}

/*
 * Plane
 */

static const u32 nextdisplay_mono_formats[] = {
	DRM_FORMAT_C2,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_BGRX8888,
};

static const u32 nextdisplay_color_formats[] = {
	DRM_FORMAT_C4,
	DRM_FORMAT_HOST_RGB565,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_BGRX8888,
};

static int nextdisplay_plane_atomic_check(struct drm_plane *plane,
					  struct drm_atomic_commit *state)
{
	struct drm_plane_state *plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct nextdisplay *nd = to_nextdisplay(plane->dev);
	unsigned int ppw = 32 / nd->model->bpp;
	struct drm_crtc_state *crtc_state = NULL;
	int ret;

	if (plane_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state,
							   plane_state->crtc);

	ret = drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret || !plane_state->visible)
		return ret;

	/* the copies need whole words of the display */
	if ((plane_state->src.x1 >> 16) % ppw) {
		drm_dbg_kms(plane->dev, "x offset %u is not a multiple of %u\n",
			    plane_state->src.x1 >> 16, ppw);
		return -EINVAL;
	}

	return 0;
}

static void nextdisplay_plane_atomic_update(struct drm_plane *plane,
					    struct drm_atomic_commit *state)
{
	struct nextdisplay *nd = to_nextdisplay(plane->dev);
	struct drm_plane_state *plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_plane_state *old_plane_state =
		drm_atomic_get_old_plane_state(state, plane);
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(plane_state);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect src_rect, damage;
	int idx;

	if (!fb || !plane_state->visible)
		return;

	if (!drm_dev_enter(&nd->dev, &idx))
		return;

	drm_rect_fp_to_int(&src_rect, &plane_state->src);

	crtc_state = drm_atomic_get_new_crtc_state(state, plane_state->crtc);
	if (crtc_state->color_mgmt_changed) {
		/* new colours: redraw everything */
		nextdisplay_load_lut(nd, crtc_state->gamma_lut ?
					 crtc_state->gamma_lut->data : NULL);
		nextdisplay_update(nd, fb, shadow_plane_state->data,
				   &src_rect, src_rect);
	} else {
		drm_atomic_helper_damage_iter_init(&iter, old_plane_state,
						   plane_state);
		drm_atomic_for_each_plane_damage(&iter, &damage)
			nextdisplay_update(nd, fb, shadow_plane_state->data,
					   &src_rect, damage);
	}

	drm_dev_exit(idx);
}

static void nextdisplay_plane_atomic_disable(struct drm_plane *plane,
					     struct drm_atomic_commit *state)
{
	struct nextdisplay *nd = to_nextdisplay(plane->dev);
	int idx;

	if (!drm_dev_enter(&nd->dev, &idx))
		return;

	/* white on monochrome machines, black on Color */
	memset_io(nd->vram, 0, nd->pitch * NEXTDISPLAY_HEIGHT);

	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs nextdisplay_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = nextdisplay_plane_atomic_check,
	.atomic_update = nextdisplay_plane_atomic_update,
	.atomic_disable = nextdisplay_plane_atomic_disable,
};

static const struct drm_plane_funcs nextdisplay_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

/*
 * CRTC
 */

static enum drm_mode_status
nextdisplay_crtc_mode_valid(struct drm_crtc *crtc,
			    const struct drm_display_mode *mode)
{
	struct nextdisplay *nd = to_nextdisplay(crtc->dev);

	return drm_crtc_helper_mode_valid_fixed(crtc, mode, &nd->mode);
}

static int nextdisplay_crtc_atomic_check(struct drm_crtc *crtc,
					 struct drm_atomic_commit *state)
{
	struct nextdisplay *nd = to_nextdisplay(crtc->dev);
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);

	if (crtc_state->gamma_lut &&
	    drm_color_lut_size(crtc_state->gamma_lut) != nd->model->lut_size) {
		drm_dbg_kms(crtc->dev, "gamma LUT size %d is not %u\n",
			    drm_color_lut_size(crtc_state->gamma_lut),
			    nd->model->lut_size);
		return -EINVAL;
	}

	if (!crtc_state->enable)
		return 0;

	return drm_atomic_helper_check_crtc_primary_plane(crtc_state);
}

/* Color only: blank or unblank, and clear the vblank interrupt */
static void nextdisplay_write_csr(struct nextdisplay *nd, u8 val)
{
	int idx;

	if (!nd->csr || !drm_dev_enter(&nd->dev, &idx))
		return;
	writeb(val | NEXTDISPLAY_CSR_CLEAR_INT, nd->csr);
	drm_dev_exit(idx);
}

static void nextdisplay_crtc_atomic_enable(struct drm_crtc *crtc,
					   struct drm_atomic_commit *state)
{
	nextdisplay_write_csr(to_nextdisplay(crtc->dev),
			      NEXTDISPLAY_CSR_UNBLANK);
	drm_crtc_vblank_on(crtc);
}

static void nextdisplay_crtc_atomic_disable(struct drm_crtc *crtc,
					    struct drm_atomic_commit *state)
{
	drm_crtc_vblank_off(crtc);
	nextdisplay_write_csr(to_nextdisplay(crtc->dev), 0);
}

static const struct drm_crtc_helper_funcs nextdisplay_crtc_helper_funcs = {
	.mode_valid = nextdisplay_crtc_mode_valid,
	.atomic_check = nextdisplay_crtc_atomic_check,
	.atomic_flush = drm_crtc_vblank_atomic_flush,
	.atomic_enable = nextdisplay_crtc_atomic_enable,
	.atomic_disable = nextdisplay_crtc_atomic_disable,
};

static const struct drm_crtc_funcs nextdisplay_crtc_funcs = {
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

static const struct drm_encoder_funcs nextdisplay_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int nextdisplay_connector_get_modes(struct drm_connector *connector)
{
	struct nextdisplay *nd = to_nextdisplay(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, &nd->mode);
}

static const struct drm_connector_helper_funcs
nextdisplay_connector_helper_funcs = {
	.get_modes = nextdisplay_connector_get_modes,
};

static const struct drm_connector_funcs nextdisplay_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static int nextdisplay_atomic_check(struct drm_device *dev,
				    struct drm_atomic_commit *state)
{
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	int i, ret;

	/*
	 * The palette is applied while copying the plane, so a new one has to
	 * redraw it, even in a commit that only changes the gamma LUT.
	 */
	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		if (!crtc_state->color_mgmt_changed)
			continue;
		ret = drm_atomic_add_affected_planes(state, crtc);
		if (ret)
			return ret;
	}

	return drm_atomic_helper_check(dev, state);
}

static const struct drm_mode_config_funcs nextdisplay_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = nextdisplay_atomic_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/*
 * Driver
 */

DEFINE_DRM_GEM_FOPS(nextdisplay_fops);

static const struct drm_driver nextdisplay_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &nextdisplay_fops,
	.name			= "nextdisplay",
	.desc			= "NeXT display",
	.major			= 1,
	.minor			= 0,
	DRM_GEM_SHMEM_DRIVER_OPS,
	DRM_FBDEV_SHMEM_DRIVER_OPS,
};

static void nextdisplay_iounmap(void *vram)
{
	iounmap(vram);
}

static int nextdisplay_kms_init(struct nextdisplay *nd)
{
	struct drm_device *dev = &nd->dev;
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	dev->mode_config.min_width = NEXTDISPLAY_WIDTH;
	dev->mode_config.max_width = NEXTDISPLAY_WIDTH;
	dev->mode_config.min_height = NEXTDISPLAY_HEIGHT;
	dev->mode_config.max_height = NEXTDISPLAY_HEIGHT;
	dev->mode_config.funcs = &nextdisplay_mode_config_funcs;
	/*
	 * legacy ADDFB gets formats in the CPU's byte order: XRGB8888 is
	 * BGRX8888, and RGB565 is DRM_FORMAT_HOST_RGB565
	 */
	dev->mode_config.quirk_addfb_prefer_host_byte_order = true;

	ret = drm_universal_plane_init(dev, &nd->plane, 0,
				       &nextdisplay_plane_funcs,
				       nd->model->formats,
				       nd->model->num_formats, NULL,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_helper_add(&nd->plane, &nextdisplay_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&nd->plane);

	ret = drm_crtc_init_with_planes(dev, &nd->crtc, &nd->plane, NULL,
					&nextdisplay_crtc_funcs, NULL);
	if (ret)
		return ret;
	drm_crtc_helper_add(&nd->crtc, &nextdisplay_crtc_helper_funcs);

	ret = drm_mode_crtc_set_gamma_size(&nd->crtc, nd->model->lut_size);
	if (ret)
		return ret;
	drm_crtc_enable_color_mgmt(&nd->crtc, 0, false, nd->model->lut_size);

	ret = drm_encoder_init(dev, &nd->encoder, &nextdisplay_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;
	nd->encoder.possible_crtcs = drm_crtc_mask(&nd->crtc);

	ret = drm_connector_init(dev, &nd->connector,
				 &nextdisplay_connector_funcs,
				 DRM_MODE_CONNECTOR_Unknown);
	if (ret)
		return ret;
	drm_connector_helper_add(&nd->connector,
				 &nextdisplay_connector_helper_funcs);
	ret = drm_connector_attach_encoder(&nd->connector, &nd->encoder);
	if (ret)
		return ret;

	ret = drm_vblank_init(dev, 1);
	if (ret)
		return ret;

	drm_mode_config_reset(dev);

	return 0;
}

static const struct nextdisplay_model nextdisplay_mono = {
	.bpp = 2,
	.lut_size = 4,
	.fbdev_format = DRM_FORMAT_C2,
	.formats = nextdisplay_mono_formats,
	.num_formats = ARRAY_SIZE(nextdisplay_mono_formats),
	.desc = "2-bit grayscale",
};

static const struct nextdisplay_model nextdisplay_color = {
	.bpp = 16,
	.lut_size = 16,
	.fbdev_format = DRM_FORMAT_C4,
	.formats = nextdisplay_color_formats,
	.num_formats = ARRAY_SIZE(nextdisplay_color_formats),
	.desc = "12-bit color",
};

static int nextdisplay_probe(struct platform_device *pdev)
{
	/* the MegaPixel Display: 92 dpi, 68 Hz */
	static const struct drm_display_mode mode = {
		DRM_MODE_INIT(68, NEXTDISPLAY_WIDTH, NEXTDISPLAY_HEIGHT,
			      310, 230)
	};
	struct nextdisplay *nd;
	struct drm_device *dev;
	struct resource *res;
	int ret;

	nd = devm_drm_dev_alloc(&pdev->dev, &nextdisplay_driver,
				struct nextdisplay, dev);
	if (IS_ERR(nd))
		return PTR_ERR(nd);
	dev = &nd->dev;
	nd->mode = mode;
	nd->model = (const struct nextdisplay_model *)
		platform_get_device_id(pdev)->driver_data;
	nextdisplay_load_lut(nd, NULL);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	nd->pitch = resource_size(res) / NEXTDISPLAY_HEIGHT;
	if (nd->pitch < NEXTDISPLAY_WIDTH * nd->model->bpp / 8) {
		drm_err(dev, "video memory of %pR is too small\n", res);
		return -EINVAL;
	}

	if (!devm_request_mem_region(&pdev->dev, res->start,
				     resource_size(res), pdev->name))
		return -EBUSY;
	/* write-through: the CPU never reads it back */
	nd->vram = ioremap_wt(res->start, resource_size(res));
	if (!nd->vram)
		return -ENOMEM;
	ret = devm_add_action_or_reset(&pdev->dev, nextdisplay_iounmap,
				       (void __force *)nd->vram);
	if (ret)
		return ret;

	if (platform_get_resource(pdev, IORESOURCE_MEM, 1)) {
		nd->csr = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(nd->csr))
			return PTR_ERR(nd->csr);
	}

	ret = nextdisplay_kms_init(nd);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, dev);

	ret = drm_dev_register(dev, 0);
	if (ret)
		return ret;

	drm_info(dev, "1120x832, %s, video memory at %pR\n", nd->model->desc,
		 res);
	drm_client_setup_with_fourcc(dev, nd->model->fbdev_format);

	return 0;
}

static void nextdisplay_remove(struct platform_device *pdev)
{
	struct drm_device *dev = platform_get_drvdata(pdev);

	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
}

static void nextdisplay_shutdown(struct platform_device *pdev)
{
	drm_atomic_helper_shutdown(platform_get_drvdata(pdev));
}

static const struct platform_device_id nextdisplay_ids[] = {
	{ "next-display", (kernel_ulong_t)&nextdisplay_mono },
	{ "next-color-display", (kernel_ulong_t)&nextdisplay_color },
	{ }
};
MODULE_DEVICE_TABLE(platform, nextdisplay_ids);

static struct platform_driver nextdisplay_platform_driver = {
	.driver = {
		.name = "next-display",
	},
	.id_table = nextdisplay_ids,
	.probe = nextdisplay_probe,
	.remove = nextdisplay_remove,
	.shutdown = nextdisplay_shutdown,
};
module_platform_driver(nextdisplay_platform_driver);

MODULE_DESCRIPTION("DRM driver for the NeXT display");
MODULE_LICENSE("GPL");
