/*
 * Rufus Linux port - Hardware Abstraction Layer interface
 *
 * The core talks to storage hardware exclusively through this vtable so that
 * the same job engine can later run on top of hal-linux and (optionally) a
 * restored hal-win32.
 * GPLv3
 */
#ifndef RUFUS_HAL_H
#define RUFUS_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define RFD_MAX_DEVICES        64
#define RFD_NODE_MAX           256
#define RFD_NAME_MAX           96

typedef struct {
	char node[RFD_NODE_MAX];       /* e.g. "/dev/sdb" */
	char vendor[RFD_NAME_MAX];
	char model[RFD_NAME_MAX];
	char serial[RFD_NAME_MAX];
	char bus[RFD_NAME_MAX];        /* "usb", "ata", "virtio"... */
	uint64_t size_bytes;
	uint32_t sector_size;
	bool removable;
	bool usb;
	bool readonly_device;          /* write-protected as reported by kernel */
} rfd_device;

typedef struct rfd_hal {
	const char *name;

	/* Fill out[] with block devices of DEVTYPE "disk"; returns count. */
	int (*enumerate_devices)(rfd_device *out, int max_count);

	/* Open exclusively for read/write; returns fd or -errno. */
	int (*open_exclusive)(const char *node);
	void (*close)(int fd);

	/* Logical sector size and total device size in bytes. */
	int (*get_geometry)(int fd, uint32_t *sector_size, uint64_t *total_bytes);

	/* Positional I/O; len should be a multiple of the logical sector size. */
	ssize_t (*write_at)(int fd, const void *buf, size_t len, uint64_t offset);
	ssize_t (*read_at)(int fd, void *buf, size_t len, uint64_t offset);

	/* Flush volatile caches all the way to the medium. */
	int (*flush)(int fd);

	/* Flush only the data we wrote (fdatasync); cheap enough for
	 * periodic use while streaming a long write. */
	int (*sync_data)(int fd);

	/* Make the kernel re-read the partition table written by us. */
	int (*reread_partitions)(const char *node);

	/* dev_t of every partition belonging to the given disk (safety checks). */
	int (*list_partitions)(const char *node, dev_t *out, int max_count);
} rfd_hal;

const rfd_hal *rufus_hal(void);

/* Human readable size into buf ("12.3 GiB"). */
void rfd_format_size(uint64_t bytes, char *buf, size_t buf_len);

#endif
