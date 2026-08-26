/*
 * Rufus Linux port - stage 1 job engine: plan a write pipeline.
 *
 * A "job" is an image/target pair plus the ordered step list that turns
 * the target into a bootable medium. Planning is separated from
 * execution ON PURPOSE: every step must be supported before the first
 * byte touches the target - a half-executed pipeline is never acceptable.
 *
 * Policy today mirrors Rufus defaults: raw images and hybrid Linux ISOs
 * go through DD (the image brings its own boot code); plain ISOs may be
 * requested as file-copy pipelines (Windows style) - the copy/format
 * executors are stage-1 work in progress and are reported as unsupported
 * instead of being half-run.
 * GPLv3
 */
#ifndef RUFUS_JOB_H
#define RUFUS_JOB_H

#include <stdbool.h>
#include <stddef.h>

#include "image.h"

#define RFD_JOB_MAX_STEPS 8

typedef enum {
	RFD_STEP_WRITE_DD = 0,
	RFD_STEP_PARTITION_MBR,
	RFD_STEP_FORMAT_FAT32,
	RFD_STEP_COPY_ISO_FILES,
	RFD_STEP_INSTALL_BOOTCODE_WIN
} rfd_step_type;

typedef struct {
	rfd_step_type type;
} rfd_step;

typedef struct {
	char image_path[4096];
	char target[256];
	bool copy_mode;              /* request file-copy pipeline (ISO) */
	rfd_image_info info;
	rfd_step steps[RFD_JOB_MAX_STEPS];
	int nsteps;
	bool supported;              /* every step has an executor yet */
} rfd_job;

/* Probe the image and fill in the step plan. Returns 0 on success. */
int rfd_job_init(rfd_job *job, const char *image_path, const char *target,
	bool copy_mode);

/* Human-readable step list, one per line, into buf. */
void rfd_job_describe(const rfd_job *job, char *buf, size_t buf_len);

const char *rfd_step_name(rfd_step_type t);

#endif
