// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2021 LG Electronics.
 *
 *   Author(s): Hyunchul Lee <hyc.lee@gmail.com>
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef _POSIX_MAPPED_FILES
#include <sys/mman.h>
#endif

#include "exfat_ondisk.h"
#include "libexfat.h"

#include "exfat_fs.h"
#include "exfat_dir.h"
#include "env.h"

struct exfat_inode *exfat_alloc_inode(__u16 attr)
{
	struct exfat_inode *node;
	int size;

	size = offsetof(struct exfat_inode, name) + NAME_BUFFER_SIZE;
	node = calloc(1, size);
	if (!node) {
		exfat_err("failed to allocate exfat_node\n");
		return NULL;
	}

	node->parent = NULL;
	INIT_LIST_HEAD(&node->children);
	INIT_LIST_HEAD(&node->sibling);
	INIT_LIST_HEAD(&node->list);

	node->attr = attr;
	return node;
}

void exfat_free_inode(struct exfat_inode *node)
{
	if (node) {
		if (node->dentry_set)
			free(node->dentry_set);
		free(node);
	}
}

void exfat_free_children(struct exfat_inode *dir, bool file_only)
{
	struct exfat_inode *node, *i;

	list_for_each_entry_safe(node, i, &dir->children, sibling) {
		if (file_only) {
			if (!(node->attr & ATTR_SUBDIR)) {
				list_del(&node->sibling);
				exfat_free_inode(node);
			}
		} else {
			list_del(&node->sibling);
			list_del(&node->list);
			exfat_free_inode(node);
		}
	}
}

void exfat_free_file_children(struct exfat_inode *dir)
{
	exfat_free_children(dir, true);
}

/* delete @child and all ancestors that does not have
 * children
 */
void exfat_free_ancestors(struct exfat_inode *child)
{
	struct exfat_inode *parent;

	while (child && list_empty(&child->children)) {
		if (!child->parent || !(child->attr & ATTR_SUBDIR))
			return;

		parent = child->parent;
		list_del(&child->sibling);
		exfat_free_inode(child);

		child = parent;
	}
	return;
}

void exfat_free_dir_list(struct exfat *exfat)
{
	struct exfat_inode *dir, *i;

	list_for_each_entry_safe(dir, i, &exfat->dir_list, list) {
		if (!dir->parent)
			continue;
		exfat_free_file_children(dir);
		list_del(&dir->list);
		exfat_free_inode(dir);
	}
}

void exfat_free_exfat(struct exfat *exfat)
{
	if (exfat == NULL)
		return;

	free(exfat->bs);
	exfat->bs = NULL;

	if (exfat->free_disk_bitmap != NULL) {
		exfat->free_disk_bitmap(exfat);
		exfat->free_disk_bitmap = NULL;
	}
	if (exfat->free_alloc_bitmap != NULL) {
		exfat->free_alloc_bitmap(exfat);
		exfat->free_alloc_bitmap = NULL;
	}

	free(exfat->upcase_table);
	exfat->upcase_table = NULL;

	exfat_free_inode(exfat->root);
	exfat->root = NULL;

	if (exfat->lookup_buffer)
		exfat_free_buffer(exfat, exfat->lookup_buffer);
	exfat->lookup_buffer = NULL;

	free(exfat);
}

#if defined(_POSIX_MAPPED_FILES) && defined(O_TMPFILE)
static void unmap_amap(struct exfat *exfat)
{
	munmap(exfat->alloc_bitmap, exfat->bm_size);
	exfat->alloc_bitmap = NULL;
}
#endif

/*
 * Create an unnamed temporary file(O_TMPFILE) that only persists with the
 * process, allocate space needed, and then mmap() it as alloc_bitmap.
 *
 * By default, the temporary file is created in /var/tmp. The path can be
 * overridden with the influential environment variable 'EXFAT_SCRATCH_AMAP'.
 * Setting it as an empty string disables this feature.
 *
 * NOTE that the idea of falling back to mkstemp() is abandoned because it is
 * impossible to guarantee the deletion of the temp file created that way -
 * there's a short window of opportunity between mkstemp() and unlink() where
 * the process can die.
 */
static bool open_tmp_amap(struct exfat *exfat)
{
	bool ret = false;
#if defined(_POSIX_MAPPED_FILES) && defined(O_TMPFILE)
	const char *path = getenv(EXFAT_ENV_SCRATCH_AMAP);
	int fd;

	if (path == NULL)
		path = EXFAT_ENV_SCRATCH_AMAP_DEFAULT;
	else if (path[0] == 0)
		return false;

	fd = open(path, O_TMPFILE | O_RDWR, 0600);
	if (fd < 0)
		goto out;

	if (fallocate(fd, 0, 0, exfat->bm_size) == 0) {
		void *m = mmap(NULL, exfat->bm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

		if (m != MAP_FAILED) {
			ret = true;
			exfat->alloc_bitmap = m;
			exfat->free_alloc_bitmap = unmap_amap;
			exfat_debug("amap: opened O_TMPFILE at %s\n", path);
			goto out;
		}
	}
out:
	if (!ret)
		exfat_debug("%s: %s\n", path, strerror(errno));
	if (fd >= 0)
		close(fd);
#endif
	return ret;
}

static void free_amap(struct exfat *exfat)
{
	free(exfat->alloc_bitmap);
	exfat->alloc_bitmap = NULL;
}

/*
 * Allocate alloc_bitmap in the traditional dynamic heap memory.
 */
static bool alloc_amap(struct exfat *exfat)
{
	exfat->alloc_bitmap = calloc(1, exfat->bm_size);

	if (exfat->alloc_bitmap != NULL) {
		exfat->free_alloc_bitmap = free_amap;
		exfat_debug("amap: calloc()'d\n");
		return true;
	}

	return false;
}

struct exfat *exfat_alloc_exfat(struct exfat_blk_dev *blk_dev, struct pbr *bs,
		struct exfat_inode *root, const bool need_amap)
{
	struct exfat *exfat;

	if (!bs) {
		if (read_boot_sect(blk_dev, &bs))
			return NULL;
	}

	exfat = calloc(1, sizeof(*exfat));
	if (!exfat) {
		if (root)
			exfat_free_inode(root);

		free(bs);
		return NULL;
	}

	INIT_LIST_HEAD(&exfat->dir_list);
	exfat->blk_dev = blk_dev;
	exfat->bs = bs;
	exfat->clus_count = le32_to_cpu(bs->bsx.clu_count);
	exfat->clus_size = EXFAT_CLUSTER_SIZE(bs);
	exfat->sect_size = EXFAT_SECTOR_SIZE(bs);
	exfat->bm_size = EXFAT_BITMAP_SIZE(exfat->clus_count);
	exfat->bm_len = DIV_ROUND_UP(exfat->clus_count, 8);
	exfat->root = root;
	exfat->pagesize = sysconf(_SC_PAGE_SIZE);

	assert(exfat->clus_count > 0);
	assert(exfat->pagesize > 0);
	assert(exfat->bm_size >= exfat->bm_len);

	if (need_amap) {
		if (!(open_tmp_amap(exfat) || alloc_amap(exfat))) {
			exfat_err("failed to allocate bitmap\n");
			goto err;
		}
	}

	exfat->buffer_count = ((MAX_EXT_DENTRIES + 1) * DENTRY_SIZE) /
		exfat_get_read_size(exfat) + 1;

	exfat->start_clu = EXFAT_FIRST_CLUSTER;

	if (exfat->root)
		return exfat;

	exfat->root = exfat_alloc_inode(ATTR_SUBDIR);
	if (!exfat->root)
		goto err;

	exfat->root->first_clus = le32_to_cpu(exfat->bs->bsx.root_cluster);

	if (exfat_root_clus_count(exfat)) {
		exfat_err("failed to follow the cluster chain of root\n");
		goto err;
	}

	return exfat;
err:
	exfat_free_exfat(exfat);
	return NULL;
}

struct buffer_desc *exfat_alloc_buffer(struct exfat *exfat)
{
	struct buffer_desc *bd;
	unsigned int i;
	unsigned int read_size = exfat_get_read_size(exfat);

	bd = calloc(exfat->buffer_count, sizeof(*bd));
	if (!bd)
		return NULL;

	for (i = 0; i < exfat->buffer_count; i++) {
		bd[i].buffer = malloc(read_size);
		if (!bd[i].buffer)
			goto err;

		memset(&bd[i].dirty, 0, sizeof(bd[i].dirty));
	}
	return bd;
err:
	exfat_free_buffer(exfat, bd);
	return NULL;
}

void exfat_free_buffer(const struct exfat *exfat, struct buffer_desc *bd)
{
	unsigned int i;

	for (i = 0; i < exfat->buffer_count; i++) {
		if (bd[i].buffer)
			free(bd[i].buffer);
	}
	free(bd);
}

#ifdef _POSIX_MAPPED_FILES
static bool msync_disk_bitmap(struct exfat *exfat)
{
	return msync(exfat->disk_bitmap_m, exfat->disk_bitmap_msize, MS_SYNC) == 0;
}

static void unmap_disk_bitmap(struct exfat *exfat)
{
	munmap(exfat->disk_bitmap_m, exfat->disk_bitmap_msize);
	exfat->disk_bitmap = exfat->disk_bitmap_m = NULL;
	exfat->disk_bitmap_msize = 0;
}
#endif

static bool write_disk_bitmap(struct exfat *exfat, size_t ofs, size_t len)
{
	return exfat_write_full(exfat->blk_dev->dev_fd, exfat->disk_bitmap + ofs,
				len, exfat->disk_bitmap_devofs + ofs);
}

static void free_disk_bitmap(struct exfat *exfat)
{
	free(exfat->disk_bitmap);
	exfat->disk_bitmap = exfat->disk_bitmap_m = NULL;
}

/*
 * Either map the allocation bitmap region on the disk to the address space or
 * allocate memory and read the allocation bitmap from the disk.
 */
bool exfat_load_disk_bitmap(struct exfat *exfat, const off_t loc, const bool rw)
{
	void *m;
#ifdef _POSIX_MAPPED_FILES
	const off_t req_size = loc + exfat->bm_size;
	const int prot = rw ? PROT_READ | PROT_WRITE : PROT_READ;

	if (req_size > 0 && exfat->blk_dev->size >= req_size) {
		const off_t pa_ofs = loc & ~((off_t)exfat->pagesize - 1);
		const size_t odelta = (size_t)(loc - pa_ofs);
		const size_t msize = exfat->bm_size + odelta;

		assert(msize >= exfat->bm_size);

		m = mmap(NULL, msize, prot, MAP_SHARED, exfat->blk_dev->dev_fd, pa_ofs);
		if (m != MAP_FAILED) {
			exfat->disk_bitmap = exfat->disk_bitmap_m = m;
			exfat->disk_bitmap += odelta;
			exfat->disk_bitmap_msize = msize;
			exfat->disk_bitmap_devofs = loc;
			exfat->invalidate_disk_bitmap = NULL;
			exfat->sync_disk_bitmap = rw ? msync_disk_bitmap : NULL;
			exfat->free_disk_bitmap = unmap_disk_bitmap;

			exfat_debug("dmap: mmap()'d\n");
			return true;
		}

		exfat_debug("mmap(): %s\n", strerror(errno));
	}
#endif
	m = calloc(1, exfat->bm_size);
	if (m != NULL && exfat_read_full(exfat->blk_dev->dev_fd, m, exfat->bm_len, loc)) {
		exfat->disk_bitmap = exfat->disk_bitmap_m = m;
		exfat->disk_bitmap_msize = exfat->bm_size;
		exfat->disk_bitmap_devofs = loc;
		exfat->invalidate_disk_bitmap = write_disk_bitmap;
		exfat->sync_disk_bitmap = NULL;
		exfat->free_disk_bitmap = free_disk_bitmap;

		exfat_debug("dmap: calloc() & read()\n");
		return true;
	}

	free(m);
	return false;
}

/*
 * get references of ancestors that include @child until the count of
 * ancesters is not larger than @count and the count of characters of
 * their names is not larger than @max_char_len.
 * return true if root is reached.
 */
static bool get_ancestors(struct exfat_inode *child,
			  struct exfat_inode **ancestors, int count,
			  int max_char_len,
			  int *ancestor_count)
{
	struct exfat_inode *dir;
	int name_len, char_len;
	int root_depth, depth, i;

	root_depth = 0;
	char_len = 0;
	max_char_len += 1;

	dir = child;
	while (dir) {
		name_len = exfat_utf16_len(dir->name, NAME_BUFFER_SIZE);
		if (char_len + name_len > max_char_len)
			break;

		/* include '/' */
		char_len += name_len + 1;
		root_depth++;

		dir = dir->parent;
	}

	depth = MIN(root_depth, count);

	for (dir = child, i = depth - 1; i >= 0; dir = dir->parent, i--)
		ancestors[i] = dir;

	*ancestor_count = depth;
	return !dir;
}

int exfat_resolve_path(struct path_resolve_ctx *ctx, struct exfat_inode *child)
{
	int depth, i;
	int name_len;
	__le16 *utf16_path;
	size_t in_size;

	ctx->local_path[0] = '\0';

	get_ancestors(child,
		      ctx->ancestors,
		      sizeof(ctx->ancestors) / sizeof(ctx->ancestors[0]),
		      PATH_MAX,
		      &depth);

	utf16_path = ctx->utf16_path;
	for (i = 0; i < depth; i++) {
		name_len = exfat_utf16_len(ctx->ancestors[i]->name,
					   NAME_BUFFER_SIZE);
		memcpy((char *)utf16_path, (char *)ctx->ancestors[i]->name,
		       name_len * 2);
		utf16_path += name_len;
		*utf16_path = UTF16_SLASH;
		utf16_path++;
	}

	if (depth > 1)
		utf16_path--;
	*utf16_path = UTF16_NULL;
	utf16_path++;

	in_size = (utf16_path - ctx->utf16_path) * sizeof(__le16);
	return exfat_utf16_dec(ctx->utf16_path, in_size,
				ctx->local_path, sizeof(ctx->local_path));
}

int exfat_resolve_path_parent(struct path_resolve_ctx *ctx,
			      struct exfat_inode *parent, struct exfat_inode *child)
{
	int ret;
	struct exfat_inode *old;

	old = child->parent;
	child->parent = parent;

	ret = exfat_resolve_path(ctx, child);
	child->parent = old;
	return ret;
}
