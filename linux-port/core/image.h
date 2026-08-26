/*
 * Rufus Linux port - image type probing, stage 1 job engine.
 *
 * Detects what an image file actually is before deciding the write
 * pipeline: ISO9660 (by its Primary Volume Descriptor at LBA 16) or a
 * raw disk image. Compressed images are unwrapped by bled upstream and
 * are probed after decompression by the executor.
 * GPLv3
 */
#ifndef RUFUS_IMAGE_H
#define RUFUS_IMAGE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	RFD_IMG_RAW = 0,     /* dd-able disk image (may be a hybrid ISO) */
	RFD_IMG_ISO9660      /* plain ISO9660/Joliet filesystem */
} rfd_image_type;

typedef struct {
	rfd_image_type type;
	char label[33];       /* volume label from PVD (ISO) */
	uint64_t size_bytes;
	bool has_boot_signature;   /* sector 0 ends with 55AA */
	bool has_partition_table;  /* non-zero entry at MBR offset 446+4 */
} rfd_image_info;

/*
 * Probe a regular file. Returns 0 and fills info; -errno on I/O error.
 * A hybrid Linux ISO reports both ISO9660 and boot signature - callers
 * decide the pipeline by their own policy.
 */
int rfd_probe_image(const char *path, rfd_image_info *info);

const char *rfd_image_type_name(rfd_image_type t);

#endif
