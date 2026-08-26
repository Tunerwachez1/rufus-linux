/*
 * Rufus Linux port - stage 1 job engine: plan a write pipeline.
 * GPLv3
 */
#include "job.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static const struct {
	rfd_step_type type;
	const char *name;
	bool implemented;      /* an executor exists in this build */
} step_table[] = {
	{ RFD_STEP_WRITE_DD,           "write image byte-for-byte (dd)",   true  },
	{ RFD_STEP_PARTITION_MBR,      "partition target (MBR)",           true  },
	{ RFD_STEP_FORMAT_FAT32,       "create FAT32 filesystem",          true  },
	{ RFD_STEP_COPY_ISO_FILES,
			"copy files from ISO (>4 GiB WIM auto-split)", true  },
	{ RFD_STEP_INSTALL_BOOTCODE_WIN,
			"install Windows boot code (MBR+PBR)",              true  },
};

const char *rfd_step_name(rfd_step_type t)
{
	size_t i;

	for (i = 0; i < sizeof(step_table) / sizeof(step_table[0]); i++)
		if (step_table[i].type == t)
			return step_table[i].name;
	return "?";
}

static bool step_implemented(rfd_step_type t)
{
	size_t i;

	for (i = 0; i < sizeof(step_table) / sizeof(step_table[0]); i++)
		if (step_table[i].type == t)
			return step_table[i].implemented;
	return false;
}

int rfd_job_init(rfd_job *job, const char *image_path, const char *target,
	bool copy_mode)
{
	int err;

	memset(job, 0, sizeof(*job));
	strncpy(job->image_path, image_path, sizeof(job->image_path) - 1);
	strncpy(job->target, target, sizeof(job->target) - 1);
	job->copy_mode = copy_mode;

	err = rfd_probe_image(image_path, &job->info);
	if (err != 0)
		return err;

	job->nsteps = 0;
	job->supported = true;

	if (!copy_mode) {
		/* DD pipeline: works for raw images and hybrid ISOs alike */
		job->steps[job->nsteps++].type = RFD_STEP_WRITE_DD;
	} else if (job->info.type == RFD_IMG_ISO9660) {
		/* Windows-style file-copy pipeline */
		job->steps[job->nsteps++].type = RFD_STEP_PARTITION_MBR;
		job->steps[job->nsteps++].type = RFD_STEP_FORMAT_FAT32;
		job->steps[job->nsteps++].type = RFD_STEP_COPY_ISO_FILES;
		job->steps[job->nsteps++].type = RFD_STEP_INSTALL_BOOTCODE_WIN;
	} else {
		rlog_err("'%s' is not an ISO9660 image - file-copy mode "
			"does not apply", image_path);
		return -EINVAL;
	}

	for (int i = 0; i < job->nsteps; i++)
		if (!step_implemented(job->steps[i].type))
			job->supported = false;

	return 0;
}

void rfd_job_describe(const rfd_job *job, char *buf, size_t buf_len)
{
	size_t off = 0;

	buf[0] = '\0';
	for (int i = 0; i < job->nsteps; i++) {
		int n = snprintf(buf + off, buf_len - off, "%2d. %s%s\n",
			i + 1, rfd_step_name(job->steps[i].type),
			step_implemented(job->steps[i].type) ? "" :
			"   [not implemented yet]");

		if ((n < 0) || ((size_t)n >= buf_len - off))
			break;
		off += (size_t)n;
	}
}
