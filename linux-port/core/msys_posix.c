/*
 * Rufus Linux port - POSIX backend for the vendored ms-sys library.
 *
 * Upstream ms-sys carries its Win32 backend in src/ms-sys/file.c, which
 * pulls in src/rufus.h and the whole Win32 world. This file implements
 * exactly the contract of src/ms-sys/inc/file.h (read_sectors,
 * write_sectors, read_data, write_data, contains_data) against POSIX
 * fds, so the rest of ms-sys compiles unmodified. The FAKE_FD._handle
 * field carries the fd as an integer casted through intptr_t; _offset
 * keeps the upstream meaning of a base offset added to every position.
 * GPLv3
 */
#include "log.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "file.h"

extern unsigned long ulBytesPerSector;   /* defined by ms-sys/br.c */

/* Returns the number of bytes written or -1 on error */
int64_t write_sectors(void *hDrive, uint64_t SectorSize,
                      uint64_t StartSector, uint64_t nSectors,
                      const void *pBuf)
{
	int fd = (int)(intptr_t)hDrive;
	uint64_t len = nSectors * SectorSize;
	uint64_t off = StartSector * SectorSize;
	const uint8_t *p = pBuf;

	if (len > 0x7FFFFFFFUL) {
		rlog_err("write_sectors: nSectors x SectorSize is too big");
		return -1;
	}

	while (len > 0) {
		ssize_t r = pwrite(fd, p, len, (off_t)off);

		if (r <= 0) {
			if ((r < 0) && (errno == EINTR))
				continue;
			rlog_err("write_sectors: write error at sector 0x%08" PRIx64 ": %s",
				StartSector, strerror(errno));
			return -1;
		}
		p += r;
		off += (uint64_t)r;
		len -= (uint64_t)r;
	}
	return (int64_t)(nSectors * SectorSize);
}

/* Returns the number of bytes read or -1 on error */
int64_t read_sectors(void *hDrive, uint64_t SectorSize,
                     uint64_t StartSector, uint64_t nSectors,
                     void *pBuf)
{
	int fd = (int)(intptr_t)hDrive;
	uint64_t want = nSectors * SectorSize;
	uint64_t len = want;
	uint64_t off = StartSector * SectorSize;
	uint8_t *p = pBuf;

	if (len > 0x7FFFFFFFUL) {
		rlog_err("read_sectors: nSectors x SectorSize is too big");
		return -1;
	}

	while (len > 0) {
		ssize_t r = pread(fd, p, len, (off_t)off);

		if (r <= 0) {
			if ((r < 0) && (errno == EINTR))
				continue;
			/* A short read must not pass as success: callers
			   would copy the uninitialized buffer tail back to
			   the device in read-modify-write paths. */
			rlog_err("read_sectors: read error at sector 0x%08" PRIx64 ": %s",
				StartSector, (r == 0) ? "end of device" : strerror(errno));
			return -1;
		}
		p += r;
		off += (uint64_t)r;
		len -= (uint64_t)r;
	}
	return (int64_t)(want - len);
}

int contains_data(FILE *fp, uint64_t Position,
	const void *pData, uint64_t Len)
{
	int r = 0;
	unsigned char *aucBuf = malloc(MAX_DATA_LEN);

	if (aucBuf == NULL)
		return 0;

	if (!read_data(fp, Position, aucBuf, Len))
		goto out;

	if (memcmp(pData, aucBuf, (size_t)Len))
		goto out;

	r = 1;

out:
	free(aucBuf);
	return r;
} /* contains_data */

int read_data(FILE *fp, uint64_t Position,
              void *pData, uint64_t Len)
{
	int r = 0;
	unsigned char *aucBuf = malloc(MAX_DATA_LEN);
	FAKE_FD *fd = (FAKE_FD *)fp;
	void *hDrive = fd->_handle;
	uint64_t StartSector, EndSector, NumSectors;

	if (aucBuf == NULL)
		return 0;

	Position += fd->_offset;

	StartSector = Position / ulBytesPerSector;
	EndSector   = (Position + Len + ulBytesPerSector - 1) / ulBytesPerSector;
	NumSectors  = EndSector - StartSector;

	if ((NumSectors * ulBytesPerSector) > MAX_DATA_LEN) {
		rlog_err("read_data: Please increase MAX_DATA_LEN in file.h");
		goto out;
	}

	if (Len > 0xFFFFFFFFUL) {
		rlog_err("read_data: Len is too big");
		goto out;
	}

	if (read_sectors(hDrive, ulBytesPerSector, StartSector,
	                 NumSectors, aucBuf) <= 0)
		goto out;

	memcpy(pData, &aucBuf[Position - StartSector * ulBytesPerSector],
		(size_t)Len);

	r = 1;

out:
	free(aucBuf);
	return r;
}  /* read_data */

/* May read/write the same sector many times, but compatible with existing ms-sys */
int write_data(FILE *fp, uint64_t Position,
               const void *pData, uint64_t Len)
{
	int r = 0;
	unsigned char *aucBuf = malloc(MAX_DATA_LEN);
	FAKE_FD *fd = (FAKE_FD *)fp;
	void *hDrive = fd->_handle;
	uint64_t StartSector, EndSector, NumSectors;

	if (aucBuf == NULL)
		return 0;

	Position += fd->_offset;

	StartSector = Position / ulBytesPerSector;
	EndSector   = (Position + Len + ulBytesPerSector - 1) / ulBytesPerSector;
	NumSectors  = EndSector - StartSector;

	if ((NumSectors * ulBytesPerSector) > MAX_DATA_LEN) {
		rlog_err("write_data: Please increase MAX_DATA_LEN in file.h");
		goto out;
	}

	if (Len > 0xFFFFFFFFUL) {
		rlog_err("write_data: Len is too big");
		goto out;
	}

	/* Data may not be aligned on a sector boundary => read-modify-write */
	if (read_sectors(hDrive, ulBytesPerSector, StartSector,
	                 NumSectors, aucBuf) <= 0)
		goto out;

	memcpy(&aucBuf[Position - StartSector * ulBytesPerSector], pData,
		(size_t)Len);

	if (write_sectors(hDrive, ulBytesPerSector, StartSector,
	                  NumSectors, aucBuf) <= 0)
		goto out;

	r = 1;

out:
	free(aucBuf);
	return r;
} /* write_data */
