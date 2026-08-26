/*
 * Rufus Linux port - unit test for image probing and job planning.
 * GPLv3
 */
#include "../core/job.h"
#include "../core/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures = 0;

#define CHECK(cond, name) do { \
	if (cond) { printf("PASS: %s\n", name); } \
	else { printf("FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__); \
		g_failures++; } \
} while (0)

int main(void)
{
	char path[] = "/tmp/rufus-job-test-XXXXXX";
	char isopath[] = "/tmp/rufus-job-iso-XXXXXX";
	rfd_image_info info;
	rfd_job job;
	int fd;

	rlog_set_level(RLOG_ERROR);

	/* ---- raw disk image ------------------------------------------ */
	fd = mkstemp(path);
	if (fd < 0)
		return 2;
	if (ftruncate(fd, (off_t)4 * 2048 * 1024) != 0)
		return 2;
	close(fd);

	CHECK(rfd_probe_image(path, &info) == 0, "probe raw file");
	CHECK(info.type == RFD_IMG_RAW, "raw file detected as raw");
	CHECK(!info.has_boot_signature, "zeroed file has no boot signature");

	CHECK(rfd_job_init(&job, path, "/dev/null", false) == 0,
		"plan raw DD job");
	CHECK((job.nsteps == 1) && (job.steps[0].type == RFD_STEP_WRITE_DD),
		"raw plan is a single DD step");
	CHECK(job.supported, "DD plan is fully supported");

	unlink(path);

	/* ---- synthetic ISO9660 with PVD at LBA 16 --------------------- */
	fd = mkstemp(isopath);
	if (fd < 0)
		return 2;
	if (ftruncate(fd, (off_t)20 * 2048) != 0)
		return 2;
	{
		uint8_t pvd[64];

		memset(pvd, ' ', sizeof(pvd));
		pvd[0] = 0x01;
		memcpy(&pvd[1], "CD001", 5);
		pvd[6] = 0x01;
		memcpy(&pvd[40], "CACHYOS_TEST", 12);
		if (pwrite(fd, pvd, sizeof(pvd),
			(off_t)16 * 2048) != (ssize_t)sizeof(pvd))
			return 2;
	}
	close(fd);

	CHECK(rfd_probe_image(isopath, &info) == 0, "probe ISO");
	CHECK(info.type == RFD_IMG_ISO9660, "ISO9660 detected via PVD");
	CHECK(strcmp(info.label, "CACHYOS_TEST") == 0,
		"volume label trimmed from PVD");

	CHECK(rfd_job_init(&job, isopath, "/dev/null", false) == 0,
		"plan ISO default (hybrid DD) job");
	CHECK(job.supported &&
	      (job.nsteps == 1) && (job.steps[0].type == RFD_STEP_WRITE_DD),
		"ISO defaults to supported DD plan");

	CHECK(rfd_job_init(&job, isopath, "/dev/null", true) == 0,
		"plan ISO file-copy job");
	CHECK(job.nsteps == 4, "copy pipeline has four steps");
	CHECK(job.supported, "copy pipeline fully supported");
	{
		char buf[1024];

		rfd_job_describe(&job, buf, sizeof(buf));
		CHECK(strstr(buf, "partition") != NULL, "describe lists partitioning");
		CHECK(strstr(buf, "[not implemented yet]") == NULL,
			"describe shows no unimplemented steps");
	}

	/* ---- copy mode rejected for non-ISO --------------------------- */
	{
		char path2[] = "/tmp/rufus-job-test2-XXXXXX";
		int fd2 = mkstemp(path2);

		if (fd2 < 0)
			return 2;
		ftruncate(fd2, 4096);
		close(fd2);
		CHECK(rfd_job_init(&job, path2, "/dev/null", true) == -EINVAL,
			"copy mode refuses non-ISO images");
		unlink(path2);
	}

	unlink(isopath);

	printf("%s\n", g_failures ? "JOB TEST: FAILED" : "JOB TEST: ALL PASSED");
	return g_failures ? 1 : 0;
}
