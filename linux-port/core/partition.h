/*
 * Rufus Linux port - disk partitioning (MBR and GPT), stage 1.
 *
 * GPT is written from scratch per the UEFI spec (protective MBR, primary
 * header at LBA1, 128 entries of 128 bytes from LBA2, backup header and
 * entries in the last 33 sectors). MBR writes only the table area
 * (bytes 440..511) so boot code installed by ms-sys stays intact.
 * GPLv3
 */
#ifndef RUFUS_PARTITION_H
#define RUFUS_PARTITION_H

#include <stdbool.h>
#include <stdint.h>

/* MBR partition types we need right now */
#define RFD_MBR_TYPE_NTFS        0x07
#define RFD_MBR_TYPE_FAT16       0x06
#define RFD_MBR_TYPE_FAT32       0x0B
#define RFD_MBR_TYPE_FAT32_LBA   0x0C
#define RFD_MBR_TYPE_LINUX       0x83
#define RFD_MBR_TYPE_GPT         0xEE

typedef struct {
	uint64_t start_lba;
	uint64_t num_lba;
	uint8_t type;          /* RFD_MBR_TYPE_* */
	bool bootable;         /* sets the 0x80 active flag */
} rfd_mbr_part;

/*
 * Write an MBR partition table. Only bytes 440..511 of sector 0 are
 * touched (disk signature, four entries, 55AA); any boot code already
 * present is preserved. If disk_signature is 0, one is generated.
 * Returns 0 on success.
 */
int rfd_write_mbr_table(int fd, uint32_t sector_size,
	const rfd_mbr_part *parts, int count, uint32_t disk_signature);

/* "EFI System Partition" GUID, big-endian mnemonic form */
extern const uint8_t rfd_guid_esp[16];
/* Microsoft basic data partition GUID */
extern const uint8_t rfd_guid_basic_data[16];
/* Linux filesystem GUID */
extern const uint8_t rfd_guid_linux_fs[16];

typedef struct {
	uint64_t start_lba;    /* inclusive */
	uint64_t end_lba;      /* inclusive */
	const uint8_t *type_guid;
	const char *name;      /* ASCII, up to 36 chars */
} rfd_gpt_part;

/*
 * Write a complete GPT: protective MBR (type 0xEE covering the disk),
 * primary header + entries, backup header + entries. The fd must be
 * opened for writing; disk_bytes gives the medium size. Partition GUIDs
 * are generated randomly. Returns 0 on success.
 */
int rfd_write_gpt(int fd, uint32_t sector_size, uint64_t disk_bytes,
	const rfd_gpt_part *parts, int count);

#endif
