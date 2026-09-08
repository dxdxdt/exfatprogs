/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *   Copyright (C) 2021 LG Electronics.
 *
 *   Author(s): Hyunchul Lee <hyc.lee@gmail.com>
 */

#ifndef _DIR_H_
#define _DIR_H_

struct exfat;
struct exfat_inode;
struct exfat_dentry_loc;
struct buffer_desc;

struct exfat_de_iter {
	struct exfat		*exfat;
	struct exfat_inode	*parent;
	struct buffer_desc	*buffer_desc;
	__u32			ra_next_clus;
	unsigned int		ra_begin_offset;
	unsigned int		ra_partial_size;
	unsigned int		read_size;		/* cluster size */
	unsigned int		write_size;		/* sector size */
	off_t			de_file_offset;
	off_t			next_read_offset;
	int			max_skip_dentries;
#define INVALID_NAME_NUM_MAX	9999999
	unsigned int		invalid_name_num;

	char *name_hash_bitmap;		/* bitmap of children's name hashes */
};

struct exfat_lookup_filter {
	struct {
		uint8_t		type;
		int		dentry_count;
		/* return 0 if matched, return 1 if not matched,
		 * otherwise return errno
		 */
		int		(*filter)(struct exfat_de_iter *iter,
					  void *param, int *dentry_count);
		void		*param;
	} in;
	struct {
		struct exfat_dentry	*dentry_set;
		int			dentry_count;
		off_t			file_offset;
		/*
		 * If the dentry_set found:
		 *   - device offset where the dentry_set locates.
		 * If the dentry_set not found:
		 *   - device offset where the first empty dentry_set locates
		 *     if in.dentry_count > 0 and there are enough empty dentry.
		 *   - device offset where the last empty dentry_set locates
		 *     if in.dentry_count = 0 or no enough empty dentry.
		 *   - EOF if no empty dentry_set.
		 */
		off_t			dev_offset;
	} out;
};

union exfat_timestamp {
	uint32_t raw;
	struct {
		uint16_t time_part;
		uint16_t date_part;
	};
};

#define EXFAT_TRAVERSAL_BF	(0x00) /* breadth-first */
struct exfat_traverse; /* Opaque object */

int exfat_de_iter_init(struct exfat_de_iter *iter, struct exfat *exfat,
		       struct exfat_inode *dir, struct buffer_desc *bd);
int exfat_de_iter_get(struct exfat_de_iter *iter,
		      int ith, struct exfat_dentry **dentry);
int exfat_de_iter_get_dirty(struct exfat_de_iter *iter,
			    int ith, struct exfat_dentry **dentry);
int exfat_de_iter_flush(struct exfat_de_iter *iter);
int exfat_de_iter_advance(struct exfat_de_iter *iter, int skip_dentries);
int exfat_de_iter_revert(struct exfat_de_iter *iter, int dentries);
off_t exfat_de_iter_device_offset(struct exfat_de_iter *iter);
off_t exfat_de_iter_file_offset(struct exfat_de_iter *iter);

int exfat_lookup_dentry_set(struct exfat *exfat, struct exfat_inode *parent,
			    struct exfat_lookup_filter *filter);
int exfat_lookup_file(struct exfat *exfat, struct exfat_inode *parent,
		      const char *name, struct exfat_lookup_filter *filter_out);
int exfat_lookup_file_by_utf16name(struct exfat *exfat,
				 struct exfat_inode *parent,
				 __le16 *utf16_name,
				 struct exfat_lookup_filter *filter_out);

int exfat_create_file(struct exfat *exfat, struct exfat_inode *parent,
		      const char *name, unsigned short attr);
int exfat_update_file_dentry_set(struct exfat *exfat,
				 struct exfat_dentry *dset, int dcount,
				 const char *name,
				 clus_t start_clu, clus_t ccount);
int exfat_build_file_dentry_set(struct exfat *exfat, const char *name,
				unsigned short attr, struct exfat_dentry **dentry_set,
				int *dentry_count);
int exfat_add_dentry_set(struct exfat *exfat, struct exfat_dentry_loc *loc,
			 struct exfat_dentry *dset, int dcount,
			 bool need_next_loc);
void exfat_calc_dentry_checksum(struct exfat_dentry *dentry,
				uint16_t *checksum, bool primary);
uint16_t calc_dentry_set_checksum(struct exfat_dentry *dset, int dcount);
uint16_t exfat_calc_name_hash(struct exfat *exfat,
			      __le16 *name, int len);

int exfat_find_free_cluster(struct exfat *exfat, int clu_count,
		clus_t *new_clu);
int exfat_alloc_cluster(struct exfat *exfat, struct exfat_inode *inode,
		clus_t *new_clu);

/* Buffer len: at least 30 characters including the null-terminator */
#define EXFAT_TIMESTAMP_STRLEN (30)

int exfat_format_timestamp(char *out, size_t size,
		const union exfat_timestamp *ts, uint8_t cs, uint8_t tz);

/*
 * fts()-like directory traversal facilities
 *
 * The directory traversal code is copied and pasted in utils including dump,
 * exfat2img and defrag. fsck deserves its own implementation as it requires
 * ability to modify the directory structure on the fly, but the rest of the
 * utils can get away with just a read-only ability to traverse directories.
 * These facilities are en effort to round up and simplify the directory
 * traversal code across the project.
 *
 * Refer to fts(3) and twalk(3) for nomenclature used here.
 *
 * If the visit is postorder(the directory is being removed from the stack),
 * the read function sets the inode to NULL. Otherwise, the inode pointer is set
 * to point a valid object allocated internally and ready to use by the caller.
 * The visit order depends on on the value of node->attr. If it has ATTR_SUBDIR
 * set, the order is preorder. Otherwise, the order is leaf.
 *
 * Note that if the directory turns out to be empty in postorder, the read
 * function sets the inode NULL to indicate that.
 *
 * The exfat_traverse object can closed anytime to abandon the traversal. The
 * inode object returned from the read function must not be freed as the
 * lifespan of the object is managed with the context object.
 */

/*
 * Begin a directory traversal
 *
 * Currently, only EXFAT_TRAVERSAL_BF is supported. On failure, errno is set to
 * indicate the error.
 *
 *  - ENOMEM
 *  - ENOTDIR if node is not a directory
 *  - EINVAL on wrong flags
 */
struct exfat_traverse *exfat_traverse_open(struct exfat_inode *node, int flags);
/*
 * Close and deallocate all the dynamically allocated resourced for the context.
 */
void exfat_traverse_close(struct exfat_traverse *ctx);
/*
 * Read and return an inode
 *
 * Returns 1 if the op was successful the node is set to either NULL or an
 * address to a valid inode object. Returns 0 if there are no more nodes left to
 * traverse(end of traversal).
 */
int exfat_traverse_read(struct exfat_traverse *ctx, struct exfat_inode **node);
/*
 * Mark the current node to skip it. No descendants of the directory will be
 * visited.
 */
int exfat_traverse_skip(struct exfat_traverse *ctx);

#endif
