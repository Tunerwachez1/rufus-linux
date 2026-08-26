/*
 * Rufus Linux port - filesystem helpers for the write pipeline.
 * GPLv3
 */
#ifndef RUFUS_FILESYS_H
#define RUFUS_FILESYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Find an executable among names[] via PATH; fills buf, returns 0 if found */
int rfd_find_tool(const char *const *names, char *buf, size_t buf_len);

/*
 * Format the partition backing part_node as FAT32 by invoking mkfs.vfat /
 * mkfs.fat (dosfstools, declared soft-dependency). Returns 0 on success,
 * -errno-style errors; -ENOTSUP when no tool found.
 */
int rfd_format_fat32(const char *part_node, uint32_t sector_size);

/*
 * Build the partition device node name for partition number index (1-based)
 * of disk_node ("/dev/sdc" -> "/dev/sdc1", "loop0" -> "loop0p1",
 * "nvme0n1" -> "nvme0n1p1"). Returns 0 and fills out[]; verifies existence
 * unless check_exist is false.
 */
int rfd_partition_node(const char *disk_node, int index,
	char *out, size_t out_len, bool check_exist);

/* Mount/umount wrappers (require appropriate privileges). */
int rfd_mount(const char *part_node, const char *dir);
int rfd_umount(const char *dir);

#endif
