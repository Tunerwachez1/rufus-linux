/*
 * Rufus Linux port - disk partitioning (MBR and GPT), stage 1.
 *
 * GPT is written from scratch per the UEFI spec: protective MBR at LBA0,
 * primary header at LBA1, 128 entries of 128 bytes starting at LBA2,
 * backup entries and backup header in the last sectors of the medium.
 * MBR writes only the table area (bytes 440..511 of sector 0) so boot
 * code installed by ms-sys stays intact.
 * GPLv3
 */
#include "partition.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "msys_wrap.h"

/* ------------------------------------------------------------------ */
/* byte-order and misc helpers                                         */

static int pwr(int fd, const void *buf, size_t len, uint64_t off)
{
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t r = pwrite(fd, p, len, (off_t)off);

		if (r <= 0) {
			if ((r < 0) && (errno == EINTR))
				continue;
			rlog_err("partition write failed at %" PRIu64 ": %s",
				off, strerror(errno));
			return -1;
		}
		p += r;
		off += (uint64_t)r;
		len -= (size_t)r;
	}
	return 0;
}

static void put_le32(uint8_t *p, uint32_t v)
{
	int i;

	for (i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

static void put_le64(uint8_t *p, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

/* CRC32 (polynomial 0xEDB88320), as required by the UEFI spec */
static uint32_t crc32_calc(const uint8_t *buf, size_t len)
{
	static uint32_t table[256];
	static bool have_table = false;
	uint32_t crc = 0xFFFFFFFFu;
	size_t i;

	if (!have_table) {
		for (i = 0; i < 256; i++) {
			uint32_t c = (uint32_t)i;
			int bit;

			for (bit = 0; bit < 8; bit++)
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		have_table = true;
	}

	for (i = 0; i < len; i++)
		crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static int read_urandom(void *buf, size_t len)
{
	static int urfd = -2;
	ssize_t r;

	if (urfd == -2)
		urfd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (urfd < 0)
		return -1;
	r = read(urfd, buf, len);
	if ((r < 0) || ((size_t)r != len))
		return -1;
	return 0;
}

/* one round of the splitmix64 finalizer - spreads weak inputs over 64 bits */
static uint64_t mix64(uint64_t x)
{
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ULL;
	x ^= x >> 27;
	x *= 0x94D049BB133111EBULL;
	x ^= x >> 31;
	return x;
}

/*
 * Generate an RFC 4122 v4 GUID directly in its on-disk (mixed-endian)
 * layout: only the version/variant bits need fixing up afterwards.
 */
static void generate_guid(uint8_t out[16])
{
	uint8_t rnd[16];
	unsigned i;

	if (read_urandom(rnd, sizeof(rnd)) != 0) {
		/* extremely unlikely; fall back to time+pid+address entropy
		   run through a mixing function (still not cryptographic,
		   but no longer a trivially predictable LCG) */
		struct timespec ts;
		uint64_t seed = (uint64_t)time(NULL) * 0x9E3779B97F4A7C15ULL;

		clock_gettime(CLOCK_REALTIME, &ts);
		seed ^= mix64((uint64_t)ts.tv_nsec) ^
			mix64(((uint64_t)getpid() << 32) ^
			      (uintptr_t)(void *)out);
		for (i = 0; i < sizeof(rnd); i++) {
			seed = mix64(seed + i);
			rnd[i] = (uint8_t)(seed >> 24);
		}
	}
	memcpy(out, rnd, 16);
	out[6] = (uint8_t)((out[6] & 0x0F) | 0x40);   /* version 4 */
	out[8] = (uint8_t)((out[8] & 0x3F) | 0x80);   /* variant 10xx */
}

/* ------------------------------------------------------------------ */
/* GUIDs used by our partitioning (on-disk mixed-endian layout)        */

const uint8_t rfd_guid_esp[16] = {
	0x28, 0x32, 0x73, 0xc1, 0x68, 0x11, 0xd2, 0x14,
	0xbc, 0x16, 0x62, 0x02, 0x00, 0x00, 0x00, 0x00
};

const uint8_t rfd_guid_basic_data[16] = {
	0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
	0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7
};

const uint8_t rfd_guid_linux_fs[16] = {
	0x30, 0x48, 0xf8, 0x63, 0x06, 0x16, 0x4a, 0x72,
	0xb8, 0x29, 0xa2, 0xa8, 0x23, 0x3f, 0x3c, 0x65
};

/* ------------------------------------------------------------------ */
/* MBR                                                                 */

/* Clamp-and-fill CHS fields the way modern tools do for big disks */
static void set_chs(uint8_t *p, uint64_t lba)
{
	const uint32_t heads = 255, sectors = 63;
	uint32_t cyl, head, sec;

	if (lba > heads * sectors * 1023ULL) {
		head = sec = 0xFE;
		cyl = 0x3FF;
	} else {
		cyl = (uint32_t)(lba / (heads * sectors));
		head = (uint32_t)((lba / sectors) % heads);
		sec = (uint32_t)(lba % sectors) + 1;
	}
	p[0] = (uint8_t)head;
	p[1] = (uint8_t)(((cyl & 0x300) >> 2) | (sec & 0x3F));
	p[2] = (uint8_t)(cyl & 0xFF);
}

int rfd_write_mbr_table(int fd, uint32_t sector_size,
	const rfd_mbr_part *parts, int count, uint32_t disk_signature)
{
	uint8_t sect[512];
	FAKE_FD fake;
	FILE *fp;
	int i;

	if ((count < 0) || (count > 4)) {
		rlog_err("MBR supports at most 4 primary partitions");
		return -1;
	}
	if (sector_size < 512) {
		rlog_err("invalid sector size %u", sector_size);
		return -1;
	}

	fp = msys_fp(&fake, fd, 0);
	if (!read_data(fp, 0, sect, sizeof(sect))) {
		rlog_err("could not read sector 0");
		return -1;
	}

	if (disk_signature == 0) {
		if (read_urandom(&sect[440], 4) != 0)
			memset(&sect[440], 0, 4);
	} else {
		put_le32(&sect[440], disk_signature);
	}

	memset(&sect[446], 0, 64);
	for (i = 0; i < count; i++) {
		uint8_t *e = &sect[446 + 16 * i];

		e[0] = parts[i].bootable ? 0x80 : 0x00;
		set_chs(&e[1], parts[i].start_lba);
		e[4] = parts[i].type;
		set_chs(&e[5],
			parts[i].num_lba ? parts[i].start_lba + parts[i].num_lba - 1 :
					   parts[i].start_lba);
		put_le32(&e[8], (uint32_t)parts[i].start_lba);
		put_le32(&e[12], (uint32_t)parts[i].num_lba);
	}
	sect[510] = 0x55;
	sect[511] = 0xAA;

	if (write_data(fp, 0, sect, sizeof(sect)) != 1) {
		rlog_err("could not write MBR");
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* GPT                                                                 */

#define GPT_N_ENTRIES     128
#define GPT_ENTRY_SIZE    128
#define GPT_MIN_SECTOR    512

static void gpt_fill_header(uint8_t *hdr, uint32_t sector_size,
	uint64_t my_lba, uint64_t alt_lba, uint64_t first_usable,
	uint64_t last_usable, const uint8_t disk_guid[16],
	uint64_t entries_lba)
{
	memset(hdr, 0, sector_size);
	memcpy(&hdr[0], "EFI PART", 8);
	put_le32(&hdr[8], 0x00010000);            /* revision 1.0 */
	put_le32(&hdr[12], 92);                   /* header size */
	/* hdr[16..19] CRC32 - filled after zeroing below */
	put_le64(&hdr[24], my_lba);
	put_le64(&hdr[32], alt_lba);
	put_le64(&hdr[40], first_usable);
	put_le64(&hdr[48], last_usable);
	memcpy(&hdr[56], disk_guid, 16);
	put_le64(&hdr[72], entries_lba);
	put_le32(&hdr[80], GPT_N_ENTRIES);
	put_le32(&hdr[84], GPT_ENTRY_SIZE);

	/* caller appends the entries CRC at [88] then we finalize here:
	   CRC is computed over the first 92 bytes with the CRC field zero */
	put_le32(&hdr[16], 0);
}

static uint32_t gpt_header_crc(uint8_t *hdr)
{
	uint32_t crc;

	crc = crc32_calc(hdr, 92);
	put_le32(&hdr[16], crc);
	return crc;
}

static void encode_utf16_name(uint8_t out[72], const char *name)
{
	size_t i;

	memset(out, 0, 72);
	for (i = 0; (i < 36) && name[i]; i++) {
		out[2 * i] = (uint8_t)name[i];
		out[2 * i + 1] = 0x00;
	}
}

int rfd_write_gpt(int fd, uint32_t sector_size, uint64_t disk_bytes,
	const rfd_gpt_part *parts, int count)
{
	uint8_t *sect = NULL, *entries = NULL;
	uint64_t last_lba, first_usable, last_usable;
	uint64_t entries_lba, backup_entries_lba, backup_header_lba;
	uint64_t protect_num, entries_sectors, min_bytes;
	uint8_t disk_guid[16];
	rfd_mbr_part protective;
	uint32_t ssz = sector_size;
	int rc = -1, i;

	if ((ssz < GPT_MIN_SECTOR) || ((ssz & (ssz - 1)) != 0)) {
		rlog_err("GPT needs a power-of-two sector size >= %u (got %u)",
			GPT_MIN_SECTOR, ssz);
		return -1;
	}
	entries_sectors = (uint64_t)GPT_N_ENTRIES * GPT_ENTRY_SIZE / ssz;
	/* LBA0 MBR + LBA1 header + primary entries + backup entries + backup
	   header: refuse anything smaller instead of letting the layout math
	   underflow below */
	min_bytes = (2 + 2 * entries_sectors) * ssz;
	if (disk_bytes < min_bytes) {
		rlog_err("medium too small for GPT (%" PRIu64 " bytes, "
			"need %" PRIu64 ")", disk_bytes, min_bytes);
		return -1;
	}
	if ((count < 0) || (count > GPT_N_ENTRIES)) {
		rlog_err("GPT supports up to %d partitions", GPT_N_ENTRIES);
		return -1;
	}

	last_lba = disk_bytes / ssz - 1;
	backup_entries_lba = last_lba - entries_sectors;
	backup_header_lba = last_lba;
	first_usable = 2 + entries_sectors;
	last_usable = backup_entries_lba - 1;

	for (i = 0; i < count; i++) {
		if ((parts[i].start_lba < first_usable) ||
		    (parts[i].end_lba > last_usable) ||
		    (parts[i].end_lba < parts[i].start_lba)) {
			rlog_err("partition %d range %" PRIu64 "-%" PRIu64
				" outside usable area %" PRIu64 "-%" PRIu64,
				i + 1, parts[i].start_lba, parts[i].end_lba,
				first_usable, last_usable);
			return -1;
		}
	}

	sect = calloc(1, ssz);
	entries = calloc(GPT_N_ENTRIES * GPT_ENTRY_SIZE, 1);
	if ((sect == NULL) || (entries == NULL))
		goto out;

	/* 1. protective MBR covering the whole medium (clamped to 32 bit) */
	protect_num = last_lba;
	if (protect_num >= 0xFFFFFFFFULL)
		protect_num = 0xFFFFFFFFULL;
	protective.start_lba = 1;
	protective.num_lba = protect_num;
	protective.type = RFD_MBR_TYPE_GPT;
	protective.bootable = false;
	if (rfd_write_mbr_table(fd, ssz, &protective, 1, 0) != 0)
		goto out;

	/* 2. partition entry array */
	for (i = 0; i < count; i++) {
		uint8_t *e = &entries[(size_t)i * GPT_ENTRY_SIZE];

		memcpy(e, parts[i].type_guid, 16);
		generate_guid(&e[16]);
		put_le64(&e[32], parts[i].start_lba);
		put_le64(&e[40], parts[i].end_lba);
		put_le64(&e[48], 0);              /* attributes */
		encode_utf16_name(&e[56], parts[i].name ? parts[i].name : "");
	}

	/* 3. primary header */
	generate_guid(disk_guid);
	gpt_fill_header(sect, ssz, 1, backup_header_lba, first_usable,
		last_usable, disk_guid, 2);
	put_le32(&sect[88], crc32_calc(entries, GPT_N_ENTRIES * GPT_ENTRY_SIZE));
	gpt_header_crc(sect);
	if (pwr(fd, sect, ssz, 1 * ssz) != 0)
		goto out;
	if (pwr(fd, entries, GPT_N_ENTRIES * GPT_ENTRY_SIZE, 2 * ssz) != 0)
		goto out;

	/* 4. backup entries + header */
	if (pwr(fd, entries, GPT_N_ENTRIES * GPT_ENTRY_SIZE,
		backup_entries_lba * ssz) != 0)
		goto out;
	generate_guid(disk_guid);
	gpt_fill_header(sect, ssz, backup_header_lba, 1, first_usable,
		last_usable, disk_guid, backup_entries_lba);
	put_le32(&sect[88], crc32_calc(entries, GPT_N_ENTRIES * GPT_ENTRY_SIZE));
	gpt_header_crc(sect);
	if (pwr(fd, sect, ssz, backup_header_lba * ssz) != 0)
		goto out;

	rc = 0;

out:
	free(sect);
	free(entries);
	return rc;
}
