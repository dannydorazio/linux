/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * asm/bootinfo-next.h -- NeXT-specific boot information definitions
 */

#ifndef _UAPI_ASM_M68K_BOOTINFO_NEXT_H
#define _UAPI_ASM_M68K_BOOTINFO_NEXT_H


    /*
     *  NeXT-specific tags
     */

#define BI_NEXT_MODEL		0x8000	/* ROM machine type (__be32) */
#define BI_NEXT_BOARD_REV	0x8001	/* ROM board revision (__be32) */
#define BI_NEXT_ETHER_ADDR	0x8002	/* ethernet address (6 bytes) */
#define BI_NEXT_ROM_MG		0x8003	/* phys. addr of ROM monitor globals (__be32) */
#define BI_NEXT_ROM_VBR		0x8004	/* ROM vector base (__be32) */


    /*
     *  NeXT machine types (BI_NEXT_MODEL), as reported by the ROM
     */

#define NEXT_MODEL_CUBE		0	/* 25MHz 68030 NeXT Computer/NeXTcube */
#define NEXT_MODEL_WARP9	1	/* 25MHz 68040 NeXTstation */
#define NEXT_MODEL_X15		2	/* 25MHz 68040 NeXTcube */
#define NEXT_MODEL_WARP9C	3	/* 25MHz 68040 NeXTstation Color */
#define NEXT_MODEL_TURBO_MONO	4	/* 33MHz 68040 NeXTstation Turbo */
#define NEXT_MODEL_TURBO_COLOR	5	/* 33MHz 68040 NeXTstation Turbo Color */
#define NEXT_MODEL_CUBE_TURBO	8	/* 33MHz 68040 NeXTcube Turbo */


    /*
     *  Latest NeXT bootinfo version
     */

#define NEXT_BOOTI_VERSION	MK_BI_VERSION(1, 0)


#endif /* _UAPI_ASM_M68K_BOOTINFO_NEXT_H */
