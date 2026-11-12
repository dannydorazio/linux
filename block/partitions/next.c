// SPDX-License-Identifier: GPL-2.0
/*
 * NeXT disk label support
 *
 * NeXTSTEP and OPENSTEP on the m68k NeXT computers put a big-endian disk
 * label at the start of the disk.  It comes in three versions, all with
 * up to eight partitions.  Partition offsets are counted in the label's
 * sector size, from the end of the "front porch" that holds the label and
 * the boot program.
 */

#include <linux/slab.h>
#include <linux/unaligned.h>

#include "check.h"

#define NEXT_LABEL_V1		0x4e655854	/* "NeXT" */
#define NEXT_LABEL_V2		0x646c5632	/* "dlV2" */
#define NEXT_LABEL_V3		0x646c5633	/* "dlV3" */
#define NEXT_MAXPARTITIONS	8
#define NEXT_NBAD		1670

struct next_partition {
	__be32	offset;			/* in label sectors, after the front porch */
	__be32	size;			/* in label sectors; < 1: unused */
	__be16	bsize;
	__be16	fsize;
	u8	opt;
	u8	pad1;
	__be16	cpg;
	__be16	density;
	u8	minfree;
	u8	newfs;
	char	mountpt[16];
	u8	automnt;
	char	type[8];		/* file system type name */
	u8	pad2;
} __packed;

struct next_disklabel {
	__be32	version;
	__be32	label_blkno;
	__be32	size;
	char	label[24];		/* disk name */
	__be32	flags;
	__be32	tag;
	char	name[24];		/* drive name */
	char	type[24];		/* drive type */
	__be32	secsize;		/* bytes per label sector */
	__be32	ntracks;
	__be32	nsectors;
	__be32	ncylinders;
	__be32	rpm;
	__be16	front;			/* sectors in the front porch */
	__be16	back;
	__be16	ngroups;
	__be16	ag_size;
	__be16	ag_alts;
	__be16	ag_off;
	__be32	boot_blkno[2];
	char	kernel[24];
	char	hostname[32];
	char	rootpartition;
	char	rwpartition;
	struct next_partition partitions[NEXT_MAXPARTITIONS];
	union {
		__be16	v3_checksum;	/* version 3 */
		__be32	bad[NEXT_NBAD];	/* versions 1 and 2: bad blocks */
	} un;
	__be16	checksum;		/* versions 1 and 2 */
} __packed;

static_assert(sizeof(struct next_partition) == 46);
static_assert(sizeof(struct next_disklabel) == 7240);

/* Sum of the big-endian 16-bit words in front of the checksum field */
static u16 next_checksum(const u8 *buf, const u8 *limit)
{
	u32 sum = 0;

	for (; buf < limit; buf += 2)
		sum += (buf[0] << 8) | buf[1];
	sum += sum >> 16;
	return sum;
}

int next_partition(struct parsed_partitions *state)
{
	unsigned int nsectors = DIV_ROUND_UP(sizeof(struct next_disklabel), 512);
	struct next_disklabel *label;
	const __be16 *checksum;
	unsigned int i, factor;
	u32 version, secsize;
	int ret = 0;
	Sector sect;
	u8 *data;

	data = read_part_sector(state, 0, &sect);
	if (!data)
		return -1;
	version = get_unaligned_be32(data);
	put_dev_sector(sect);
	if (version != NEXT_LABEL_V1 && version != NEXT_LABEL_V2 &&
	    version != NEXT_LABEL_V3)
		return 0;

	label = kmalloc(nsectors * 512, GFP_KERNEL);
	if (!label)
		return -1;
	for (i = 0; i < nsectors; i++) {
		data = read_part_sector(state, i, &sect);
		if (!data) {
			ret = -1;
			goto out;
		}
		memcpy((u8 *)label + i * 512, data, 512);
		put_dev_sector(sect);
	}

	checksum = version == NEXT_LABEL_V3 ? &label->un.v3_checksum
					    : &label->checksum;
	if (next_checksum((u8 *)label, (u8 *)checksum) !=
	    be16_to_cpu(*checksum))
		goto out;

	secsize = be32_to_cpu(label->secsize);
	if (secsize < 512 || secsize % 512)
		goto out;
	factor = secsize / 512;

	seq_buf_printf(&state->pp_buf, " [NeXT %.24s]", label->label);
	for (i = 0; i < NEXT_MAXPARTITIONS && i + 1 < state->limit; i++) {
		const struct next_partition *p = &label->partitions[i];
		s32 offset = be32_to_cpu(p->offset);
		s32 size = be32_to_cpu(p->size);

		if (size <= 0 || offset < 0)
			continue;
		put_partition(state, i + 1,
			      ((sector_t)be16_to_cpu(label->front) + offset) * factor,
			      (sector_t)size * factor);
	}
	seq_buf_puts(&state->pp_buf, "\n");
	ret = 1;
out:
	kfree(label);
	return ret;
}
