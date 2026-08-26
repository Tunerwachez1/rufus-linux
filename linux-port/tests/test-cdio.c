/*
 * Rufus Linux port - integration test for the vendored libcdio.
 *
 * Opens a REAL ISO (path in argv[1]) through both layers exactly the way
 * Rufus does: ISO9660 (+Joliet/Rock Ridge) first, then UDF - which holds
 * the real files on Microsoft images while their ISO9660 layer only
 * carries a README and the El Torito catalog.
 *
 * Usage: test-cdio <image.iso> <output-file> [path-inside-iso]
 * The extracted file lands in <output-file> for byte comparison against
 * an independent extractor (bsdtar).
 * GPLv3
 */
#include <cdio/cdio.h>
#include <cdio/iso9660.h>
#include <cdio/logging.h>
#include <cdio/udf.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ISO_BLOCKSIZE 2048

static int g_failures = 0;

#define CHECK(cond, name) do { \
	if (cond) { printf("PASS: %s\n", name); } \
	else { printf("FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__); \
		g_failures++; } \
} while (0)

int main(int argc, char **argv)
{
	const char *iso_path, *inner_path, *out_path;
	char inner_name[256];
	int extracted = 0;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <iso> <out> [inner-path]\n", argv[0]);
		return 2;
	}
	iso_path = argv[1];
	out_path = argv[2];
	inner_path = (argc > 3) ? argv[3] : "/arch/version";
	{
		const char *base = strrchr(inner_path, '/');

		strncpy(inner_name, base ? base + 1 : inner_path,
			sizeof(inner_name) - 1);
	}

	cdio_log_set_handler(NULL);

	/* ---------------- ISO9660 layer ------------------------------- */
	{
		iso9660_t *iso = iso9660_open_ext(iso_path, ISO_EXTENSION_ALL);
		char *volume_id = NULL;
		CdioList_t *entlist;
		CdioListNode_t *entnode;
		int real_files = 0;

		CHECK(iso != NULL, "iso9660_open_ext");
		if (iso == NULL)
			return 1;

		CHECK(iso9660_ifs_get_volume_id(iso, &volume_id) && volume_id,
			"read volume id");
		printf("   volume id: '%s'\n", volume_id ? volume_id : "(null)");
		free(volume_id);

		entlist = iso9660_ifs_readdir(iso, "/");
		CHECK(entlist != NULL, "readdir root");
		for (entnode = _cdio_list_begin(entlist); entnode != NULL;
		     entnode = _cdio_list_node_next(entnode)) {
			iso9660_stat_t *e = _cdio_list_node_data(entnode);

			printf("   iso entry: '%s'\n", e->filename);
			if ((strcmp(e->filename, ".") != 0) &&
			    (strcmp(e->filename, "..") != 0))
				real_files++;
		}
		_cdio_list_free(entlist, true, free);

		if (!real_files)
			printf("   -> ISO9660 layer carries no real files "
				"(Microsoft bridge image)\n");

		/* try extracting inner_path from this layer */
		{
			iso9660_stat_t *st =
				iso9660_ifs_stat_translate(iso, inner_path);

			if (st != NULL) {
				long nblocks = CDIO_EXTENT_BLOCKS(st->total_size);
				uint8_t *buf = calloc((size_t)nblocks,
					ISO_BLOCKSIZE);
				FILE *out;

				if (buf == NULL) {
					fprintf(stderr,
						"out of memory for %ld blocks\n",
						nblocks);
					iso9660_stat_free(st);
					return 2;
				}
				CHECK(iso9660_iso_seek_read(iso, buf, st->lsn,
					nblocks) ==
					nblocks * ISO_BLOCKSIZE,
					"iso9660 seek_read");
				out = fopen(out_path, "wb");
				if (out == NULL) {
					perror("fopen out");
					return 2;
				}
				fwrite(buf, 1, (size_t)st->total_size, out);
				fclose(out);
				extracted = 1;
				printf("   extracted %s (%" PRIu64
					" bytes) via ISO9660\n", inner_path,
					st->total_size);
				free(buf);
				iso9660_stat_free(st);
			} else {
				printf("   '%s' not in ISO9660 layer\n",
					inner_path);
			}
		}
		iso9660_close(iso);
	}

	/* ---------------- UDF layer ----------------------------------- */
	{
		udf_t *p_udf = udf_open(iso_path);

		if (p_udf == NULL) {
			printf("   no UDF layer (plain Linux hybrid)\n");
			CHECK(extracted, "extraction happened via ISO9660");
		} else {
			udf_dirent_t *root = udf_get_root(p_udf, true, 0);
			udf_dirent_t *it, *f;

			CHECK(root != NULL, "udf_get_root");

			/* open the file BEFORE traversing: udf_fopen expects
			   a pristine root, same call order as iso.c */
			f = udf_fopen(root, inner_name);
			if (f != NULL) {
				uint64_t len = udf_get_file_length(f);
				long nblocks = (long)((len +
					ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE);
				uint8_t *buf = malloc((size_t)nblocks *
					ISO_BLOCKSIZE);
				FILE *out;
				long long got;

				CHECK(buf != NULL, "alloc buffer");
				got = udf_read_block(f, buf, nblocks);
				/* returns bytes read; must cover the file */
				CHECK(got >= (long long)len,
					"udf_read_block full content");
				out = fopen(out_path, "wb");
				if (out == NULL) {
					perror("fopen out");
					return 2;
				}
				fwrite(buf, 1, (size_t)len, out);
				fclose(out);
				extracted = 1;
				printf("   extracted %s (%" PRIu64
					" bytes) via UDF\n", inner_path, len);
				free(buf);
			}

			/* diagnostic listing afterwards */
			it = root;
			while ((it = udf_readdir(it)) != NULL) {
				printf("   udf entry: '%s'%s\n",
					udf_get_filename(it),
					udf_is_dir(it) ? "/" : "");
			}
			CHECK(extracted, "extraction happened via UDF");
			udf_close(p_udf);
		}
	}

	printf("%s\n", g_failures ? "CDIO TEST: FAILED" :
		"CDIO TEST: ALL PASSED");
	return g_failures ? 1 : 0;
}
