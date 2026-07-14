/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *   Copyright (C) 2021 LG Electronics.
 *
 *   Author(s): Hyunchul Lee <hyc.lee@gmail.com>
 */
#ifndef _EXFAT_FS_H_
#define _EXFAT_FS_H_

#include <sys/types.h>
#include "list.h"

struct exfat_dentry;

struct exfat_inode {
	struct exfat_inode	*parent;
	struct list_head	children;
	struct list_head	sibling;
	struct list_head	list;
	clus_t			first_clus;
	__u16			attr;
	uint64_t		size;
	bool			is_contiguous;
	struct exfat_dentry	*dentry_set;
	int			dentry_count;
	off_t			dev_offset;
	__le16			name[0];	/* only for directory */
};

#define EXFAT_NAME_MAX			255
#define NAME_BUFFER_SIZE		((EXFAT_NAME_MAX + 1) * 2)

struct exfat {
	struct exfat_blk_dev	*blk_dev;
	struct pbr		*bs;
	char			volume_label[VOLUME_LABEL_BUFFER_SIZE];
	struct exfat_inode	*root;
	struct list_head	dir_list;
	clus_t			clus_count;
	unsigned int		clus_size;
	unsigned int		sect_size;
	/* cached `sysconf(_SC_PAGE_SIZE)` */
	long			pagesize;
	/* Actual start address of memory allocated for `disk_bitmap` */
	void			*disk_bitmap_m;
	/* Actual size of `disk_bitmap_m` */
	size_t			disk_bitmap_msize;
	/*
	 * Offset to the allocation bitmap on disk(for calloc() and read/write()
	 * method)
	 */
	off_t			disk_bitmap_devofs;
	/*
	 * The allocation bitmap as stored on disk.
	 *
	 * Can be either a mmap()'d region of the block device or good-old
	 * calloc()'d and read()'d from disk.
	 */
	unsigned char		*disk_bitmap;
	/*
	 * Ephemeral bitmap allocation for authoring a new allocation bitmap or
	 * repairing the existing one.
	 *
	 * Can be either file-backed mapping or good-old calloc() memory.
	 */
	unsigned char		*alloc_bitmap;
	/*
	 * The byte size of allocation bitmap(both `disk_bitmap` and
	 * `alloc_bitmap`) in multiple of sizeof(bitmap_t). The valid length
	 * could be less than this.
	 *
	 * Unfortunately, 'bitmap_size' is a macro define, hence the "bm_"
	 * prefix.
	 */
	size_t			bm_size;
	/*
	 * The exact valid byte length of allocation bitmap.
	 *
	 * The size actually allocated may be much more than this and even
	 * `bm_size` due to the page size alignment requirement of mmap().
	 */
	size_t			bm_len;
	clus_t			disk_bitmap_clus;
	__u16			*upcase_table;
	clus_t			start_clu;
	unsigned int		buffer_count;
	struct buffer_desc	*lookup_buffer; /* for dentry set lookup */

	/*
	 * Callback after changing the contents of `disk_bitmap`.
	 *
	 * If `disk_bitmap` is regular calloc()'d memory and its contents read()
	 * from the disk, write() will be done in the function. If the address
	 * is mmap()'d, this is NULL because write() is handled in kernel.
	 */
	bool (*invalidate_disk_bitmap)(struct exfat *exfat, size_t ofs, size_t len);
	/*
	 * Callback for requesting sync of `disk_bitmap`.
	 *
	 * Currently, this is only set for mmap()'d `disk_bitmap` to msync()
	 * the dirty pages. It's NULL otherwise because fsync() is issued
	 * elsewhere prior to process exit.
	 */
	bool (*sync_disk_bitmap)(struct exfat *exfat);
	/* Callback to free up `disk_bitmap`(free() or munmap()) */
	void (*free_disk_bitmap)(struct exfat *exfat);
	/* Callback to free up `alloc_bitmap`(free() or munmap()) */
	void (*free_alloc_bitmap)(struct exfat *exfat);

	/*
	 * The pid of the sentry process
	 *
	 * See SIGBUS Sentry Facilities for more.
	 */
	pid_t sentry_pid;
	/*
	 * The write end of the sentry pipe.
	 *
	 * See SIGBUS Sentry Facilities for more.
	 */
	int sentry_pipe;
};

struct exfat_dentry_loc {
	struct exfat_inode	*parent;
	off_t			file_offset;
	off_t			dev_offset;
};

struct path_resolve_ctx {
	struct exfat_inode	*ancestors[255];
	__le16			utf16_path[PATH_MAX + 2];
	char			local_path[PATH_MAX * MB_LEN_MAX + 1];
};

struct buffer_desc {
	__u32		p_clus;
	unsigned int	offset;
	char		*buffer;
	char		dirty[EXFAT_BITMAP_SIZE(4 * KB / 512)];
};

struct exfat *exfat_alloc_exfat(struct exfat_blk_dev *blk_dev, struct pbr *bs,
		struct exfat_inode *root, const bool need_amap);
void exfat_free_exfat(struct exfat *exfat);

struct exfat_inode *exfat_alloc_inode(__u16 attr);
void exfat_free_inode(struct exfat_inode *node);

void exfat_free_children(struct exfat_inode *dir, bool file_only);
void exfat_free_file_children(struct exfat_inode *dir);
void exfat_free_ancestors(struct exfat_inode *child);
void exfat_free_dir_list(struct exfat *exfat);

int exfat_resolve_path(struct path_resolve_ctx *ctx, struct exfat_inode *child);
int exfat_resolve_path_parent(struct path_resolve_ctx *ctx,
			      struct exfat_inode *parent, struct exfat_inode *child);

struct buffer_desc *exfat_alloc_buffer(struct exfat *exfat);
void exfat_free_buffer(const struct exfat *exfat, struct buffer_desc *bd);

bool exfat_load_disk_bitmap(struct exfat *exfat, const off_t loc, const bool rw);

static inline unsigned int exfat_get_read_size(const struct exfat *exfat)
{
	return MIN(exfat->clus_size, 4 * KB);
}
#endif
