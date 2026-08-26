/*
 * Rufus Linux port - image type probing, stage 1 job engine.
 * GPLv3
 */
#include "image.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ISO_PVD_LBA        16u
#define ISO_STD_SECTOR     2048u

int rfd_probe_image(const char *path, rfd_image_info *info)
{
	uint8_t sector[ISO_STD_SECTOR];
	uint8_t pvd[128];       /* covers the label field ending at byte 72 */
	uint64_t pvd_off = (uint64_t)ISO_PVD_LBA * ISO_STD_SECTOR;
	int fd, r;

	memset(info, 0, sizeof(*info));

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	{
		struct stat st;

		if (fstat(fd, &st) != 0) {
			close(fd);
			return -errno;
		}
		if (!S_ISREG(st.st_mode)) {
			close(fd);
			return -EINVAL;
		}
		info->size_bytes = (uint64_t)st.st_size;
	}

	r = (int)pread(fd, sector, sizeof(sector), 0);
	if (r != (int)sizeof(sector)) {
		close(fd);
		return (r < 0) ? -errno : -EIO;
	}

	if ((sector[510] == 0x55) && (sector[511] == 0xAA))
		info->has_boot_signature = true;
	if (sector[446 + 4] != 0x00)
		info->has_partition_table = true;

	/* Primary Volume Descriptor: type byte 0x01, id "CD001" at LBA 16 */
	if (info->size_bytes > pvd_off + sizeof(pvd)) {
		r = (int)pread(fd, pvd, sizeof(pvd), (off_t)pvd_off);
		if (r == (int)sizeof(pvd)) {
		if ((pvd[0] == 0x01) &&
		    (memcmp(&pvd[1], "CD001", 5) == 0) &&
		    (pvd[6] == 0x01)) {
				char field[33];
				size_t len;

				info->type = RFD_IMG_ISO9660;
				memcpy(field, &pvd[40], 32);
				field[32] = '\0';
				len = strspn(field, " ");          /* leading pad */
				memmove(field, field + len,
					sizeof(field) - len);
				len = strlen(field);
				while ((len > 0) && (field[len - 1] == ' '))
					field[--len] = '\0';       /* trailing */
				memcpy(info->label, field, len + 1);
			}
		}
	}

	close(fd);
	return 0;
}

const char *rfd_image_type_name(rfd_image_type t)
{
	return (t == RFD_IMG_ISO9660) ? "iso9660" : "raw";
}
