/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _EXFAT_ENV_H
#define _EXFAT_ENV_H

/* Influential env vars */

#define EXFAT_ENV_TTY_OVERRIDE		"EXFAT_TTY_OVERRIDE"
#define EXFAT_ENV_SCRATCH_AMAP		"EXFAT_SCRATCH_AMAP"
#define EXFAT_ENV_SCRATCH_AMAP_DEFAULT	"/var/tmp"
#define EXFAT_ENV_MMAP			"EXFAT_MMAP"
/* Allow mmap() in exfat_map_zeromem() and exfat_map_blankmem() */
#define EXFAT_ENV_MMAP_MEMDEV		(0x01)
/* Allow mmap() for allocation bitmap handling */
#define EXFAT_ENV_MMAP_BITMAP		(0x02)
/*
 * Enable EXFAT_ENV_MMAP_BITMAP upon user's request.
 *
 * There's only one implementation of /dev/zero which is memdev but there are
 * many different implementations backing blockdev, some of which may exhibit
 * problematic behaviour when mmap()'d.
 */
#define EXFAT_ENV_MMAP_DEFAULT		(EXFAT_ENV_MMAP_MEMDEV)

#endif /* !_EXFAT_ENV_H */
