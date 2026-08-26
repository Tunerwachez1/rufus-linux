/*
 * Rufus Linux port - stage 0 command line interface
 *
 * Subcommands:
 *   list                    enumerate writable block devices
 *   write <image> <device>  write a raw/compressed disk image (dd mode)
 *
 * Decisions (per project owner): block-device writes require root, no
 * polkit helper in stage 0; auto-update checks are removed completely.
 * GPLv3
 */
#include "../core/log.h"
#include "../core/job.h"
#include "../core/partition.h"
#include "br.h"
#include "fat32.h"
#include "../core/msys_wrap.h"
#include "../core/filesys.h"
#include "../core/isocopy.h"
#include "../hal/hal.h"

#include <bled.h>

#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

#define COPY_BUF_SIZE (4u << 20)
#define SYNC_INTERVAL (16u << 20)   /* push data to the medium this often */
#define SPEED_WINDOW  5.0           /* seconds for the rolling speed */

static volatile sig_atomic_t g_cancel = 0;      /* set by the signal handler */
static unsigned long g_cancel_bled = 0;         /* watched by bled */

/* Without periodic flushes the kernel lets gigabytes pile up in the page
 * cache and then blocks write() behind one huge writeback - the progress
 * bar freezes while the LED blinks. Flushing every SYNC_INTERVAL keeps
 * write() unblocked and makes reported progress match the medium. The
 * interval must stay small: the whole sync happens with the display
 * blocked, so it doubles as the longest possible "freeze". */
static int g_out_fd = -1;
static uint64_t g_last_flush = 0;
static bool g_flushing = false;                 /* shown as [sync] in the bar */

static void render_progress(bool force);

static void periodic_flush(uint64_t done_now)
{
	int err;

	if ((g_out_fd < 0) || (done_now - g_last_flush < SYNC_INTERVAL))
		return;
	g_flushing = true;
	render_progress(true);
	err = rufus_hal()->sync_data(g_out_fd);
	g_flushing = false;
	if (err != 0)
		rlog_warn("periodic flush failed: %s (continuing)", strerror(-err));
	else
		rlog_dbg("periodic flush: %.0f MiB now on the medium",
			(double)done_now / 1048576.0);
	g_last_flush = done_now;
}

typedef struct {
	uint64_t done;
	uint64_t total;      /* 0 = unknown */
	struct timespec start;
	struct timespec last_render;
	char current[512];   /* file being copied, for error messages */
} progress_state;

static progress_state g_prog;

/*
 * The bar redraws with \r and never ends its line; any other writer
 * (logs, phase messages) must break the line first or a stale frame stays
 * above the live one - looking like two bars, one frozen.
 */
static bool g_bar_drawn = false;

static void bar_break(void)
{
	if (g_bar_drawn) {
		fputc('\n', stderr);
		g_bar_drawn = false;
	}
}

/* Rolling speed samples: (monotonic time, bytes done at that moment). */
#define SPEED_SAMPLES 32
static struct { double t; uint64_t done; } g_speed_hist[SPEED_SAMPLES];
static int g_speed_len;

extern uint64_t bb_total_wb;   /* provided by bled/libbb */

static void on_sigint(int sig)
{
	(void)sig;
	g_cancel = 1;
	g_cancel_bled = 1;
}

static double ts_sec(const struct timespec *ts)
{
	return (double)ts->tv_sec + (double)ts->tv_nsec / 1e9;
}

/*
 * Speed over the last SPEED_WINDOW seconds, not the cumulative average:
 * during page-cache bursts the average shows inflated RAM-speed numbers,
 * while what the user needs is the rate of the medium.
 */
static double rolling_speed(double now)
{
	double span, delta;
	int n, i, oldest = -1;

	g_speed_hist[g_speed_len % SPEED_SAMPLES].t = now;
	g_speed_hist[g_speed_len % SPEED_SAMPLES].done = g_prog.done;
	g_speed_len++;

	n = (g_speed_len < SPEED_SAMPLES) ? g_speed_len : SPEED_SAMPLES;
	for (i = 1; i <= n; i++) {
		int idx = (g_speed_len - i) % SPEED_SAMPLES;

		if ((now - g_speed_hist[idx].t) > SPEED_WINDOW)
			break;
		oldest = idx;
	}
	if (oldest < 0)
		return -1.0;
	span = now - g_speed_hist[oldest].t;
	if (span < 0.5)
		return -1.0;   /* not enough history yet */
	delta = (double)(g_prog.done - g_speed_hist[oldest].done);
	return delta / span;
}

static void render_progress(bool force)
{
	char bar[41];
	struct timespec now;
	double elapsed, speed, eta;
	const char *sync_tag = g_flushing ? "[sync] " : "       ";

	if (isatty(STDERR_FILENO) == 0)
		return;

	clock_gettime(CLOCK_MONOTONIC, &now);
	elapsed = ts_sec(&now) - ts_sec(&g_prog.start);
	/* last_render keeps ABSOLUTE monotonic seconds - comparing it against
	   elapsed (seconds since start) goes negative forever after the first
	   frame and froze the bar until the end of the phase */
	if (!force && (ts_sec(&now) - ts_sec(&g_prog.last_render) < 0.4))
		return;
	g_prog.last_render = now;

	speed = rolling_speed(ts_sec(&now));
	if (speed < 0.0)
		speed = (elapsed > 0.2) ? (double)g_prog.done / elapsed : 0.0;

	memset(bar, ' ', sizeof(bar) - 1);
	bar[sizeof(bar) - 1] = '\0';

	if (g_prog.total > 0) {
		double frac = (double)g_prog.done / (double)g_prog.total;
		int fill;

		if (frac > 1.0)
			frac = 1.0;
		fill = (int)(frac * (double)(sizeof(bar) - 1));
		memset(bar, '#', fill);
		eta = (speed > 1.0 && g_prog.done > 0) ?
			(double)(g_prog.total - g_prog.done) / speed : 0.0;
		fprintf(stderr,
			"\r [%s] %3.0f%%  %.1f/%.1f MiB  %.1f MiB/s  ETA %02d:%02d  %s",
			bar, frac * 100.0,
			(double)g_prog.done / 1048576.0, (double)g_prog.total / 1048576.0,
			speed / 1048576.0, (int)eta / 60, (int)eta % 60, sync_tag);
	} else {
		fprintf(stderr, "\r [%s] %.1f MiB written  %.1f MiB/s  %s",
			bar, (double)g_prog.done / 1048576.0,
			speed / 1048576.0, sync_tag);
	}
	/* clear any tail left by a longer previous frame */
	fputs("\x1b[K", stderr);
	g_bar_drawn = true;
	fflush(stderr);
}

/* ------------------------------------------------------------------ */
/* Image compression detection                                         */

static const struct {
	const char *ext;
	int type;
} comp_map[] = {
	{ ".zip",  BLED_COMPRESSION_ZIP },
	{ ".gz",   BLED_COMPRESSION_GZIP },
	{ ".gzip", BLED_COMPRESSION_GZIP },
	{ ".bz2",  BLED_COMPRESSION_BZIP2 },
	{ ".lzma", BLED_COMPRESSION_LZMA },
	{ ".xz",   BLED_COMPRESSION_XZ },
	{ ".zst",  BLED_COMPRESSION_ZSTD },
	{ ".vtsi", BLED_COMPRESSION_VTSI },
};

static int detect_compression(const char *path)
{
	size_t len = strlen(path);

	for (size_t i = 0; i < sizeof(comp_map) / sizeof(comp_map[0]); i++) {
		size_t elen = strlen(comp_map[i].ext);

		if ((len >= elen) &&
		    (strcasecmp(path + len - elen, comp_map[i].ext) == 0))
			return comp_map[i].type;
	}
	/* compress'd files conventionally end in uppercase '.Z' */
	if ((len >= 2) && (path[len - 2] == '.') && (path[len - 1] == 'Z'))
		return BLED_COMPRESSION_LZW;
	return -1;
}

static const char *comp_name(int type)
{
	switch (type) {
		case BLED_COMPRESSION_ZIP:   return "zip";
		case BLED_COMPRESSION_GZIP:  return "gzip";
		case BLED_COMPRESSION_LZW:   return "compress";
		case BLED_COMPRESSION_LZMA:  return "lzma";
		case BLED_COMPRESSION_BZIP2: return "bzip2";
		case BLED_COMPRESSION_XZ:    return "xz";
		case BLED_COMPRESSION_VTSI:  return "vtsi";
		case BLED_COMPRESSION_ZSTD:  return "zstd";
		default:                     return "none";
	}
}

/* ------------------------------------------------------------------ */
/* Safety checks                                                       */

/*
 * Returns how many mount points are backed by the given device or any of its
 * partitions, appending their paths to mnt_out for reporting.
 */
static int count_mounted_targets(const rfd_hal *hal, const char *node,
	char *mnt_out, size_t mnt_len)
{
	dev_t targets[RFD_MAX_DEVICES];
	struct stat st;
	unsigned mj, mn;
	int ntargets, mounted = 0;
	FILE *f;
	char line[1024], root[512], mpoint[512];

	mnt_out[0] = '\0';

	if (stat(node, &st) != 0)
		return -1;
	targets[0] = st.st_rdev;
	ntargets = 1;
	if (S_ISBLK(st.st_mode))
		ntargets += hal->list_partitions(node, &targets[ntargets],
			RFD_MAX_DEVICES - ntargets);

	f = fopen("/proc/self/mountinfo", "r");
	if (f == NULL)
		return 0;

	while (fgets(line, sizeof(line), f) != NULL) {
		bool hit = false;

		if (sscanf(line, "%*d %*d %u:%u %511s %511s", &mj, &mn, root, mpoint) < 4)
			continue;
		for (int i = 0; (i < ntargets) && !hit; i++)
			hit = (targets[i] == makedev(mj, mn));
		if (!hit)
			continue;
		mounted++;
		strncat(mnt_out, " ", mnt_len - strlen(mnt_out) - 1);
		strncat(mnt_out, mpoint, mnt_len - strlen(mnt_out) - 1);
	}
	fclose(f);
	return mounted;
}

static bool confirm_destructive(const char *image_path, const char *node)
{
	const char *slash = strrchr(node, '/');
	const char *base = (slash != NULL) ? slash + 1 : node;
	char answer[64];

	fprintf(stderr,
		"\n"
		"  WARNING: ALL DATA ON '%s' WILL BE DESTROYED.\n"
		"  Image : %s\n\n", node, image_path);

	if (isatty(STDIN_FILENO) == 0) {
		rlog_err("refusing to write without confirmation: re-run with --yes");
		return false;
	}
	printf("To continue, type the device name ('%s'): ", base);
	fflush(stdout);
	if (fgets(answer, sizeof(answer), stdin) == NULL)
		return false;
	answer[strcspn(answer, "\n")] = '\0';
	return strcmp(answer, base) == 0;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */

static int cmd_list(bool usb_only)
{
	const rfd_hal *hal = rufus_hal();
	rfd_device devs[RFD_MAX_DEVICES];
	char size[32];
	int n, shown = 0;

	n = hal->enumerate_devices(devs, RFD_MAX_DEVICES);
	if (n < 0)
		return 1;

	printf("%-12s %-10s %-3s %-6s %-16.16s %-22.22s %s\n",
		"DEVICE", "SIZE", "RM", "BUS", "VENDOR", "MODEL", "SERIAL");
	for (int i = 0; i < n; i++) {
		if (usb_only && !devs[i].usb)
			continue;
		rfd_format_size(devs[i].size_bytes, size, sizeof(size));
		printf("%-12s %-10s %-3s %-6s %-16.16s %-22.22s %s\n",
			devs[i].node, size,
			devs[i].removable ? "yes" : "no",
			devs[i].bus[0] ? devs[i].bus : "?",
			devs[i].vendor, devs[i].model,
			devs[i].serial);
		shown++;
	}
	printf("\n%d device(s)%s\n", shown, usb_only ? " (USB only)" : "");
	return 0;
}

static void bled_printf_cb(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

static void bled_progress_cb(const int64_t processed)
{
	if (processed < 0) {
		g_prog.total = (uint64_t)(-processed);
		return;
	}
	g_prog.done = (uint64_t)processed;
	render_progress(false);
	periodic_flush((uint64_t)processed);
}

static int write_raw(int sfd, int dfd, const rfd_hal *hal,
	uint8_t *buf, uint64_t *written)
{
	uint64_t off = 0;

	for (;;) {
		ssize_t r = read(sfd, buf, COPY_BUF_SIZE);

		if (r < 0) {
			if (errno == EINTR) {
				if (g_cancel)
					break;
				continue;
			}
			rlog_err("read error while copying image: %s", strerror(errno));
			return -1;
		}
		if (r == 0)
			break;
		if (hal->write_at(dfd, buf, (size_t)r, off) != r) {
			rlog_err("write error on target: %s", strerror(errno));
			return -1;
		}
		off += (uint64_t)r;
		g_prog.done = off;
		render_progress(false);
		periodic_flush(off);
		if (g_cancel)
			break;
	}
	*written = off;
	return 0;
}

static int write_compressed(int sfd, int dfd, int ctype, uint64_t *written)
{
	int64_t ret;

	bled_init(COPY_BUF_SIZE, bled_printf_cb, NULL, NULL,
		bled_progress_cb, NULL, &g_cancel_bled);

	ret = bled_uncompress_with_handles((HANDLE)(intptr_t)sfd,
		(HANDLE)(intptr_t)dfd, ctype);

	bled_exit();

	if (ret < 0) {
		rlog_err("decompression failed (%s)", comp_name(ctype));
		return -1;
	}
	*written = bb_total_wb;
	g_prog.done = bb_total_wb;
	return 0;
}

/*
 * File-copy write pipeline (Windows-style): partition -> format FAT32 ->
 * mount -> copy ISO tree (splitting >4 GiB *.wim into .swm) -> install
 * boot code. Runs with root; target is a whole disk already confirmed
 * destructive by the caller.
 */
static void copy_progress_cb(uint64_t done, uint64_t total,
	const char *name, void *ud)
{
	(void)ud;
	if (name != NULL)
		snprintf(g_prog.current, sizeof(g_prog.current), "%s", name);
	g_prog.done = done;
	if (total)
		g_prog.total = total;
	render_progress(false);
	periodic_flush(done);
}

/* oversized files found by the pre-flight scan in run_copy_pipeline */
struct bigfile_state {
	int n_wim, n_other;
	uint64_t max_wim;
};

static void bigfile_cb(const char *rel, uint64_t len, bool is_wim, void *ud)
{
	struct bigfile_state *st = ud;

	if (is_wim) {
		st->n_wim++;
		if (len > st->max_wim)
			st->max_wim = len;
		rlog_warn("'%s' (%.2f GiB) exceeds the FAT32 limit - will be "
			"split into .swm parts", rel, len / 1073741824.0);
	} else {
		st->n_other++;
		rlog_err("'%s' (%.2f GiB) exceeds the FAT32 limit and is not "
			"a splittable .wim", rel, len / 1073741824.0);
	}
}

/*
 * Verify that the fd just opened still refers to the same device the user
 * inspected and confirmed (TOCTOU: udev may renumber devices while the
 * interactive confirmation is being typed).
 */
static int verify_opened_target(int dfd, const struct stat *st_ref,
	const char *node)
{
	struct stat st_now;

	if ((fstat(dfd, &st_now) != 0) || !S_ISBLK(st_now.st_mode) ||
	    (st_now.st_rdev != st_ref->st_rdev)) {
		rlog_err("target '%s' changed while preparing the write - "
			"refusing to continue", node);
		return -1;
	}
	return 0;
}

static int run_copy_pipeline(const char *image_path, const char *node,
	const struct stat *st_ref, uint64_t max_bytes)
{
	const rfd_hal *hal = rufus_hal();
	uint32_t ssz = 512;
	uint64_t total_bytes;
	char pnode[300], mnt[] = "/tmp/rufus-mnt.XXXXXX";
	int dfd, rc = 1;

	/*
	 * Pre-flight, before the target is touched: the pipeline creates a
	 * FAT32 filesystem, whose per-file ceiling is 4 GiB - 1. Oversized
	 * *.wim files are split into .swm parts automatically; anything
	 * else that big is fatal and must be reported before partitioning.
	 */
	{
		struct bigfile_state big = { 0 };
		uint64_t tree_bytes = 0;

		rc = rfd_iso_scan_bigfiles(image_path, RFD_FAT32_MAX_FILE,
			bigfile_cb, &big, &tree_bytes);
		if (rc != 0) {
			rlog_err("cannot scan '%s': %s", image_path,
				strerror(-rc));
			return 1;
		}
		if (big.n_other > 0) {
			rlog_err("%d file(s) exceed the FAT32 limit of "
				"4 GiB - 1 bytes and are not splittable "
				"WIMs", big.n_other);
			return 1;
		}
		if (big.n_wim > 0) {
			struct statvfs vfs;

			if ((statvfs("/var/tmp", &vfs) == 0) &&
			    ((uint64_t)vfs.f_bavail * vfs.f_frsize <
			     big.max_wim)) {
				rlog_err("not enough scratch space in "
					"/var/tmp to split the WIM "
					"(%.1f GiB needed)",
					big.max_wim / 1073741824.0);
				return 1;
			}
			printf("      %d WIM(s) will be split into .swm "
				"parts\n", big.n_wim);
			fflush(stdout);
		}
		g_prog.total = tree_bytes;
	}
	if (g_cancel) {
		printf("cancelled - nothing was written\n");
		return 130;
	}

	dfd = hal->open_exclusive(node);
	if (dfd < 0)
		return 1;
	if (verify_opened_target(dfd, st_ref, node) != 0) {
		hal->close(dfd);
		return 1;
	}
	if (hal->get_geometry(dfd, &ssz, &total_bytes) != 0) {
		rlog_err("could not query geometry of '%s'", node);
		goto out;
	}
	if (total_bytes < (uint64_t)2048 * ssz) {
		rlog_err("'%s' is too small for the copy pipeline "
			"(%" PRIu64 " bytes)", node, total_bytes);
		goto out;
	}

	/* 1. partition */
	{
		rfd_mbr_part part = {
			.start_lba = 2048,
			.num_lba = total_bytes / ssz - 2048,
			.type = RFD_MBR_TYPE_FAT32_LBA,
			.bootable = true,
		};

		printf("[1/4] partitioning (MBR, FAT32 LBA)\n");
		fflush(stdout);
		if (rfd_write_mbr_table(dfd, ssz, &part, 1, 0) != 0)
			goto out;
	}
	hal->flush(dfd);
	hal->close(dfd);
	dfd = -1;
	hal->reread_partitions(node);

	/* wait briefly for udev to create the partition node */
	{
		int tries;

		for (tries = 0; tries < 25; tries++) {
			if (rfd_partition_node(node, 1, pnode, sizeof(pnode),
				true) == 0)
				break;
			usleep(200000);
		}
		if (tries == 25) {
			rlog_err("kernel did not create the partition node");
			return 1;
		}
	}

	/* 2. filesystem */
	printf("[2/4] formatting %s as FAT32\n", pnode);
	fflush(stdout);
	if (rfd_format_fat32(pnode, ssz) != 0)
		return 1;

	/* 3. mount + copy */
	if (mkdtemp(mnt) == NULL) {
		rlog_err("mkdtemp failed: %s", strerror(errno));
		return 1;
	}
	if (rfd_mount(pnode, mnt) != 0) {
		rlog_err("mount failed: %s", strerror(errno));
		rmdir(mnt);
		return 1;
	}
	printf("[3/4] copying files from image\n");
	fflush(stdout);
	/*
	 * Bound writeback stalls like the dd path does: fdatasync the mounted
	 * partition every SYNC_INTERVAL (a read-only descriptor is enough)
	 * so the page cache never piles up gigabytes behind one huge flush.
	 */
	{
		int sync_fd = open(pnode, O_RDONLY | O_CLOEXEC);

		if (sync_fd >= 0) {
			g_out_fd = sync_fd;
			g_last_flush = 0;
		} else
			rlog_warn("cannot open '%s' for periodic flushes: %s",
				pnode, strerror(errno));
		{
			rfd_copy_ctx ctx = { .cancel = &g_cancel,
				.progress = copy_progress_cb, .max_bytes = max_bytes,
				.max_file_size = RFD_FAT32_MAX_FILE,
				.tmp_dir = "/var/tmp" };

			clock_gettime(CLOCK_MONOTONIC, &g_prog.start);
			rc = rfd_iso_copy_tree(image_path, mnt, &ctx);
		}
		if (sync_fd >= 0) {
			g_out_fd = -1;
			close(sync_fd);
		}
	}
	render_progress(true);
	fprintf(stderr, "\n");
	g_bar_drawn = false;

	{
		int uerr = rfd_umount(mnt);

		if (uerr != 0)
			rlog_warn("could not unmount '%s': %s - the target "
				"may be left busy", mnt, strerror(-uerr));
	}
	if (rmdir(mnt) != 0)
		rlog_dbg("could not remove '%s': %s", mnt, strerror(errno));
	if ((rc != 0) && (rc != -ECANCELED)) {
		rlog_err("copy failed on '%s': %s",
			g_prog.current[0] ? g_prog.current : "(unknown file)",
			strerror(-rc));
		return 1;
	}
	if (rc == -ECANCELED) {
		/* no boot code on a half-copied filesystem */
		printf("%s - target left in PARTIAL state\n",
			g_cancel ? "cancelled by user (Ctrl+C)" :
			"stopped by the byte limit");
		return g_cancel ? 130 : 0;
	}

	/* 4. boot code: win7 MBR (keeps our table) + FAT32 PBR */
	printf("[4/4] installing boot code\n");
	fflush(stdout);
	dfd = hal->open_exclusive(node);
	if ((dfd >= 0) && (verify_opened_target(dfd, st_ref, node) != 0)) {
		hal->close(dfd);
		dfd = -1;
		rlog_warn("target changed - skipping boot code installation");
	}
	if (dfd >= 0) {
		FAKE_FD fdisk, fpart;
		FILE *fp_disk = msys_fp(&fdisk, dfd, 0);
		FILE *fp_part = msys_fp(&fpart, dfd,
			(uint64_t)2048 * ssz);

		set_bytes_per_sector(ssz);
		if (!write_win7_mbr(fp_disk))
			rlog_warn("could not install Windows MBR");
		if (!write_fat_32_br(fp_part, false))
			rlog_warn("could not install FAT32 boot sector");
		hal->flush(dfd);
		hal->close(dfd);
	}

	if (rc == -ECANCELED)
		printf("stopped early by limit/cancel - target left "
			"in PARTIAL state\n");
	else
		printf("File-copy pipeline complete: %s is now bootable "
			"(BIOS+UEFI via FAT32)\n", node);
	return 0;

out:
	if (dfd >= 0)
		hal->close(dfd);
	return rc;
}

static int cmd_write(const char *image_path, const char *node,
	bool assume_yes, bool force, bool dry_run, bool copy_pipeline,
	uint64_t max_bytes)
{
	const rfd_hal *hal = rufus_hal();
	struct stat st_img, st_dev = { 0 };
	char mnts[2048], sizebuf[32];
	uint8_t *buf = NULL;
	uint64_t written = 0;
	int sfd = -1, dfd = -1, ctype;
	int rc = 1;
	struct timespec t_end;
	bool is_block;
	bool target_existed = false;

	if (stat(image_path, &st_img) != 0) {
		rlog_err("image '%s': %s", image_path, strerror(errno));
		return 1;
	}
	if (!S_ISREG(st_img.st_mode)) {
		rlog_err("'%s' is not a regular file", image_path);
		return 1;
	}

	if (stat(node, &st_dev) == 0) {
		is_block = S_ISBLK(st_dev.st_mode) != 0;
		target_existed = true;
	} else if (force && (errno == ENOENT)) {
		rlog_info("target '%s' does not exist yet - it will be created", node);
		is_block = false;
	} else {
		rlog_err("device '%s': %s", node, strerror(errno));
		return 1;
	}
	if (is_block && (geteuid() != 0)) {
		rlog_err("writing to a block device requires root - re-run with sudo");
		return 1;
	}
	if (!is_block && !force) {
		rlog_err("'%s' is not a block device (use --force to write to a plain file)", node);
		return 1;
	}
	if (!is_block && (node[0] != '/')) {
		/* root + O_CREAT|O_TRUNC on a relative path would silently
		   clobber whatever sits in the current directory */
		rlog_err("refusing a relative plain-file target '%s' - "
			"use an absolute path", node);
		return 1;
	}

	ctype = detect_compression(image_path);

	/* Refuse to clobber anything that is mounted right now */
	if (!force) {
		int mounted = count_mounted_targets(hal, node, mnts, sizeof(mnts));

		if (mounted < 0) {
			rlog_err("could not inspect '%s'", node);
			return 1;
		}
		if (mounted > 0) {
			rlog_err("'%s' has mounted filesystems (%s ) - unmount them or use --force",
				node, mnts);
			return 1;
		}
	} else {
		rlog_warn("--force given: skipping mounted-filesystem safety checks");
	}

	rfd_format_size((uint64_t)st_img.st_size, sizebuf, sizeof(sizebuf));
	printf("Image  : %s (%s, %s)\n", image_path, sizebuf,
		(ctype >= 0) ? comp_name(ctype) : "raw");
	printf("Target : %s%s\n", node,
		S_ISBLK(st_dev.st_mode) ? "" : " [plain file]");

	/* job engine: probe the image and build the step plan */
	if (ctype < 0) {   /* compressed images unwrap to raw/ISO downstream */
		rfd_job job;
		int jerr = rfd_job_init(&job, image_path, node, copy_pipeline);

		if (jerr != 0) {
			rlog_err("cannot plan write pipeline: %s",
				strerror(-jerr));
			return 1;
		}
		printf("Type   : %s%s\n", rfd_image_type_name(job.info.type),
			(job.info.type == RFD_IMG_ISO9660) ?
			(job.info.has_boot_signature ? " (hybrid)" : "") : "");
		if (!copy_pipeline && (job.info.type == RFD_IMG_ISO9660))
			printf("Info   : hybrid ISO - dd keeps its own boot code\n");
		if (copy_pipeline) {
			char plan[1024];

			rfd_job_describe(&job, plan, sizeof(plan));
			fputs(plan, stdout);
		}
		if (!job.supported) {
			rlog_err("pipeline step unsupported in this build");
			return 1;
		}
	}
	fflush(stdout);

	if (dry_run) {
		printf("Dry run: all safety checks passed, nothing was written.\n");
		return 0;
	}

	if ((!assume_yes) && (!confirm_destructive(image_path, node))) {
		rlog_err("aborted by user");
		return 1;
	}

	/*
	 * The handler must be active for the copy pipeline too: without it
	 * a ^C kills the process outright and leaves the mount point under
	 * /tmp/rufus-mnt.* and the WIM scratch file under /var/tmp behind.
	 */
	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	if (copy_pipeline && (ctype < 0) && is_block)
		return run_copy_pipeline(image_path, node, &st_dev, max_bytes);
	if (copy_pipeline && (ctype < 0)) {
		rlog_err("--copy requires a real block device target");
		return 1;
	}

	buf = malloc(COPY_BUF_SIZE);
	if (buf == NULL) {
		rlog_err("out of memory");
		return 1;
	}

	if (is_block) {
		dfd = hal->open_exclusive(node);
	} else {
		dfd = open(node, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (dfd < 0)
			rlog_err("cannot create '%s': %s", node, strerror(errno));
	}
	if (dfd < 0)
		goto out;
	/* the user confirmed this exact target: make sure the node we just
	   opened is still that target (it may have been swapped meanwhile) */
	if (target_existed) {
		struct stat st_now;

		if ((fstat(dfd, &st_now) != 0) ||
		    (S_ISBLK(st_now.st_mode) != is_block) ||
		    (st_now.st_dev != st_dev.st_dev) ||
		    (st_now.st_ino != st_dev.st_ino)) {
			rlog_err("target '%s' changed while preparing the "
				"write - refusing to continue", node);
			goto out;
		}
	}
	sfd = open(image_path, O_RDONLY | O_CLOEXEC);
	if (sfd < 0) {
		rlog_err("cannot open image: %s", strerror(errno));
		goto out;
	}

	g_out_fd = dfd;
	g_last_flush = 0;

	clock_gettime(CLOCK_MONOTONIC, &g_prog.start);

	if (ctype >= 0) {
		if (write_compressed(sfd, dfd, ctype, &written) != 0)
			goto out;
	} else {
		g_prog.total = (uint64_t)st_img.st_size;
		if (write_raw(sfd, dfd, hal, buf, &written) != 0)
			goto out;
	}

	g_out_fd = -1;      /* no more periodic flushes past this point */
	{
		int err = hal->flush(dfd);

		if ((err != 0) && !g_cancel) {
			rlog_err("final flush failed: %s", strerror(-err));
			goto out;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &t_end);

	render_progress(true);
	if (isatty(STDERR_FILENO))
		fprintf(stderr, "\n");
	g_bar_drawn = false;

	if (g_cancel) {
		rlog_warn("cancelled by user after %.1f MiB - target left in PARTIAL state",
			(double)written / 1048576.0);
		rc = 130;
		goto out;
	}

	{
		double secs = ts_sec(&t_end) - ts_sec(&g_prog.start);

		printf("%.1f MiB written in %.1fs (avg %.1f MiB/s)\n",
			(double)written / 1048576.0, secs,
			(secs > 0.01) ? (double)written / secs / 1048576.0 : 0.0);
	}
	rc = 0;

out:
	g_out_fd = -1;
	if (sfd >= 0)
		close(sfd);
	if (dfd >= 0)
		hal->close(dfd);
	free(buf);
	return rc;
}

/*
 * mktable <target> --gpt|--mbr
 *
 * Writes a fresh partition table with one partition spanning the usable
 * area (GPT: EFI System; MBR: FAT32 LBA from a 1 MiB alignment). Shares
 * the safety rails of 'write': root for block devices, mounted-FS check,
 * interactive confirmation. On success the kernel re-reads the table.
 */
static int cmd_mktable(const char *node, bool gpt, bool assume_yes, bool force)
{
	const rfd_hal *hal = rufus_hal();
	struct stat st;
	char mnts[2048], sizebuf[32];
	uint32_t sector_size = 512;
	uint64_t total_bytes;
	int dfd = -1, rc = 1;
	bool is_block;

	if (stat(node, &st) != 0) {
		rlog_err("target '%s': %s", node, strerror(errno));
		return 1;
	}
	is_block = S_ISBLK(st.st_mode) != 0;
	if (is_block && (geteuid() != 0)) {
		rlog_err("partitioning a block device requires root - re-run with sudo");
		return 1;
	}
	if (!is_block && !force) {
		rlog_err("'%s' is not a block device (use --force for a plain file)", node);
		return 1;
	}

	if (!force) {
		int mounted = count_mounted_targets(hal, node, mnts, sizeof(mnts));

		if ((mounted > 0) && is_block) {
			rlog_err("'%s' has mounted filesystems (%s ) - unmount them or use --force",
				node, mnts);
			return 1;
		}
	}

	dfd = is_block ? hal->open_exclusive(node) :
		open(node, O_RDWR | O_CLOEXEC);
	if (dfd < 0) {
		rlog_err("cannot open '%s': %s", node,
			is_block ? strerror(-dfd) : strerror(errno));
		return 1;
	}

	if (is_block) {
		if (hal->get_geometry(dfd, &sector_size, &total_bytes) != 0) {
			rlog_err("could not query geometry of '%s'", node);
			goto out;
		}
	} else {
		total_bytes = (uint64_t)st.st_size;
	}
	if (total_bytes < (uint64_t)2048 * sector_size) {
		rlog_err("'%s' is too small (%" PRIu64 " bytes)", node, total_bytes);
		goto out;
	}

	rfd_format_size(total_bytes, sizebuf, sizeof(sizebuf));
	printf("Target : %s%s\n", node, is_block ? "" : " [plain file]");
	printf("Size   : %s (sector %u)\n", sizebuf, sector_size);
	fflush(stdout);

	if ((!assume_yes) &&
	    (!confirm_destructive(gpt ? "GPT partition table" :
	     "MBR partition table", node))) {
		rlog_err("aborted by user");
		goto out;
	}

	if (gpt) {
		uint64_t entries_sectors = 16384ULL / sector_size;
		uint64_t first_usable = 2 + entries_sectors;
		uint64_t last_usable = total_bytes / sector_size - 1 -
			entries_sectors - 1;
		rfd_gpt_part part;

		part.start_lba = first_usable;
		part.end_lba = last_usable;
		part.type_guid = rfd_guid_esp;
		part.name = "EFI System";
		if (rfd_write_gpt(dfd, sector_size, total_bytes, &part, 1) != 0)
			goto out;
		printf("Created: GPT with EFI System partition %"
			PRIu64 "-%" PRIu64 "\n", part.start_lba, part.end_lba);
	} else {
		rfd_mbr_part part;

		part.start_lba = 2048;    /* 1 MiB alignment */
		part.num_lba = total_bytes / sector_size - part.start_lba;
		part.type = RFD_MBR_TYPE_FAT32_LBA;
		part.bootable = true;
		if (rfd_write_mbr_table(dfd, sector_size, &part, 1, 0) != 0)
			goto out;
		printf("Created: MBR with FAT32(LBA) partition start=%"
			PRIu64 " sectors=%" PRIu64 " (active)\n",
			part.start_lba, part.num_lba);
	}

	if (is_block) {
		int err;

		/* release the exclusive claim first: BLKRRPART refuses to run
		   while the caller itself holds the device with O_EXCL */
		hal->close(dfd);
		dfd = -1;
		err = hal->reread_partitions(node);
		if (err != 0)
			rlog_warn("could not re-read the partition table (%s)"
				" - unplug/replug or run partprobe",
				strerror(-err));
	}
	rc = 0;

out:
	if (dfd >= 0)
		hal->close(dfd);
	return rc;
}

/* ------------------------------------------------------------------ */

static void usage(FILE *f)
{
	fprintf(f,
		"rufus-cli - Rufus Linux port (stage 0 prototype + stage 1 core)\n"
		"\n"
		"Usage:\n"
		"  rufus-cli list [--usb]\n"
		"  rufus-cli write <image> <device> [--yes] [--force] [--dry-run]\n"
		"  rufus-cli mktable <target> --gpt|--mbr [--yes] [--force]\n"
		"\n"
		"Options:\n"
		"  --usb       'list': show USB devices only\n"
		"  --gpt       'mktable': write a GPT with one EFI System partition\n"
		"  --mbr       'mktable': write an MBR with one active FAT32 partition\n"
		"  --copy      'write': request the file-copy pipeline (ISO9660;\n"
		"              stage-1 WIP - the plan is shown, execution refused)\n"
		"  --yes       skip interactive confirmation (for scripts)\n"
		"  --force     allow non-block targets, skip mounted-checks\n"
		"  --dry-run   run all safety checks without writing\n"
		"  -v/--verbose debug logging\n"
		"  -h/--help   this help\n"
		"\n"
		"Supported images: raw (.iso/.img) and compressed\n"
		"(.zip/.gz/.xz/.zst/.bz2/.lzma/.vtsi/.Z) via bled.\n");
}

int main(int argc, char **argv)
{
	bool usb_only = false, assume_yes = false, force = false, dry_run = false;
	bool gpt_table = false, mbr_table = false, copy_pipeline = false;
	uint64_t max_bytes = 0;
	const char *pos[3] = { NULL, NULL, NULL };
	int npos = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	rlog_set_linebreak_hook(bar_break);

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if ((strcmp(arg, "-h") == 0) || (strcmp(arg, "--help") == 0)) {
			usage(stdout);
			return 0;
		} else if ((strcmp(arg, "-v") == 0) || (strcmp(arg, "--verbose") == 0)) {
			rlog_set_level(RLOG_DEBUG);
		} else if (strcmp(arg, "--usb") == 0) {
			usb_only = true;
		} else if (strcmp(arg, "--yes") == 0) {
			assume_yes = true;
		} else if (strcmp(arg, "--force") == 0) {
			force = true;
		} else if (strcmp(arg, "--dry-run") == 0) {
			dry_run = true;
		} else if (strcmp(arg, "--gpt") == 0) {
			gpt_table = true;
		} else if (strcmp(arg, "--mbr") == 0) {
			mbr_table = true;
		} else if (strcmp(arg, "--copy") == 0) {
			copy_pipeline = true;
		} else if (strncmp(arg, "--max-mb=", 9) == 0) {
			max_bytes = (uint64_t)strtoull(arg + 9, NULL, 10) <<
				20;
		} else if (arg[0] == '-') {
			rlog_err("unknown option '%s'", arg);
			usage(stderr);
			return 2;
		} else if (npos < 3) {
			pos[npos++] = arg;
		} else {
			rlog_err("unexpected argument '%s'", arg);
			return 2;
		}
	}

	if (npos == 0) {
		usage(stderr);
		return 2;
	}

	if (strcmp(pos[0], "list") == 0)
		return cmd_list(usb_only);

	if (strcmp(pos[0], "write") == 0) {
		if (npos != 3) {
			rlog_err("'write' needs exactly an image and a target device");
			usage(stderr);
			return 2;
		}
		return cmd_write(pos[1], pos[2], assume_yes, force, dry_run,
			copy_pipeline, max_bytes);
	}

	if (strcmp(pos[0], "mktable") == 0) {
		if (npos != 2) {
			rlog_err("'mktable' needs exactly one target");
			usage(stderr);
			return 2;
		}
		if (gpt_table == mbr_table) {
			rlog_err("'mktable' needs exactly one of --gpt or --mbr");
			return 2;
		}
		if (dry_run) {
			printf("Dry run: all safety checks passed, nothing was written.\n");
			return 0;
		}
		return cmd_mktable(pos[1], gpt_table, assume_yes, force);
	}

	rlog_err("unknown command '%s' (expected 'list', 'write' or 'mktable')",
		pos[0]);
	usage(stderr);
	return 2;
}
