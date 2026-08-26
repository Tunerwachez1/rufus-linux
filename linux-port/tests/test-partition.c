/*
 * Rufus Linux port - unit test for MBR/GPT writing.
 *
 * Writes tables into a plain file and validates them with an independent
 * parser implemented here (not sharing code with core/partition.c):
 * byte layouts, CRC32 recomputation, primary/backup header mirroring,
 * protective MBR contents, and MBR boot-code preservation.
 * GPLv3
 */
#include "../core/log.h"
#include "../core/partition.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SSZ        512u
#define IMG_SECT   131072u          /* 64 MiB image */

static int g_failures = 0;

#define CHECK(cond, name) do { \
	if (cond) { \
		printf("PASS: %s\n", name); \
	} else { \
		printf("FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__); \
		g_failures++; \
	} \
} while (0)

static int rd(int fd, uint64_t off, void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len > 0) {
		ssize_t r = pread(fd, p, len, (off_t)off);

		if (r <= 0)
			return 0;
		p += r;
		off += (uint64_t)r;
		len -= (size_t)r;
	}
	return 1;
}

static uint32_t crc_of(const uint8_t *buf, size_t len)
{
	static uint32_t table[256];
	static bool built = false;
	uint32_t crc = 0xFFFFFFFFu;
	size_t i;
	int bit;

	if (!built) {
		for (i = 0; i < 256; i++) {
			uint32_t c = (uint32_t)i;

			for (bit = 0; bit < 8; bit++)
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
		built = true;
	}
	for (i = 0; i < len; i++)
		crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static uint64_t ld64(const uint8_t *p)
{
	uint64_t v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

int main(void)
{
	char path[] = "/tmp/rufus-gpt-test-XXXXXX";
	uint8_t sect[SSZ], entries[16384], backup_entries[16384], boot[446];
	int fd, last_lba;
	rfd_gpt_part parts[2];

	rlog_set_level(RLOG_ERROR);

	fd = mkstemp(path);
	if (fd < 0)
		return 2;
	if (ftruncate(fd, (off_t)IMG_SECT * SSZ) != 0)
		return 2;
	last_lba = IMG_SECT - 1;

	/* ---- GPT with two partitions ------------------------------- */
	memset(boot, 0xAB, sizeof(boot));
	if (pwrite(fd, boot, sizeof(boot), 0) != (ssize_t)sizeof(boot))
		return 2;

	parts[0].start_lba = 34;
	parts[0].end_lba = 20807;             /* ~10 MiB ESP-ish */
	parts[0].type_guid = rfd_guid_esp;
	parts[0].name = "EFI System";
	parts[1].start_lba = 20808;
	parts[1].end_lba = last_lba - 33 - 16;
	parts[1].type_guid = rfd_guid_basic_data;
	parts[1].name = "RUFUS";

	CHECK(rfd_write_gpt(fd, SSZ, (uint64_t)IMG_SECT * SSZ, parts, 2) == 0,
		"rfd_write_gpt");

	/* protective MBR */
	CHECK(rd(fd, 0, sect, SSZ), "read protective MBR");
	CHECK((sect[510] == 0x55) && (sect[511] == 0xAA), "protective MBR sig");
	CHECK(sect[446 + 4] == 0xEE, "protective MBR type 0xEE");
	CHECK((sect[454] == 0x01) && (sect[455] == 0x00) &&
	      (sect[456] == 0x00) && (sect[457] == 0x00),
		"protective entry starts at LBA 1");

	/* primary header */
	CHECK(rd(fd, SSZ, sect, SSZ), "read primary header");
	CHECK(memcmp(sect, "EFI PART", 8) == 0, "primary header signature");
	CHECK((sect[8] | (sect[9] << 8)) == 0x0000 &&
	      sect[10] == 0x01 && sect[11] == 0x00, "revision 1.0");
	{
		uint8_t tmp[92];
		uint32_t stored_crc, calc_crc, ent_crc_stored, ent_crc_calc;

		memcpy(tmp, sect, 92);
		stored_crc = (uint32_t)tmp[16] | ((uint32_t)tmp[17] << 8) |
			((uint32_t)tmp[18] << 16) | ((uint32_t)tmp[19] << 24);
		memset(&tmp[16], 0, 4);
		calc_crc = crc_of(tmp, 92);
		CHECK(stored_crc == calc_crc, "primary header CRC valid");

		CHECK(ld64(&sect[24]) == 1, "MyLBA == 1");
		CHECK(ld64(&sect[32]) == (uint64_t)last_lba, "AlternateLBA == last");
		CHECK(ld64(&sect[40]) == 34, "FirstUsableLBA == 34");
		CHECK(ld64(&sect[48]) == (uint64_t)last_lba - 33, "LastUsableLBA");
		CHECK(ld64(&sect[72]) == 2, "PartitionEntriesLBA == 2");
		CHECK(((uint32_t)sect[80] | ((uint32_t)sect[81] << 8) |
		       ((uint32_t)sect[82] << 16) | ((uint32_t)sect[83] << 24))
			== 128, "128 entries");
		ent_crc_stored = (uint32_t)sect[88] | ((uint32_t)sect[89] << 8) |
			((uint32_t)sect[90] << 16) | ((uint32_t)sect[91] << 24);

		CHECK(rd(fd, 2 * SSZ, entries, sizeof(entries)),
			"read primary entries");
		ent_crc_calc = crc_of(entries, sizeof(entries));
		CHECK(ent_crc_stored == ent_crc_calc, "entries CRC valid");

		/* first entry content */
		CHECK(memcmp(&entries[0], rfd_guid_esp, 16) == 0,
			"entry 0 is EFI System type");
		CHECK(ld64(&entries[32]) == 34 &&
		      ld64(&entries[40]) == parts[0].end_lba,
			"entry 0 LBA range");
		CHECK((entries[56] == 'E') && (entries[57] == 0) &&
		      (entries[58] == 'F') && (entries[59] == 0),
			"entry 0 name is UTF-16LE \"EF...\"");

		/* unique GUID differs between entries */
		CHECK(memcmp(&entries[16], &entries[144 + 16], 16) != 0,
			"unique GUIDs differ per entry");

		/* backup mirror */
		CHECK(rd(fd, (uint64_t)(last_lba - 32) * SSZ, backup_entries,
			sizeof(backup_entries)), "read backup entries");
		CHECK(memcmp(entries, backup_entries, sizeof(entries)) == 0,
			"backup entries mirror primary");

		CHECK(rd(fd, (uint64_t)last_lba * SSZ, sect, SSZ),
			"read backup header");
		CHECK(memcmp(sect, "EFI PART", 8) == 0, "backup header signature");
		CHECK(ld64(&sect[24]) == (uint64_t)last_lba, "backup MyLBA");
		CHECK(ld64(&sect[32]) == 1, "backup AlternateLBA == 1");
		CHECK(ld64(&sect[72]) == (uint64_t)last_lba - 32,
			"backup entries LBA");
		memcpy(tmp, sect, 92);
		stored_crc = (uint32_t)tmp[16] | ((uint32_t)tmp[17] << 8) |
			((uint32_t)tmp[18] << 16) | ((uint32_t)tmp[19] << 24);
		memset(&tmp[16], 0, 4);
		CHECK(stored_crc == crc_of(tmp, 92), "backup header CRC valid");
	}

	/* boot code in sector 0 must have survived (bytes 0..439 untouched;
	   440..443 now hold the random disk signature) */
	CHECK(rd(fd, 0, sect, SSZ), "re-read sector 0 after GPT");
	CHECK(memcmp(sect, boot, 440) == 0,
		"protective MBR preserves pre-existing boot code");

	/* ---- out-of-range partitions are rejected -------------------- */
	{
		rfd_gpt_part bad = parts[0];

		bad.start_lba = 1;   /* inside header area */
		bad.end_lba = 100;
		CHECK(rfd_write_gpt(fd, SSZ, (uint64_t)IMG_SECT * SSZ,
			&bad, 1) != 0, "GPT rejects range below FirstUsable");
	}

	/* ---- MBR table writer ---------------------------------------- */
	{
		char path2[] = "/tmp/rufus-mbr-test-XXXXXX";
		uint8_t pattern[446];
		rfd_mbr_part mp[2];
		int fd2;

		fd2 = mkstemp(path2);
		if (fd2 < 0)
			return 2;
		if (ftruncate(fd2, (off_t)IMG_SECT * SSZ) != 0)
			return 2;

		memset(pattern, 0xC3, sizeof(pattern));
		if (pwrite(fd2, pattern, sizeof(pattern), 0) !=
		    (ssize_t)sizeof(pattern))
			return 2;

		mp[0].start_lba = 2048;
		mp[0].num_lba = 100000;
		mp[0].type = RFD_MBR_TYPE_FAT32_LBA;
		mp[0].bootable = true;
		mp[1].start_lba = 102048;
		mp[1].num_lba = 5000;
		mp[1].type = RFD_MBR_TYPE_LINUX;
		mp[1].bootable = false;

		CHECK(rfd_write_mbr_table(fd2, SSZ, mp, 2, 0xAABBCCDDu) == 0,
			"rfd_write_mbr_table");

		CHECK(rd(fd2, 0, sect, SSZ), "read MBR sector");
		CHECK((sect[510] == 0x55) && (sect[511] == 0xAA), "MBR sig");
		CHECK((sect[440] == 0xDD) && (sect[441] == 0xCC) &&
		      (sect[442] == 0xBB) && (sect[443] == 0xAA),
			"disk signature little-endian at offset 440");
		CHECK(sect[446] == 0x80, "partition 1 bootable flag");
		CHECK(sect[446 + 4] == RFD_MBR_TYPE_FAT32_LBA, "partition 1 type");
		CHECK(sect[446 + 16] == 0x00, "partition 2 not bootable");
		CHECK(sect[446 + 16 + 4] == RFD_MBR_TYPE_LINUX, "partition 2 type");
		CHECK((uint32_t)sect[446 + 8] |
		      ((uint32_t)sect[446 + 9] << 8) |
		      ((uint32_t)sect[446 + 10] << 16) |
		      ((uint32_t)sect[446 + 11] << 24) == 2048,
			"partition 1 start LBA");
		CHECK((uint32_t)sect[446 + 16 + 12] |
		      ((uint32_t)sect[446 + 16 + 13] << 8) |
		      ((uint32_t)sect[446 + 16 + 14] << 16) |
		      ((uint32_t)sect[446 + 16 + 15] << 24) == 5000,
			"partition 2 length");
		CHECK(memcmp(sect, pattern, 440) == 0,
			"MBR writer preserves boot code area");
		close(fd2);
		unlink(path2);
	}

	close(fd);
	unlink(path);

	printf("%s\n", g_failures ? "PARTITION TEST: FAILED" :
		"PARTITION TEST: ALL PASSED");
	return g_failures ? 1 : 0;
}
