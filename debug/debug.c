#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <locale.h>

#include <getopt.h>

#include "exfat_ondisk.h"
#include "libexfat.h"
#include "exfat_fs.h"
#include "exfat_dir.h"

#define PROGNAME "debug.exfat"

static struct {
	struct exfat_user_input	ui;
	struct exfat_blk_dev bd;
	struct exfat *exfat;
	struct {
		int (*mainf)(void);
		char **argv;
		int argc;
		bool help:1;
		bool recursive:1;
		bool dryrun:1;
		bool secure:1;
		bool zero:1;
	} param;
	bool soiled:1;
} dbg;

static void usage(void)
{
	fputs(	"Usage: " PROGNAME " [-h]\n"
		"       " PROGNAME " [-Rv]   [--] get  <DEVICE> <FILES ...> <DST>\n"
		"       " PROGNAME " [-Rv]   [--] put  <DEVICE> <FILES ...> <DST>\n"
		"       " PROGNAME " [-Rv]   [--] del  <DEVICE> <FILES ...>\n"
		"       " PROGNAME " [-vdsz] [--] trim <DEVICE>\n"
		"       " PROGNAME " [-vd]   [--] gc   <DEVICE>\n"
		"\n"
		"-- These aren't the droids you're looking for --\n"
		"(Use at your own risk. See debug.exfat(8))\n"
		, stderr);
}

static int main_get(void);
static int main_put(void);
static int main_del(void);
static int main_trim(void);
static int main_gc(void);

static bool parse_opts(int argc, char *argv[])
{
	int c;
	int min_args, max_args = -1;
	const char *cmd;

	for (;;) {
		c = getopt(argc, (char *const *)argv, "hRvdsz");
		switch (c) {
		case 'h':
			dbg.param.help = true;
			break;
		case 'r':
			dbg.param.recursive = true;
			break;
		case 'v':
			print_level++;
			break;
		case 'd':
			dbg.param.dryrun = true;
			break;
		case 's':
			dbg.param.secure = true;
			break;
		case 'z':
			dbg.param.zero = true;
			break;
		default:
			if (c < 0)
				goto done;
			return false;
		}
	}
done:
	if (dbg.param.help)
		return true;

	if (optind + 1 >= argc)
		goto few_args;

	cmd = argv[optind++];
	if (strcmp("put", cmd) == 0) {
		if (dbg.param.secure || dbg.param.zero)
			goto nosz;
		if (dbg.param.dryrun)
			goto nodry;

		min_args = 2;
		dbg.param.mainf = main_put;
		dbg.ui.writeable = true;
	} else if (strcmp("get", cmd) == 0) {
		if (dbg.param.secure || dbg.param.zero)
			goto nosz;
		if (dbg.param.dryrun)
			goto nodry;

		min_args = 2;
		dbg.param.mainf = main_get;
	} else if (strcmp("del", cmd) == 0) {
		if (dbg.param.secure || dbg.param.zero)
			goto nosz;
		if (dbg.param.dryrun)
			goto nodry;

		min_args = 1;
		dbg.param.mainf = main_del;
		dbg.ui.writeable = true;
	} else if (strcmp("trim", cmd) == 0) {
		if (dbg.param.recursive)
			goto norec;
		/* These are serious security feature so we don't muck around */
		if (dbg.param.secure && dbg.param.zero)
			goto nobothsz;

		max_args = 1;
		min_args = 0;
		dbg.param.mainf = main_trim;
		dbg.ui.writeable = true;
	} else if (strcmp("gc", cmd) == 0) {
		if (dbg.param.recursive)
			goto norec;
		if (dbg.param.secure || dbg.param.zero)
			goto nosz;

		max_args = 1;
		min_args = 0;
		dbg.param.mainf = main_gc;
		dbg.ui.writeable = true;
	} else {
		exfat_err(PROGNAME ": %s: unknown command\n", cmd);
		return false;
	}

	if (max_args > 0 && optind + max_args < argc)
		goto many_args;

	dbg.ui.dev_name = argv[optind++];
	dbg.param.argv = argv + optind;
	dbg.param.argc = argc - optind;
	if (min_args > dbg.param.argc)
		goto few_args;

	if (dbg.param.dryrun)
		dbg.ui.writeable = false;

	return true;
few_args:
	exfat_err(PROGNAME ": too few arguments\n");
	return false;
many_args:
	exfat_err(PROGNAME ": too many arguments\n");
	return false;
norec:
	exfat_err(PROGNAME ": -R cannot be used for this command\n");
	return false;
nosz:
	exfat_err(PROGNAME ": -s and -z cannot be used for this command\n");
	return false;
nodry:
	exfat_err(PROGNAME ": -d cannot be used for this command\n");
	return false;
nobothsz:
	exfat_err(PROGNAME ": -s and -z cannot be specified at the same time\n");
	return false;

}

static bool soil_vol(void)
{
	if (!dbg.param.dryrun && !dbg.soiled) {
		dbg.soiled = exfat_mark_volume_dirty(dbg.exfat, true) == 0;
		return dbg.soiled;
	}

	return true;
}

static char *get_dst(void)
{
	return dbg.param.argv[dbg.param.argc - 1];
}

static bool is_dash(const char *s)
{
	return s[0] == '-' && s[1] == 0;
}

static bool has_bad_name(const char *path)
{
	for (; *path != 0; path++) {
		if (exfat_bad_char(*path))
			return true;
	}
	return false;
}

static bool validate_fileop_argv(const bool isput)
{
	char *a, *errmsg;
	const int cnt = dbg.param.argc - 1;
	int src_dashes = 0;
	int saved_errno;

	for (int i = 0; i < cnt; i++) {
		a = dbg.param.argv[i];
		src_dashes += is_dash(a);
		if (!isput) { /* main_get() */
			if (a[0] != '/') {
				errmsg = "source should be an absolute path";
				goto patherr;
			}
			exfat_normalpath_logical_scrub(a, '/');
			if (has_bad_name(a))
				goto badname;
		}
	}

	a = get_dst();
	if (dbg.param.recursive && (src_dashes > 0 || is_dash(a))) {
		exfat_err(PROGNAME ": dash not allowed in recursive mode\n");
		return false;
	}

	if (isput) { /* main_put() */
		exfat_normalpath_logical_scrub(a, '/');
		if (has_bad_name(a))
			goto badname;
		if (a[0] != '/') {
			errmsg = "destination should be an absolute path";
			goto patherr;
		}
		if (src_dashes > 1)  {
			errmsg = "multiple dashes in source list";
			goto patherr;
		}
	}

	return true;
badname:
	errmsg = "invalid exFAT file name";
	/* fall-through */
patherr:
	exfat_err(PROGNAME ": %s: %s\n", a, errmsg);
	return false;
}

static void sort_argv(const char **argv, const size_t len)
{
	/* TODO */
}

struct target_tree_element {
	struct target_tree_element *parent;
	struct list_head children;
};

static struct list_head target_tree;

/*
 * argv must be sorted in ASC order so that the unique directory names are in
 * groups and shorter paths appear first.
 *
 * This is important! Otherwise the behaviour is undefined!
 */
static bool build_target_tree(const char **argv, const size_t len)
{
	/* TODO */
}

static bool chase_targets(bool(*callback)(struct target_tree_element *e))
{
	/* TODO */
}

static int main_get(void)
{
	if (!validate_fileop_argv(false))
		return 2;
	// TODO
	errno = ENOSYS;
	perror(PROGNAME);
	return EXIT_FAILURE;
}

static int main_put(void)
{
	if (!validate_fileop_argv(true))
		return 2;
	// TODO
	// TODO call soil_vol() right before making changes
	errno = ENOSYS;
	perror(PROGNAME);
	return EXIT_FAILURE;
}

static int main_del(void)
{
	// TODO
	// TODO call soil_vol() right before making changes
	errno = ENOSYS;
	perror(PROGNAME);
	return EXIT_FAILURE;
}

static int main_trim_inner(const bool ckrun)
{
	/* Discard 2 GB at a time */
	const clus_t step = (clus_t)DIV_ROUND_UP(2ULL << 30, dbg.exfat->clus_size);
	const clus_t lim = dbg.exfat->clus_count + EXFAT_FIRST_CLUSTER;
	clus_t start = EXFAT_FIRST_CLUSTER, end, clen, sum = 0, used_clus;
	off_t o, l;
	int err;

	do {
		if (exfat_bitmap_find_zero(dbg.exfat, dbg.exfat->disk_bitmap, start, &start))
			break;
		if (exfat_bitmap_find_one(dbg.exfat, dbg.exfat->disk_bitmap, start, &end))
			end = dbg.exfat->clus_count + EXFAT_FIRST_CLUSTER;

		assert(start < end);
		assert(exfat_heap_clus(dbg.exfat, start) && exfat_heap_clus(dbg.exfat, end - 1));

		clen = end - start;
		if (clen > step) {
			end = start + step;
			clen = step;
		}
		sum += clen;
		o = exfat_c2o(dbg.exfat, start);
		l = (off_t)dbg.exfat->clus_size * clen;
		if (!ckrun)
			exfat_debug("trim: %" PRIu32 "+%" PRIu32 "(%" PRIuMAX "+%" PRIuMAX ")\n",
					start, clen, (uintmax_t)o, (uintmax_t)l);

		if (!dbg.param.dryrun && !ckrun) {
			const char *fname = "";

			if (dbg.bd.isblk) {
				if (dbg.param.secure) {
					err = exfat_secerase_blocks(dbg.bd.dev_fd, o, l);
					fname = "exfat_secerase_blocks()";
				} else if (dbg.param.zero) {
					err = exfat_zeroout_blocks(dbg.bd.dev_fd, o, l);
					fname = "exfat_zeroout_blocks()";
				} else {
					err = exfat_discard_blocks(dbg.bd.dev_fd, o, l);
					fname = "exfat_discard_blocks()";
				}
			} else {
				if (dbg.param.zero) {
					err = exfat_write_zero(dbg.bd.dev_fd, l, o);
					fname = "exfat_write_zero()";
				} else {
					err = exfat_dealloc_file_range(dbg.bd.dev_fd, o, l);
					fname = "exfat_dealloc_file_range()";
				}
			}

			if (err) {
				exfat_err(PROGNAME ": %s: %s\n", fname, strerror(err));
				return -err;
			}
		}

		start = end;
	} while (start < lim);

	if (!ckrun)
		exfat_info("%s: %" PRIuMAX " bytes (%" PRIu32 " clusters) trimmed%s\n",
				dbg.ui.dev_name, (uintmax_t)sum * dbg.exfat->clus_size, sum,
				dbg.param.dryrun ? " (dryrun)" : "");

	used_clus = exfat_count_used_clusters(dbg.exfat->disk_bitmap,
			dbg.exfat->disk_bitmap_size, dbg.exfat->clus_count);
	assert(used_clus + sum == dbg.exfat->clus_count);

	return 0;
}

static int main_trim(void)
{
	int ret;

	if (!dbg.bd.isblk && dbg.param.secure) {
		/*
		 * As there's no security built in fallocate(), there's simply
		 * no way to implement it on Linux.
		 */
		exfat_err(PROGNAME ": %s: -s on image file is not supported\n",
				dbg.ui.dev_name);
		return 2;
	}

	/* Do a self-test run before actually issuing discard/fallocate */
	ret = main_trim_inner(true);
	if (ret == 0)
		ret = main_trim_inner(false);

	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int main_gc(void)
{
	/*
	 * TODO
	 *
	 * refuse to trim directories with unrecognised secondary/benign
	 * dentries as they could have some offset-relative data. Just dock at
	 * the last dentry.
	 */
}

static int read_bitmap(void)
{
	/*
	 * FIXME this is copied and pasted from fsck.c. After merging #380,
	 * all this will have to change as well to support mmap().
	 */
	struct exfat_lookup_filter filter = {
		.in.type	= EXFAT_BITMAP,
		.in.dentry_count = 0,
		.in.filter	= NULL,
		.in.param	= NULL,
	};
	struct exfat_dentry *dentry;
	uint64_t map_size, need_map_size;
	int retval;

	retval = exfat_lookup_dentry_set(dbg.exfat, dbg.exfat->root, &filter);
	if (retval)
		return retval;

	dentry = filter.out.dentry_set;
	exfat_debug(PROGNAME ": bitmap start cluster %#x, size %#" PRIx64 "\n",
			le32_to_cpu(dentry->bitmap_start_clu),
			le64_to_cpu(dentry->bitmap_size));

	/* Validate on-disk bitmap size and required size */
	map_size = le64_to_cpu(dentry->bitmap_size);
	need_map_size = DIV_ROUND_UP(dbg.exfat->clus_count, 8);
	if (map_size != need_map_size) {
		exfat_err(PROGNAME ": invalid bitmap size. %" PRIu64 "\n", map_size);
		return -EINVAL;
	}

	if (!exfat_heap_clus(dbg.exfat, le32_to_cpu(dentry->bitmap_start_clu))) {
		exfat_err(PROGNAME ": invalid start cluster of allocate bitmap. 0x%x\n",
				le32_to_cpu(dentry->bitmap_start_clu));
		retval = -EINVAL;
		goto out;
	}

	dbg.exfat->disk_bitmap_clus = le32_to_cpu(dentry->bitmap_start_clu);
	dbg.exfat->disk_bitmap_size = DIV_ROUND_UP(dbg.exfat->clus_count, 8);

	exfat_bitmap_set_range(dbg.exfat, dbg.exfat->alloc_bitmap,
			       le32_to_cpu(dentry->bitmap_start_clu),
			       DIV_ROUND_UP(dbg.exfat->disk_bitmap_size,
					    dbg.exfat->clus_size));
	/* TODO use exfat_load_disk_bitmap() */
	if (!exfat_read_full(dbg.exfat->blk_dev->dev_fd, dbg.exfat->disk_bitmap,
			dbg.exfat->disk_bitmap_size,
			exfat_c2o(dbg.exfat, dbg.exfat->disk_bitmap_clus))) {
		exfat_err(PROGNAME ": I/O error\n");
		retval = -EIO;
		goto out;
	}

out:
	free(filter.out.dentry_set);
	return retval;
}

int main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE;

	setlocale(LC_ALL, "");

	INIT_LIST_HEAD(&target_tree);
	exfat_init_blk_dev_info(&dbg.bd);
	exfat_init_user_input(&dbg.ui);
	print_level = EXFAT_ERROR;

	if (!parse_opts(argc, argv))
		return 2;

	if (dbg.param.help) {
		usage();
		return 0;
	}

	if (exfat_get_blk_dev_info(&dbg.ui, &dbg.bd))
		goto out;
	dbg.exfat = exfat_alloc_exfat(&dbg.bd, NULL, NULL/*, true*/);
	if (dbg.exfat == NULL)
		goto out;
	if (read_bitmap())
		goto out;

	/*
	 * Data loss could occur if there's any filesystem error, especially in
	 * main_trim() if there's any clusters not marked in the allocation
	 * bitmap but in a valid cluster chain.
	 *
	 * As a line of defence, check the dirty bit.
	 */
	if (exfat_get_volume_dirty(dbg.exfat)) {
		if (dbg.ui.writeable) {
			exfat_err(PROGNAME ": %s: volume marked dirty.\n"
					"Please run fsck first in order to use "
					PROGNAME " on this device/image\n",
					dbg.ui.dev_name);
			goto out;
		} else
			exfat_info(PROGNAME ": %s: volume marked dirty.\n"
					"Please run fsck. Continuing anyway, "
					"but the results may be bogus\n",
					dbg.ui.dev_name);
	}

	ret = dbg.param.mainf();
	if (ret)
		goto out;

	if (dbg.soiled && exfat_mark_volume_dirty(dbg.exfat, false))
		goto sync_err;
	if (dbg.ui.writeable && exfat_fsync(dbg.bd.dev_fd))
		/*
		 * Reached only after trim command(no change in exFAT structure,
		 * but issued discard/fallocate op). No harm in calling fsync()
		 * twice.
		 */
		goto sync_err;
	goto out;

sync_err:
	exfat_err(PROGNAME ": %s: sync failed: %s\n", dbg.ui.dev_name, strerror(errno));
	ret = EXIT_FAILURE;
out:
	exfat_free_exfat(dbg.exfat);
	exfat_deinit_blk_dev_info(&dbg.bd);
	exfat_deinit_user_input(&dbg.ui);

	exfat_print_iostat();
	return ret;
}
