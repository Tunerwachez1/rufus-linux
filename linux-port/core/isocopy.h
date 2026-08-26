/*
 * Rufus Linux port - copy an ISO's file tree onto a mounted filesystem.
 *
 * Uses the UDF layer when the image has one (Microsoft images keep their
 * real content there) and falls back to ISO9660/Joliet/RockRidge for
 * plain hybrids. Traversal and extraction mirror src/iso.c.
 * GPLv3
 */
#ifndef RUFUS_ISOCOPY_H
#define RUFUS_ISOCOPY_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

/* classic FAT32 ceiling: one file can hold at most 2^32 - 1 bytes */
#define RFD_FAT32_MAX_FILE 0xFFFFFFFFull

/*
 * Target size of one .swm part when a *.wim exceeds max_file_size.
 * Comfortably below the FAT32 limit so that metadata growth during the
 * split can never push a part past what the filesystem accepts.
 */
#define RFD_WIM_SPLIT_CHUNK (3500ull << 20)

typedef void (*rfd_copy_progress)(uint64_t done_bytes, uint64_t total_bytes,
	const char *current_name, void *udata);

typedef void (*rfd_bigfile_cb)(const char *rel_path, uint64_t len,
	bool is_wim, void *udata);

typedef struct {
	volatile sig_atomic_t *cancel;   /* may be NULL */
	rfd_copy_progress progress;      /* may be NULL */
	void *udata;
	uint64_t max_bytes;              /* 0 = unlimited (testing aid) */
	/*
	 * Files longer than this are rejected with -EFBIG unless their name
	 * ends in .wim, in which case they are extracted to tmp_dir and
	 * split into <name>.swm/<name>N.swm parts (Windows setup reassembles
	 * them natively). 0 disables the check entirely.
	 */
	uint64_t max_file_size;
	const char *tmp_dir;             /* scratch dir for WIM splitting */
} rfd_copy_ctx;

/*
 * Copy the whole file tree of iso_path into dest_dir (a mounted FAT32).
 * Returns 0 on success, -ECANCELED on cancel/limit, negative errno else.
 */
int rfd_iso_copy_tree(const char *iso_path, const char *dest_dir,
	const rfd_copy_ctx *ctx);

/*
 * Metadata-only walk of the image: report every file longer than limit
 * through cb and sum the size of the whole tree into *tree_bytes (either
 * may be NULL). Cheap - no file data is read.
 */
int rfd_iso_scan_bigfiles(const char *iso_path, uint64_t limit,
	rfd_bigfile_cb cb, void *udata, uint64_t *tree_bytes);

bool rfd_filename_is_wim(const char *name);

#endif
