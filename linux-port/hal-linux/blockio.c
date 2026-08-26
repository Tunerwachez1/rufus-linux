/*
 * Rufus Linux port - Linux HAL: raw block device I/O
 * GPLv3
 */
#include "linux_hal_internal.h"
#include "../core/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/fs.h>

static int linux_open_exclusive(const char *node)
{
	int fd;

	fd = open(node, O_RDWR | O_EXCL | O_CLOEXEC);
	if (fd >= 0)
		return fd;

	{
		int err = errno;   /* rlog_err may clobber errno */

		switch (err) {
			case EACCES:
				rlog_err("'%s': permission denied - this operation requires root (run with sudo)", node);
				break;
			case EBUSY:
				rlog_err("'%s': device is busy or opened by another process", node);
				break;
			case ENOENT:
				rlog_err("'%s': no such device", node);
				break;
			default:
				rlog_err("'%s': could not open exclusively: %s", node, strerror(err));
				break;
		}
		return -err;
	}
}

static void linux_close(int fd)
{
	if (fd >= 0)
		close(fd);
}

static int linux_get_geometry(int fd, uint32_t *sector_size, uint64_t *total_bytes)
{
	uint64_t size64 = 0;
	int ssz = 0;

	if (ioctl(fd, BLKSSZGET, &ssz) != 0)
		return -errno;
	if (ioctl(fd, BLKGETSIZE64, &size64) != 0)
		return -errno;
	if (ssz <= 0)
		ssz = 512;

	if (sector_size != NULL)
		*sector_size = (uint32_t)ssz;
	if (total_bytes != NULL)
		*total_bytes = size64;
	return 0;
}

static ssize_t linux_write_at(int fd, const void *buf, size_t len, uint64_t offset)
{
	const uint8_t *p = buf;
	ssize_t done = 0;

	while ((size_t)done < len) {
		ssize_t r = pwrite(fd, p + done, len - done, (off_t)(offset + (uint64_t)done));

		if (r <= 0) {
			if ((r < 0) && (errno == EINTR))
				continue;
			if (done > 0)
				return done;
			if (r == 0)     /* unexpected EOF: leave a sane errno */
				errno = EIO;
			return -1;
		}
		done += r;
	}
	return done;
}

static ssize_t linux_read_at(int fd, void *buf, size_t len, uint64_t offset)
{
	uint8_t *p = buf;
	ssize_t done = 0;

	while ((size_t)done < len) {
		ssize_t r = pread(fd, p + done, len - done, (off_t)(offset + (uint64_t)done));

		if (r <= 0) {
			if ((r < 0) && (errno == EINTR))
				continue;
			if (done > 0)
				return done;
			if (r == 0)     /* unexpected EOF: leave a sane errno */
				errno = EIO;
			return -1;
		}
		done += r;
	}
	return done;
}

static int linux_flush(int fd)
{
	int r = fsync(fd);  /* flush our own writes */

	sync();             /* belt and braces: kernel caches for the whole device */
	return (r == 0) ? 0 : -errno;
}

static int linux_sync_data(int fd)
{
	return (fdatasync(fd) == 0) ? 0 : -errno;
}

static int linux_reread_partitions(const char *node)
{
	int fd, r;

	/*
	 * Must be called with no exclusive holder of the device left
	 * (BLKRRPART answers EBUSY otherwise), i.e. after close().
	 */
	fd = open(node, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	r = ioctl(fd, BLKRRPART);
	if (r != 0) {
		int err = errno;   /* read before close() */

		close(fd);
		return -err;
	}
	close(fd);
	return 0;
}

const rfd_hal *rufus_hal(void)
{
	static const rfd_hal hal = {
		.name               = "linux",
		.enumerate_devices  = rfd_linux_enumerate_devices,
		.open_exclusive     = linux_open_exclusive,
		.close              = linux_close,
		.get_geometry       = linux_get_geometry,
		.write_at           = linux_write_at,
		.read_at            = linux_read_at,
		.flush              = linux_flush,
		.sync_data          = linux_sync_data,
		.reread_partitions  = linux_reread_partitions,
		.list_partitions    = rfd_linux_list_partitions,
	};

	return &hal;
}

void rfd_format_size(uint64_t bytes, char *buf, size_t buf_len)
{
	const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", NULL };
	double size = (double)bytes;
	int u = 0;

	while ((units[u + 1] != NULL) && (size >= 1024.0)) {
		size /= 1024.0;
		u++;
	}
	snprintf(buf, buf_len, "%.1f %s", size, units[u]);
}
