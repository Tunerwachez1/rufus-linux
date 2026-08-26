/*
 * Rufus Linux port - copy an ISO's file tree onto a mounted filesystem.
 *
 * Every fwrite is checked and the written size is verified against the
 * source length: a filesystem that silently caps file size (FAT32 at
 * 4 GiB - 1) must abort the copy loudly instead of leaving a truncated
 * image behind. Oversized *.wim files are extracted to a scratch dir and
 * split into .swm parts via vendored wimlib; Windows setup reassembles
 * them natively.
 * GPLv3
 */
#include "isocopy.h"
#include "log.h"

#include <cdio/cdio.h>
#include <cdio/iso9660.h>
#include <cdio/logging.h>
#include <cdio/udf.h>

#include <wimlib.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define COPY_BLOCKSIZE 2048
#define COPY_BUF_BLOCKS 1024          /* 2 MiB per read */
/* guard against crafted images with absurd directory nesting (stack DoS) */
#define RFD_MAX_TREE_DEPTH 64

static const rfd_copy_ctx *g_ctx;
static uint64_t g_done, g_total;
/* file being copied right now - progress callbacks fire mid-file too */
static char g_cur_name[512];

/*
 * Names coming out of an image are attacker-controlled data. Refuse any
 * name that could escape dest_dir: separators inside a component, empty
 * names and the "." / ".." components. Such entries are skipped.
 */
static bool name_is_safe(const char *name)
{
	if ((name == NULL) || (name[0] == '\0'))
		return false;
	if ((strchr(name, '/') != NULL) || (strchr(name, '\\') != NULL))
		return false;
	return (strcmp(name, ".") != 0) && (strcmp(name, "..") != 0);
}

static void set_current(const char *name)
{
	snprintf(g_cur_name, sizeof(g_cur_name), "%s",
		(name != NULL) ? name : "");
}

/* name == NULL re-reports the current file without changing it */
static int report(const char *name)
{
	if (name != NULL)
		set_current(name);
	if (g_ctx->cancel && *g_ctx->cancel)
		return -ECANCELED;
	if (g_ctx->progress != NULL)
		g_ctx->progress(g_done, g_total, g_cur_name, g_ctx->udata);
	return 0;
}

bool rfd_filename_is_wim(const char *name)
{
	size_t len = strlen(name);

	return (len > 4) && (strcasecmp(name + len - 4, ".wim") == 0);
}

/* map a failed stdio write to a meaningful errno */
static int write_err(FILE *out)
{
	int e = ferror(out) ? errno : EIO;

	if (e == 0)
		e = EIO;
	return -e;
}

/*
 * Close a fully written stream: inspect the error state while out is
 * still valid, then fold close failures (final flush hitting ENOSPC etc.)
 * into the result.
 */
static int close_checked(FILE *out)
{
	int werr = ferror(out) ? errno : 0;
	int cerr;

	errno = 0;
	cerr = fclose(out);
	if (cerr != 0)
		return (errno != 0) ? -errno :
			(werr != 0 ? -werr : -EIO);
	return (werr != 0) ? -werr : 0;
}

/*
 * half_rate: while staging an oversized WIM into scratch space only half
 * of its bytes advance progress - writing the .swm parts afterwards adds
 * the other half, so the file still counts exactly len units when done.
 */
static int write_file_data(FILE *out, uint64_t len,
	int (*reader)(void *ud, void *buf, long nblocks), void *ud,
	bool half_rate)
{
	uint8_t *buf = malloc(COPY_BUF_BLOCKS * COPY_BLOCKSIZE);
	uint64_t pos = 0, half_rem = 0;
	int ticks = 0;

	if (buf == NULL)
		return -ENOMEM;
	while (pos < len) {
		uint64_t remain = len - pos;
		long blocks = (long)((remain + COPY_BLOCKSIZE - 1) /
			COPY_BLOCKSIZE);
		size_t chunk, put;
		ssize_t got;
		int rrc;

		if (blocks > COPY_BUF_BLOCKS)
			blocks = COPY_BUF_BLOCKS;
		got = reader(ud, buf, blocks);
		if (got <= 0) {
			free(buf);
			return -EIO;
		}
		/*
		 * The last read is rounded up to whole 2048-byte blocks by
		 * both layers; advance by the exact number of payload bytes
		 * so the final length check below sees len, not the padded
		 * read size.
		 */
		chunk = (size_t)((remain < (uint64_t)got) ? remain :
			(uint64_t)got);
		put = fwrite(buf, 1, chunk, out);
		if (put != chunk) {
			int rc = write_err(out);

			free(buf);
			return rc;
		}
		pos += chunk;
		if (half_rate) {
			uint64_t acc = half_rem + chunk;

			g_done += acc >> 1;
			half_rem = acc & 1;
		} else {
			g_done += chunk;
		}
		/*
		 * A single large file used to keep the bar frozen for
		 * minutes; report every ~16 MiB so it keeps moving.
		 */
		if ((++ticks & 7) == 0) {
			rrc = report(NULL);
			if (rrc != 0) {
				free(buf);
				return rrc;
			}
		}
		if (g_ctx->cancel && *g_ctx->cancel) {
			free(buf);
			return -ECANCELED;
		}
	}
	free(buf);
	/*
	 * Flush here so a filesystem size cap surfaces as an error from
	 * this function instead of silently truncating the file.
	 */
	if ((fflush(out) != 0) || (len != pos))
		return (errno == EFBIG) ? -EFBIG : -EIO;
	return 0;
}

struct wim_split_prog {
	uint64_t base;       /* g_done value when the split began */
	uint64_t len;        /* WIM file length in progress units */
	uint64_t done_out;   /* output bytes copied out in finished parts */
	uint64_t cur_max;    /* monotonic guard within the current part */
	uint64_t split_total;/* total resource bytes to copy out (from wimlib) */
};

/*
 * The split must carry g_done from base + len/2 (staging consumed the
 * first half) up to base + len. wimlib restarts its per-part counters,
 * and write_streams.completed_bytes counts UNCOMPRESSED bytes (it can
 * exceed the file size), so use completed_compressed_bytes clamped by a
 * running maximum, plus the authoritative cumulative split.completed_bytes
 * at each part boundary. A pending ^C aborts wimlib promptly instead of
 * waiting out the multi-minute split.
 */
static enum wimlib_progress_status wim_split_progress(
	enum wimlib_progress_msg msg, union wimlib_progress_info *info,
	void *ud)
{
	struct wim_split_prog *p = ud;
	uint64_t frac_num;

	switch (msg) {
		case WIMLIB_PROGRESS_MSG_WRITE_STREAMS:
			if (info->write_streams.completed_compressed_bytes >
			    p->cur_max)
				p->cur_max =
					info->write_streams.completed_compressed_bytes;
			frac_num = p->done_out + p->cur_max;
			break;
		case WIMLIB_PROGRESS_MSG_SPLIT_BEGIN_PART:
			p->split_total = info->split.total_bytes;
			p->cur_max = 0;
			return (g_ctx->cancel && *g_ctx->cancel) ?
				WIMLIB_PROGRESS_STATUS_ABORT :
				WIMLIB_PROGRESS_STATUS_CONTINUE;
		case WIMLIB_PROGRESS_MSG_SPLIT_END_PART:
			p->done_out = info->split.completed_bytes;
			p->cur_max = 0;
			frac_num = p->done_out;
			break;
		default:
			return (g_ctx->cancel && *g_ctx->cancel) ?
				WIMLIB_PROGRESS_STATUS_ABORT :
				WIMLIB_PROGRESS_STATUS_CONTINUE;
	}
	if ((p->split_total > 0) && (frac_num > p->split_total))
		frac_num = p->split_total;
	if (p->split_total > 0) {
		g_done = p->base + p->len / 2 +
			frac_num * (p->len - p->len / 2) / p->split_total;
		report(NULL);
	}
	return (g_ctx->cancel && *g_ctx->cancel) ?
		WIMLIB_PROGRESS_STATUS_ABORT :
		WIMLIB_PROGRESS_STATUS_CONTINUE;
}

/*
 * Extract one oversized *.wim through `reader` into tmp_dir and split it
 * into FAT32-safe .swm parts next to the other copied files. dpath holds
 * the destination path of the would-be .wim; the parts are named by
 * replacing the .wim suffix with .swm (part 1) and inserting part numbers
 * before the suffix (install2.swm, ...) per Microsoft convention.
 */
static int copy_split_wim(const char *dpath, const char *rel,
	uint64_t len, int (*reader)(void *ud, void *buf, long nblocks),
	void *ud)
{
	char tpl[4096], swm[4096];
	const char *tmpdir = (g_ctx->tmp_dir != NULL) ? g_ctx->tmp_dir :
		"/var/tmp";
	const char *fname;
	size_t flen, base;
	struct wim_split_prog prog = { .base = g_done, .len = len };
	WIMStruct *wim = NULL;
	FILE *out;
	int tfd, rc;

	/* the scratch file must NOT live on the target FAT32: a >4 GiB wim
	   would hit the very ceiling the split exists to avoid */
	base = strlen(dpath);
	if ((base < 4) || (base + 16 > sizeof(swm)) ||
	    (strcasecmp(dpath + base - 4, ".wim") != 0))
		return -EINVAL;
	fname = strrchr(dpath, '/');
	fname = (fname != NULL) ? fname + 1 : dpath;
	flen = strlen(fname);
	if ((flen < 5) || (flen > sizeof(tpl) - 32))
		return -EINVAL;
	snprintf(tpl, sizeof(tpl), "%s/%.*s.split-XXXXXX", tmpdir,
		(int)(flen - 4), fname);
	rlog_info("'%s' (%.2f GiB): staging to scratch, then writing .swm "
		"parts - this phase takes a few minutes", rel,
		len / 1073741824.0);

	tfd = mkstemp(tpl);
	if (tfd < 0)
		return -errno;
	out = fdopen(tfd, "wb");
	if (out == NULL) {
		rc = -errno;
		close(tfd);
		unlink(tpl);
		return rc;
	}
	set_current(rel);
	rc = write_file_data(out, len, reader, ud, true);
	if (rc == 0)
		rc = close_checked(out);
	else
		fclose(out);
	if (rc != 0) {
		unlink(tpl);
		return rc;
	}
	/* drop the half_rate rounding so accounting stays exact */
	g_done = prog.base + len / 2;

	wimlib_set_print_errors(true);
	rc = wimlib_open_wim(tpl, 0, &wim);
	if (rc != 0) {
		rlog_err("wimlib cannot open extracted '%s': %s", rel,
			wimlib_get_error_string((enum wimlib_error_code)rc));
		unlink(tpl);
		return -EIO;
	}
	wimlib_register_progress_function(wim, wim_split_progress, &prog);
	snprintf(swm, sizeof(swm), "%.*s.swm", (int)(base - 4), dpath);
	rc = wimlib_split(wim, swm, RFD_WIM_SPLIT_CHUNK, 0);
	wimlib_free(wim);
	unlink(tpl);
	if (rc != 0) {
		/* a user ^C is handled as a normal cancel upstream - not
		   an error worth an alarming message */
		if (rc != WIMLIB_ERR_ABORTED_BY_PROGRESS)
			rlog_err("splitting '%s' failed: %s", rel,
				wimlib_get_error_string(
					(enum wimlib_error_code)rc));
		return (rc == WIMLIB_ERR_ABORTED_BY_PROGRESS) ? -ECANCELED :
			-EIO;
	}
	g_done = prog.base + len;
	report(rel);
	return 0;
}

/* ---------------- UDF layer --------------------------------------- */

struct udf_reader {
	udf_dirent_t *f;
};

static int udf_reader(void *ud, void *buf, long nblocks)
{
	struct udf_reader *r = ud;

	/* udf_read_block returns the number of bytes actually read: pass it
	   through unchanged so a short read is caught by write_file_data
	   instead of writing an uninitialized buffer tail to the target */
	return (int)udf_read_block(r->f, buf, nblocks);
}

static int udf_walk(udf_dirent_t *dir, const char *dst, const char *rel,
	int depth)
{
	/* mirrors iso.c udf_extract_files(): chain readdir at this level,
	   descend into subdirs via udf_opendir(child) + fresh iterator */
	udf_dirent_t *it = dir;
	int rc;

	if (depth >= RFD_MAX_TREE_DEPTH) {
		rlog_err("image directory nesting exceeds %d levels - "
			"refusing to descend further", RFD_MAX_TREE_DEPTH);
		return -ELOOP;
	}

	for (;;) {
		const char *name;
		char dpath[4096], rpath[512];
		int n;

		it = udf_readdir(it);
		if (it == NULL) {
			rc = 0;
			break;
		}
		name = udf_get_filename(it);
		if (!name_is_safe(name))
			continue;
		n = snprintf(dpath, sizeof(dpath), "%s/%s", dst, name);
		if ((n < 0) || ((size_t)n >= sizeof(dpath))) {
			rlog_dbg("skipping '%s': destination path too long",
				name);
			continue;
		}
		snprintf(rpath, sizeof(rpath), "%s/%s",
			rel[0] ? rel : ".", name);

		if (udf_is_dir(it)) {
			if ((mkdir(dpath, 0755) != 0) && (errno != EEXIST)) {
				rc = -errno;
				break;
			}
			{
				udf_dirent_t *child = udf_opendir(it);

				if (child == NULL)
					continue;
				rc = udf_walk(child, dpath, rpath, depth + 1);
				if (rc != 0)
					break;
			}
		} else {
			uint64_t len = udf_get_file_length(it);
			struct udf_reader rd = { .f = it };
			bool oversize = (g_ctx->max_file_size != 0) &&
				(len > g_ctx->max_file_size);

			if (oversize && !rfd_filename_is_wim(name)) {
				rlog_err("'%s' is %" PRIu64 " bytes - exceeds "
					"the target filesystem limit and is "
					"not a splittable .wim", rpath, len);
				rc = -EFBIG;
				break;
			}
			if (oversize) {
				rc = copy_split_wim(dpath, rpath, len,
					udf_reader, &rd);
				if (rc != 0)
					break;
			} else {
				FILE *out;

				set_current(rpath);
				out = fopen(dpath, "wb");
				if (out == NULL) {
					rc = -errno;
					break;
				}
				rc = write_file_data(out, len, udf_reader, &rd,
					false);
				if (rc == 0)
					rc = close_checked(out);
				else
					fclose(out);
				if (rc != 0)
					break;
			}
			report(rpath);
			if (rc != 0)
				break;
			if ((g_ctx->max_bytes) && (g_done >= g_ctx->max_bytes))
				return -ECANCELED;
		}
	}
	return rc;
}

/* ---------------- ISO9660 layer ------------------------------------ */

struct iso_reader {
	iso9660_t *iso;
	lsn_t lsn;
	uint64_t pos;
};

static int iso_reader(void *ud, void *buf, long nblocks)
{
	struct iso_reader *r = ud;
	long n = iso9660_iso_seek_read(r->iso, buf, r->lsn +
		r->pos / COPY_BLOCKSIZE, nblocks);

	if (n > 0)
		r->pos += n;
	return n;
}

static int iso_walk(iso9660_t *iso, const char *srcdir, const char *dst,
	const char *rel, int depth)
{
	CdioList_t *list = iso9660_ifs_readdir(iso, srcdir);
	CdioListNode_t *node;
	char child[4096];
	int rc = 0;

	if (list == NULL)
		return -EIO;
	if (depth >= RFD_MAX_TREE_DEPTH) {
		rlog_err("image directory nesting exceeds %d levels - "
			"refusing to descend further", RFD_MAX_TREE_DEPTH);
		_cdio_list_free(list, true, free);
		return -ELOOP;
	}
	for (node = _cdio_list_begin(list); node != NULL;
	     node = _cdio_list_node_next(node)) {
		iso9660_stat_t *e = _cdio_list_node_data(node);
		const char *name = e->filename;

		if (!name_is_safe(name))
			continue;
		if ((e->type == _STAT_DIR) &&
		    (snprintf(child, sizeof(child), "%s/%s", srcdir, name) <
		     (int)sizeof(child))) {
			char dpath[4096], rpath[512];
			int n;

			n = snprintf(dpath, sizeof(dpath), "%s/%s", dst, name);
			if ((n < 0) || ((size_t)n >= sizeof(dpath))) {
				rlog_dbg("skipping '%s': destination path "
					"too long", name);
				continue;
			}
			snprintf(rpath, sizeof(rpath), "%s/%s",
				rel[0] ? rel : ".", name);
			if (((mkdir(dpath, 0755) != 0) && (errno != EEXIST)) ||
			    ((rc = iso_walk(iso, child, dpath, rpath,
			     depth + 1)) != 0))
				break;
			continue;
		}
		if (e->type != _STAT_FILE)
			continue;   /* symlinks etc. skipped */

		{
			char fpath[4096], rpath[512];
			struct iso_reader rd = { .iso = iso, .lsn = e->lsn,
				.pos = 0 };
			bool oversize = (g_ctx->max_file_size != 0) &&
				(e->total_size > g_ctx->max_file_size);
			int n = snprintf(fpath, sizeof(fpath), "%s/%s",
				dst, name);

			if ((n < 0) || ((size_t)n >= sizeof(fpath))) {
				rlog_dbg("skipping '%s': destination path "
					"too long", name);
				continue;
			}
			snprintf(rpath, sizeof(rpath), "%s/%s",
				rel[0] ? rel : ".", name);
			set_current(rpath);
			if (oversize && !rfd_filename_is_wim(name)) {
				rlog_err("'%s' is %" PRIu64 " bytes - exceeds "
					"the target filesystem limit and is "
					"not a splittable .wim", rpath,
					e->total_size);
				rc = -EFBIG;
				break;
			}
			if (oversize) {
				rc = copy_split_wim(fpath, rpath,
					e->total_size, iso_reader, &rd);
				if (rc != 0)
					break;
			} else {
				FILE *out = fopen(fpath, "wb");

				if (out == NULL) {
					rc = -errno;
					break;
				}
				rc = write_file_data(out, e->total_size,
					iso_reader, &rd, false);
				if (rc == 0)
					rc = close_checked(out);
				else
					fclose(out);
				if (rc != 0)
					break;
			}
			report(rpath);
			if (rc != 0)
				break;
			if ((g_ctx->max_bytes) && (g_done >= g_ctx->max_bytes)) {
				rc = -ECANCELED;
				break;
			}
		}
	}
	_cdio_list_free(list, true, free);
	return rc;
}

/* ---------------- entry point -------------------------------------- */

int rfd_iso_copy_tree(const char *iso_path, const char *dest_dir,
	const rfd_copy_ctx *ctx)
{
	
	int rc;

	g_ctx = ctx;
	g_done = 0;
	g_total = 0;

	{
		udf_t *p_udf = udf_open(iso_path);

		if (p_udf != NULL) {
			udf_dirent_t *root = udf_get_root(p_udf, true, 0);

			rlog_dbg("copying from UDF layer");
			rc = root ? udf_walk(root, dest_dir, "", 0) : -EIO;
			udf_close(p_udf);
			return rc;
		}
	}

	{
		iso9660_t *iso = iso9660_open_ext(iso_path,
			ISO_EXTENSION_ALL);

		rlog_dbg("copying from ISO9660 layer");
		if (iso == NULL)
			return -EIO;
		rc = iso_walk(iso, "/", dest_dir, "", 0);
		iso9660_close(iso);
	}
	return rc;
}

/* ---------------- big-file scan (metadata only) -------------------- */

static uint64_t g_limit, g_scan_total;
static rfd_bigfile_cb g_bigcb;
static void *g_bigud;

static void scan_note(const char *rpath, uint64_t len)
{
	g_scan_total += len;
	if ((len > g_limit) && (g_bigcb != NULL))
		g_bigcb(rpath, len, rfd_filename_is_wim(rpath), g_bigud);
}

static int udf_scan(udf_dirent_t *dir, const char *rel, int depth)
{
	udf_dirent_t *it = dir;
	int rc;

	if (depth >= RFD_MAX_TREE_DEPTH)
		return -ELOOP;

	for (;;) {
		const char *name;
		char rpath[512];

		it = udf_readdir(it);
		if (it == NULL)
			return 0;
		name = udf_get_filename(it);
		if (!name_is_safe(name))
			continue;
		snprintf(rpath, sizeof(rpath), "%s/%s",
			rel[0] ? rel : ".", name);

		if (udf_is_dir(it)) {
			udf_dirent_t *child = udf_opendir(it);

			if (child == NULL)
				continue;
			rc = udf_scan(child, rpath, depth + 1);
			if (rc != 0)
				return rc;
		} else {
			scan_note(rpath, udf_get_file_length(it));
		}
	}
}

static int iso_scan(iso9660_t *iso, const char *srcdir, const char *rel,
	int depth)
{
	CdioList_t *list = iso9660_ifs_readdir(iso, srcdir);
	CdioListNode_t *node;
	char child[4096];
	int rc = 0;

	if (list == NULL)
		return -EIO;
	if (depth >= RFD_MAX_TREE_DEPTH) {
		_cdio_list_free(list, true, free);
		return -ELOOP;
	}
	for (node = _cdio_list_begin(list); node != NULL;
	     node = _cdio_list_node_next(node)) {
		iso9660_stat_t *e = _cdio_list_node_data(node);
		const char *name = e->filename;

		if (!name_is_safe(name))
			continue;
		if ((e->type == _STAT_DIR) &&
		    (snprintf(child, sizeof(child), "%s/%s", srcdir, name) <
		     (int)sizeof(child))) {
			char rpath[512];

			snprintf(rpath, sizeof(rpath), "%s/%s",
				rel[0] ? rel : ".", name);
			rc = iso_scan(iso, child, rpath, depth + 1);
			if (rc != 0)
				break;
			continue;
		}
		if (e->type == _STAT_FILE) {
			/* report the full relative path like the UDF branch */
			char rpath[512];

			snprintf(rpath, sizeof(rpath), "%s/%s",
				rel[0] ? rel : ".", name);
			scan_note(rpath, e->total_size);
		}
	}
	_cdio_list_free(list, true, free);
	return rc;
}

int rfd_iso_scan_bigfiles(const char *iso_path, uint64_t limit,
	rfd_bigfile_cb cb, void *udata, uint64_t *tree_bytes)
{
	int rc;

	g_limit = limit;
	g_bigcb = cb;
	g_bigud = udata;
	g_scan_total = 0;

	{
		udf_t *p_udf = udf_open(iso_path);

		if (p_udf != NULL) {
			udf_dirent_t *root = udf_get_root(p_udf, true, 0);

			rc = root ? udf_scan(root, "", 0) : -EIO;
			udf_close(p_udf);
			if (tree_bytes != NULL)
				*tree_bytes = g_scan_total;
			return rc;
		}
	}

	{
		iso9660_t *iso = iso9660_open_ext(iso_path,
			ISO_EXTENSION_ALL);

		if (iso == NULL)
			return -EIO;
		rc = iso_scan(iso, "/", "", 0);
		iso9660_close(iso);
	}
	if (tree_bytes != NULL)
		*tree_bytes = g_scan_total;
	return rc;
}
