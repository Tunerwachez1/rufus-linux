/*
 * Rufus Linux port - thin wrapper around the vendored ms-sys library.
 *
 * ms-sys operates on a hijacked FILE* (FAKE_FD) that hides a handle and a
 * base offset. On Windows the handle is a HANDLE; on this port it is the
 * HAL fd. These helpers hide the casting so callers just pass an fd.
 * GPLv3
 */
#ifndef RUFUS_MSYS_WRAP_H
#define RUFUS_MSYS_WRAP_H

#include <stdint.h>
#include <stdio.h>

#include "file.h"

/* Fill caller's FAKE_FD so it can be passed to any ms-sys call as FILE*. */
static inline FILE *msys_fp(FAKE_FD *fake, int fd, uint64_t base_offset)
{
	fake->_handle = (void *)(intptr_t)fd;
	fake->_offset = base_offset;
	return (FILE *)fake;
}

#endif
