/*
 * Rufus Linux port - filesystem helpers for the write pipeline.
 * GPLv3
 */
#include "filesys.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int rfd_find_tool(const char *const *names, char *buf, size_t buf_len)
{
	const char *path = getenv("PATH");
	char dirbuf[1024];
	size_t i;

	if (path == NULL)
		return -ENOENT;

	while (*path != '\0') {
		const char *sep = strchr(path, ':');
		size_t dlen = sep ? (size_t)(sep - path) : strlen(path);

		if ((dlen == 0) || (dlen >= sizeof(dirbuf))) {
			/* An empty PATH element means the current directory:
			   executing a found binary from cwd as root is a
			   hijack vector, so skip it (and oversized entries).
			   Advance unconditionally to avoid an endless loop. */
			if (sep == NULL)
				break;
			path = sep + 1;
			continue;
		}
		memcpy(dirbuf, path, dlen);
		dirbuf[dlen] = '\0';

		for (i = 0; names[i] != NULL; i++) {
			struct stat st;

			snprintf(buf, buf_len, "%s/%s", dirbuf, names[i]);
			if ((stat(buf, &st) == 0) && S_ISREG(st.st_mode) &&
			    (access(buf, X_OK) == 0))
				return 0;
		}

		path += dlen;
		if (*path == ':')
			path++;
	}
	return -ENOENT;
}

int rfd_format_fat32(const char *part_node, uint32_t sector_size)
{
	static const char *const tools[] =
		{ "mkfs.vfat", "mkfs.fat", "mkdosfs", NULL };
	char tool[512], sszopt[64];
	pid_t pid;
	int status = 0;

	if (rfd_find_tool(tools, tool, sizeof(tool)) != 0) {
		rlog_err("mkfs.vfat not found - install dosfstools "
			"(declared soft-dependency)");
		return -ENOTSUP;
	}

	snprintf(sszopt, sizeof(sszopt), "-S%u", sector_size);

	pid = fork();
	if (pid < 0)
		return -errno;
	if (pid == 0) {
		execl(tool, tool, "-F32", sszopt, part_node, (char *)NULL);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return -errno;
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		rlog_err("%s failed on %s (exit %d)", tool, part_node,
			WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		return -EIO;
	}
	rlog_dbg("formatted %s as FAT32 via %s", part_node, tool);
	return 0;
}

int rfd_partition_node(const char *disk_node, int index,
	char *out, size_t out_len, bool check_exist)
{
	struct stat st;
	size_t len = strlen(disk_node);
	bool digit_last = (len > 0) && (disk_node[len - 1] >= '0') &&
		(disk_node[len - 1] <= '9');
	int n;

	/* digit-ending disks need a "p" separator: loop0 -> loop0p1 */
	if (digit_last)
		n = snprintf(out, out_len, "%sp%d", disk_node, index);
	else
		n = snprintf(out, out_len, "%s%d", disk_node, index);
	if ((n < 0) || ((size_t)n >= out_len))
		return -EINVAL;
	if (check_exist && (stat(out, &st) != 0))
		return -errno;
	return 0;
}

int rfd_mount(const char *part_node, const char *dir)
{
	if (mount(part_node, dir, "vfat", 0, NULL) != 0)
		return -errno;
	return 0;
}

int rfd_umount(const char *dir)
{
	if (umount(dir) != 0)
		return -errno;
	return 0;
}
