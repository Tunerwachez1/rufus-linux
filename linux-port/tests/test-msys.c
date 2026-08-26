/*
 * Rufus Linux port - unit test for the ms-sys POSIX backend.
 *
 * Exercises the vendored ms-sys code (unmodified) against a plain file
 * through the FAKE_FD/fd shim: MBR writers + identifiers + the
 * read_data/write_data/contains_data layer with unaligned access.
 * GPLv3
 */
#include "../core/log.h"
#include "../core/msys_wrap.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "br.h"

static int g_failures = 0;

#define CHECK(cond, name) do { \
	if (cond) { \
		printf("PASS: %s\n", name); \
	} else { \
		printf("FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__); \
		g_failures++; \
	} \
} while (0)

static int read_raw(int fd, uint64_t off, void *buf, size_t len)
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

int main(void)
{
	char path[] = "/tmp/rufus-msys-test-XXXXXX";
	uint8_t sector[512], expect[512];
	FAKE_FD fake;
	int fd;
	FILE *fp;

	rlog_set_level(RLOG_ERROR);

	fd = mkstemp(path);
	if (fd < 0) {
		perror("mkstemp");
		return 2;
	}
	if (ftruncate(fd, 8192 * 512) != 0) {
		perror("ftruncate");
		return 2;
	}

	set_bytes_per_sector(512);
	fp = msys_fp(&fake, fd, 0);

	/* --- zero MBR ------------------------------------------------ */
	CHECK(write_zero_mbr(fp) == 1, "write_zero_mbr");
	memset(sector, 0xAA, sizeof(sector));
	CHECK(read_data(fp, 0, sector, sizeof(sector)) == 1,
		"read_data full sector");
	CHECK(is_zero_mbr(fp) == 1, "is_zero_mbr");
	CHECK((sector[510] == 0x55) && (sector[511] == 0xAA),
		"zero MBR has 55AA signature");

	/* --- Windows 7 MBR ------------------------------------------- */
	CHECK(write_win7_mbr(fp) == 1, "write_win7_mbr");
	CHECK(read_data(fp, 0, sector, sizeof(sector)) == 1,
		"read_data after win7 mbr");
	CHECK(is_win7_mbr(fp) == 1, "is_win7_mbr recognizes its own boot code");
	CHECK(memcmp(sector, (const uint8_t *)"\x33\xc0\x8e\xd0\xbc\x00", 6) == 0,
		"win7 MBR starts with its documented boot prologue");
	CHECK((sector[510] == 0x55) && (sector[511] == 0xAA),
		"win7 MBR has 55AA signature");

	/* partition table area must be inside the first sector */
	CHECK(read_raw(fd, 446, expect, 64), "read raw partition entries");
	CHECK(memcmp(&sector[446], expect, 64) != 0 || expect[4] == 0,
		"win7 MBR carries partition table bytes");

	/* --- disk signature roundtrip -------------------------------- */
	CHECK(write_windows_disk_signature(fp, 0x11223344u) == 1,
		"write_windows_disk_signature");
	CHECK(read_windows_disk_signature(fp) == 0x11223344u,
		"read_windows_disk_signature roundtrip");
	CHECK(is_win7_mbr(fp) == 1,
		"is_win7_mbr ignores the disk signature");

	/* --- base offset handling (FAKE_FD._offset) ------------------- */
	{
		uint8_t before[512];

		CHECK(read_raw(fd, 0, before, sizeof(before)), "snapshot sector 0");
		fake._offset = 1024;   /* second sector becomes "position 0" */
		CHECK(write_win7_mbr(fp) == 1, "write_win7_mbr at _offset=1024");
		memset(sector, 0, sizeof(sector));
		CHECK(read_data(fp, 0, sector, 6) == 1, "read_data at base");
		CHECK(memcmp(sector,
			(const uint8_t *)"\x33\xc0\x8e\xd0\xbc\x00", 6) == 0,
			"win7 MBR landed at offset 1024");
		CHECK(read_raw(fd, 0, expect, sizeof(expect)) &&
		      (memcmp(before, expect, sizeof(before)) == 0),
			"first sector untouched by offset write");
		fake._offset = 0;
	}

	/* --- unaligned read-modify-write ------------------------------- */
	{
		const uint8_t pattern[7] = { 1, 2, 3, 4, 5, 6, 7 };
		uint8_t big[4096];
		uint64_t region = 1024 * 1024;   /* far from any boot code */

		memset(big, 0x5A, sizeof(big));
		CHECK(write_data(fp, region, big, sizeof(big)) == 1,
			"prefill test region with known filler");

		CHECK(write_data(fp, region + 300, pattern, sizeof(pattern)) == 1,
			"write_data unaligned inside filler");
		CHECK(contains_data(fp, region + 300, pattern, sizeof(pattern)) == 1,
			"contains_data sees the pattern");
		{
			const uint8_t other[7] = { 9, 9, 9, 9, 9, 9, 9 };

			CHECK(contains_data(fp, region + 300, other,
				sizeof(other)) == 0,
				"contains_data rejects wrong pattern");
		}
		memset(sector, 0, sizeof(sector));
		CHECK(read_data(fp, region + 296, sector, 15) == 1,
			"read across the written pattern");
		CHECK((sector[0] == 0x5A) && (sector[4] == 1) &&
		      (sector[10] == 7) && (sector[11] == 0x5A),
			"bytes outside the write window preserved");
	}

	/* --- GRUB2 / syslinux MBR variants compile in and land -------- */
	CHECK(write_grub2_mbr(fp) == 1, "write_grub2_mbr");
	CHECK(is_grub2_mbr(fp) == 1, "is_grub2_mbr recognizes it");
	CHECK(write_syslinux_gpt_mbr(fp) == 1, "write_syslinux_gpt_mbr");
	CHECK(is_syslinux_gpt_mbr(fp) == 1, "is_syslinux_gpt_mbr");

	close(fd);
	unlink(path);

	printf("%s\n", g_failures ? "MSYS TEST: FAILED" : "MSYS TEST: ALL PASSED");
	return g_failures ? 1 : 0;
}
