/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_M68K_KBDLEDS_H
#define _ASM_M68K_KBDLEDS_H

#include <asm/setup.h>

/*
 * NeXT keyboards have no NumLock key, and their keypad is for numbers,
 * so start with NumLock on.
 */
static inline int kbd_defleds(void)
{
	if (IS_ENABLED(CONFIG_KEYBOARD_NEXT) && MACH_IS_NEXT)
		return 1 << VC_NUMLOCK;
	return 0;
}

#endif /* _ASM_M68K_KBDLEDS_H */
