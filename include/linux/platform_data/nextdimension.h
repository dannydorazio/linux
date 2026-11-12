/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __PLATFORM_DATA_NEXTDIMENSION_H
#define __PLATFORM_DATA_NEXTDIMENSION_H

#include <linux/types.h>

/*
 * The NeXTdimension's DRAM: four banks, each at the start of its own 16 MB
 * of the board's space and holding 4 or 16 MB, or nothing.
 */
#define NEXTDIM_DRAM_BANKS	4
#define NEXTDIM_DRAM_BANK_SIZE	0x01000000

struct nextdim_platform_data {
	u8 bank_mb[NEXTDIM_DRAM_BANKS];		/* 0, 4 or 16 */
};

#endif
