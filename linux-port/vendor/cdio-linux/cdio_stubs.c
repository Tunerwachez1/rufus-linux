/*
 * Rufus Linux port - stubs mirroring what Rufus itself provides for the
 * vendored libcdio (see src/iso.c:73-74): physical/CD-ROM access is
 * disabled, so udf_open() falls back to its FILE*-based stdio stream.
 * GPLv3
 */
#include <cdio/cdio.h>
#include <cdio/device.h>

CdIo_t *cdio_open (const char *psz_source, driver_id_t driver_id)
{
	(void)psz_source;
	(void)driver_id;
	return NULL;
}

void cdio_destroy (CdIo_t *p_cdio)
{
	(void)p_cdio;
}
