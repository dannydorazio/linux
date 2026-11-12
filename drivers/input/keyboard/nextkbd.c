// SPDX-License-Identifier: GPL-2.0
/*
 * NeXT keyboard and mouse
 *
 * The keyboard, and the mouse plugged into it, sit on a serial bus behind
 * the sound and keyboard/mouse interface in the display or Soundbox.  The
 * interface polls them by itself and interrupts with each event: a 32-bit
 * word with the device address on top and the device's data below.
 *
 * The keyboard sends the state of all modifier keys with every event, and a
 * change of modifiers alone as an event without a key.  The mouse sends
 * 7-bit movement counts and the state of its two buttons.
 *
 * Neither repeats keys or accelerates by itself; the input core repeats.
 */

#include <linux/bits.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define NEXTKBD_KM_STATUS	1	/* keyboard/mouse status and control */
#define  NEXTKBD_KM_RECEIVED	0x40	/* an event is waiting */
#define  NEXTKBD_KM_OVERRUN	0x20	/* one was lost; write to clear */
#define NEXTKBD_KM_DATA		8	/* reading it takes the event */

#define NEXTKBD_NO_RESPONSE	BIT(30)	/* a device didn't answer */
#define NEXTKBD_USER_POLL	BIT(29)	/* the answer to a command */
#define NEXTKBD_MOUSE		BIT(24)	/* from the mouse, not the keyboard */

#define NEXTKBD_MOD_SHIFT	8	/* modifier bits, from control up */
#define NEXTKBD_NUM_MODS	7
#define NEXTKBD_KEY_UP		0x80
#define NEXTKBD_KEY_MASK	0x7f

#define NEXTKBD_MOUSE_Y_SHIFT	9	/* counts, negative right and down */
#define NEXTKBD_MOUSE_RIGHT_UP	BIT(8)
#define NEXTKBD_MOUSE_X_SHIFT	1
#define NEXTKBD_MOUSE_LEFT_UP	BIT(0)

static const unsigned short nextkbd_modifiers[NEXTKBD_NUM_MODS] = {
	KEY_LEFTCTRL,		/* the only Control key */
	KEY_LEFTSHIFT,
	KEY_RIGHTSHIFT,
	KEY_LEFTMETA,		/* Command */
	KEY_RIGHTMETA,
	KEY_LEFTALT,		/* Alternate */
	KEY_RIGHTALT,
};

static const unsigned short nextkbd_keycodes[NEXTKBD_KEY_MASK + 1] = {
	[0x01] = KEY_BRIGHTNESSDOWN,
	[0x02] = KEY_VOLUMEDOWN,
	[0x03] = KEY_BACKSLASH,
	[0x04] = KEY_RIGHTBRACE,
	[0x05] = KEY_LEFTBRACE,
	[0x06] = KEY_I,
	[0x07] = KEY_O,
	[0x08] = KEY_P,
	[0x09] = KEY_LEFT,
	[0x0b] = KEY_KP0,
	[0x0c] = KEY_KPDOT,
	[0x0d] = KEY_KPENTER,
	[0x0f] = KEY_DOWN,
	[0x10] = KEY_RIGHT,
	[0x11] = KEY_KP1,
	[0x12] = KEY_KP4,
	[0x13] = KEY_KP6,
	[0x14] = KEY_KP3,
	[0x15] = KEY_KPPLUS,
	[0x16] = KEY_UP,
	[0x17] = KEY_KP2,
	[0x18] = KEY_KP5,
	[0x19] = KEY_BRIGHTNESSUP,
	[0x1a] = KEY_VOLUMEUP,
	[0x1b] = KEY_BACKSPACE,		/* Delete */
	[0x1c] = KEY_EQUAL,
	[0x1d] = KEY_MINUS,
	[0x1e] = KEY_8,
	[0x1f] = KEY_9,
	[0x20] = KEY_0,
	[0x21] = KEY_KP7,
	[0x22] = KEY_KP8,
	[0x23] = KEY_KP9,
	[0x24] = KEY_KPMINUS,
	[0x25] = KEY_KPASTERISK,
	[0x26] = KEY_GRAVE,
	[0x27] = KEY_KPEQUAL,
	[0x28] = KEY_KPSLASH,
	[0x2a] = KEY_ENTER,		/* Return */
	[0x2b] = KEY_APOSTROPHE,
	[0x2c] = KEY_SEMICOLON,
	[0x2d] = KEY_L,
	[0x2e] = KEY_COMMA,
	[0x2f] = KEY_DOT,
	[0x30] = KEY_SLASH,
	[0x31] = KEY_Z,
	[0x32] = KEY_X,
	[0x33] = KEY_C,
	[0x34] = KEY_V,
	[0x35] = KEY_B,
	[0x36] = KEY_M,
	[0x37] = KEY_N,
	[0x38] = KEY_SPACE,
	[0x39] = KEY_A,
	[0x3a] = KEY_S,
	[0x3b] = KEY_D,
	[0x3c] = KEY_F,
	[0x3d] = KEY_G,
	[0x3e] = KEY_K,
	[0x3f] = KEY_J,
	[0x40] = KEY_H,
	[0x41] = KEY_TAB,
	[0x42] = KEY_Q,
	[0x43] = KEY_W,
	[0x44] = KEY_E,
	[0x45] = KEY_R,
	[0x46] = KEY_U,
	[0x47] = KEY_Y,
	[0x48] = KEY_T,
	[0x49] = KEY_ESC,
	[0x4a] = KEY_1,
	[0x4b] = KEY_2,
	[0x4c] = KEY_3,
	[0x4d] = KEY_4,
	[0x4e] = KEY_7,
	[0x4f] = KEY_6,
	[0x50] = KEY_5,
	[0x58] = KEY_POWER,
};

struct nextkbd {
	void __iomem *regs;
	struct input_dev *kbd;
	struct input_dev *mouse;
	unsigned short keycodes[ARRAY_SIZE(nextkbd_keycodes)];
	unsigned int mods;
};

/* After a lost event, let go of every key so none is left repeating. */
static void nextkbd_release_keys(struct nextkbd *nk)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(nk->keycodes); i++)
		if (test_bit(nk->keycodes[i], nk->kbd->key))
			input_report_key(nk->kbd, nk->keycodes[i], 0);
	input_sync(nk->kbd);
}

static void nextkbd_key_event(struct nextkbd *nk, u32 data)
{
	unsigned int mods = (data >> NEXTKBD_MOD_SHIFT) &
			    GENMASK(NEXTKBD_NUM_MODS - 1, 0);
	unsigned int scancode = data & NEXTKBD_KEY_MASK;
	unsigned long changed = mods ^ nk->mods;
	unsigned int i;

	for_each_set_bit(i, &changed, NEXTKBD_NUM_MODS)
		input_report_key(nk->kbd, nextkbd_modifiers[i],
				 mods & BIT(i));
	nk->mods = mods;

	if (scancode) {
		input_event(nk->kbd, EV_MSC, MSC_SCAN, scancode);
		input_report_key(nk->kbd, nk->keycodes[scancode],
				 !(data & NEXTKBD_KEY_UP));
	}
	input_sync(nk->kbd);
}

/* A 7-bit two's complement count, which runs the other way */
static int nextkbd_mouse_delta(u32 data, unsigned int shift)
{
	return -sign_extend32(data >> shift, 6);
}

static void nextkbd_mouse_event(struct nextkbd *nk, u32 data)
{
	input_report_rel(nk->mouse, REL_X,
			 nextkbd_mouse_delta(data, NEXTKBD_MOUSE_X_SHIFT));
	input_report_rel(nk->mouse, REL_Y,
			 nextkbd_mouse_delta(data, NEXTKBD_MOUSE_Y_SHIFT));
	input_report_key(nk->mouse, BTN_LEFT,
			 !(data & NEXTKBD_MOUSE_LEFT_UP));
	input_report_key(nk->mouse, BTN_RIGHT,
			 !(data & NEXTKBD_MOUSE_RIGHT_UP));
	input_sync(nk->mouse);
}

static irqreturn_t nextkbd_interrupt(int irq, void *dev_id)
{
	struct nextkbd *nk = dev_id;
	u8 status = readb(nk->regs + NEXTKBD_KM_STATUS);

	if (!(status & NEXTKBD_KM_RECEIVED))
		return IRQ_NONE;

	do {
		u32 data = ioread32be(nk->regs + NEXTKBD_KM_DATA);

		if (status & NEXTKBD_KM_OVERRUN) {
			writeb(NEXTKBD_KM_OVERRUN,
			       nk->regs + NEXTKBD_KM_STATUS);
			nextkbd_release_keys(nk);
		}

		/* Answers to commands are not input. */
		if (!(data & (NEXTKBD_NO_RESPONSE | NEXTKBD_USER_POLL))) {
			if (data & NEXTKBD_MOUSE)
				nextkbd_mouse_event(nk, data);
			else
				nextkbd_key_event(nk, data);
		}

		status = readb(nk->regs + NEXTKBD_KM_STATUS);
	} while (status & NEXTKBD_KM_RECEIVED);

	return IRQ_HANDLED;
}

static int nextkbd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nextkbd *nk;
	unsigned int i;
	int irq, err;

	nk = devm_kzalloc(dev, sizeof(*nk), GFP_KERNEL);
	if (!nk)
		return -ENOMEM;

	nk->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(nk->regs))
		return PTR_ERR(nk->regs);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	nk->kbd = devm_input_allocate_device(dev);
	nk->mouse = devm_input_allocate_device(dev);
	if (!nk->kbd || !nk->mouse)
		return -ENOMEM;

	nk->kbd->name = "NeXT keyboard";
	nk->kbd->phys = "next/input0";
	nk->kbd->id.bustype = BUS_HOST;
	memcpy(nk->keycodes, nextkbd_keycodes, sizeof(nk->keycodes));
	nk->kbd->keycode = nk->keycodes;
	nk->kbd->keycodesize = sizeof(nk->keycodes[0]);
	nk->kbd->keycodemax = ARRAY_SIZE(nk->keycodes);
	__set_bit(EV_KEY, nk->kbd->evbit);
	__set_bit(EV_REP, nk->kbd->evbit);
	input_set_capability(nk->kbd, EV_MSC, MSC_SCAN);
	for (i = 0; i < ARRAY_SIZE(nk->keycodes); i++)
		__set_bit(nk->keycodes[i], nk->kbd->keybit);
	for (i = 0; i < ARRAY_SIZE(nextkbd_modifiers); i++)
		__set_bit(nextkbd_modifiers[i], nk->kbd->keybit);
	__clear_bit(KEY_RESERVED, nk->kbd->keybit);

	nk->mouse->name = "NeXT mouse";
	nk->mouse->phys = "next/input1";
	nk->mouse->id.bustype = BUS_HOST;
	input_set_capability(nk->mouse, EV_REL, REL_X);
	input_set_capability(nk->mouse, EV_REL, REL_Y);
	input_set_capability(nk->mouse, EV_KEY, BTN_LEFT);
	input_set_capability(nk->mouse, EV_KEY, BTN_RIGHT);

	err = input_register_device(nk->kbd);
	if (err)
		return err;
	err = input_register_device(nk->mouse);
	if (err)
		return err;

	return devm_request_irq(dev, irq, nextkbd_interrupt, 0, "next-kbd", nk);
}

static struct platform_driver nextkbd_driver = {
	.probe = nextkbd_probe,
	.driver = {
		.name = "next-kbd",
	},
};
module_platform_driver(nextkbd_driver);

MODULE_DESCRIPTION("NeXT keyboard and mouse");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:next-kbd");
