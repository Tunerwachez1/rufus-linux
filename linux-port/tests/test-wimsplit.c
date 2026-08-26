/*
 * Rufus Linux port - integration test for vendored wimlib.
 *
 * Exercises exactly what the FAT32 copy pipeline relies on: capture a
 * directory into a .wim, split it into parts small enough for FAT32
 * (here 512 KiB, mirroring RFD_WIM_SPLIT_CHUNK logic), re-open the first
 * part, reference the remaining ones and extract - then verify the
 * extracted bytes match the originals.
 *
 * Note: wimlib never splits a single resource across parts, so the tree
 * is made of many mid-size files to force a multi-part result.
 *
 * GPLv3
 */
#define _GNU_SOURCE            /* asprintf */
#include <wimlib.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define PART_SIZE   (512u << 10)
#define NFILES      12
#define FILE_SIZE   (200u << 10)

static int g_failures;

#define CHECK(cond, name) do { \
	if (cond) { printf("PASS: %s\n", name); } \
	else { printf("FAIL: %s (%s:%d)\n", name, __FILE__, __LINE__); \
		g_failures++; } \
} while (0)

static void die(const char *what)
{
	perror(what);
	exit(2);
}

static int rm_tree_cb(const char *fpath, const struct stat *sb, int typeflag,
	struct FTW *ftwbuf)
{
	(void)sb;
	(void)typeflag;
	(void)ftwbuf;
	return remove(fpath);
}

/* best-effort recursive removal without a shell (FTW_DEPTH: children first) */
static void remove_tree(const char *path)
{
	nftw(path, rm_tree_cb, 16, FTW_DEPTH | FTW_PHYS);
}

static void write_seed_file(const char *path, const void *buf, size_t len)
{
	FILE *f = fopen(path, "wb");

	if (f == NULL)
		die(path);
	if (fwrite(buf, 1, len, f) != len)
		die(path);
	fclose(f);
}

int main(void)
{
	static uint8_t seed[NFILES][FILE_SIZE];
	char wdir[] = "/tmp/rufus-wimsplit.XXXXXX";
	char srcdir[4096], outdir[4096], path[4096];
	char subtxt[] = "split me back together\n";
	uint8_t txtback[128];
	char *globpat;
	const wimlib_tchar *globs[1];
	const wimlib_tchar *paths[NFILES + 1];
	WIMStruct *wim = NULL, *part1 = NULL;
	DIR *d;
	struct dirent *de;
	int nparts = 0, nref = 0, fd, i;
	FILE *f;

	CHECK(wimlib_global_init(0) == 0, "wimlib_global_init");

	if (mkdtemp(wdir) == NULL)
		die("mkdtemp");
	snprintf(srcdir, sizeof(srcdir), "%s/src", wdir);
	snprintf(outdir, sizeof(outdir), "%s/out", wdir);
	if ((mkdir(srcdir, 0755) != 0) || (mkdir(outdir, 0755) != 0))
		die("mkdir");

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		die("urandom");
	for (i = 0; i < NFILES; i++) {
		if (read(fd, seed[i], FILE_SIZE) != FILE_SIZE)
			die("read urandom");
		snprintf(path, sizeof(path), "%s/f%02d.bin", srcdir, i);
		write_seed_file(path, seed[i], FILE_SIZE);
	}
	close(fd);
	snprintf(path, sizeof(path), "%s/nested", srcdir);
	if (mkdir(path, 0755) != 0)
		die("mkdir nested");
	snprintf(path, sizeof(path), "%s/nested/sub.txt", srcdir);
	write_seed_file(path, subtxt, sizeof(subtxt));

	/* capture -> base.wim */
	CHECK(wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_LZX,
		&wim) == 0, "create_new_wim");
	snprintf(path, sizeof(path), "%s/base.wim", wdir);
	CHECK((wim != NULL) && (wimlib_add_image(wim, srcdir, "img",
		NULL, 0) == 0), "add_image");
	CHECK(wimlib_write(wim, path, 1, 0, 0) == 0, "write base.wim");
	wimlib_free(wim);

	/* split -> parts of 512 KiB */
	CHECK(wimlib_open_wim(path, 0, &wim) == 0, "open base.wim");
	snprintf(path, sizeof(path), "%s/p.swm", wdir);
	CHECK(wimlib_split(wim, path, PART_SIZE, 0) == 0, "split");
	wimlib_free(wim);

	snprintf(path, sizeof(path), "%s", wdir);
	d = opendir(path);
	while ((de = readdir(d)) != NULL) {
		size_t n = strlen(de->d_name);

		if ((n > 4) && (strcmp(de->d_name + n - 4, ".swm") == 0))
			nparts++;
	}
	closedir(d);
	CHECK(nparts >= 4, "multiple parts created");

	/* read back through part 1 + referenced rest */
	snprintf(path, sizeof(path), "%s/p.swm", wdir);
	CHECK(wimlib_open_wim(path, 0, &part1) == 0, "open part 1");
	asprintf(&globpat, "%s/p?*.swm", wdir);
	globs[0] = globpat;
	CHECK(wimlib_reference_resource_files(part1, globs, 1,
		WIMLIB_REF_FLAG_GLOB_ENABLE, 0) == 0,
		"reference other parts");
	free(globpat);

	for (i = 0; i < NFILES; i++) {
		snprintf(path, sizeof(path), "f%02d.bin", i);
		paths[nref++] = strdup(path);
	}
	paths[nref++] = "nested/sub.txt";
	CHECK(wimlib_extract_paths(part1, 1, outdir, paths,
		(size_t)nref, 0) == 0, "extract from split set");

	for (i = 0; i < NFILES; i++) {
		uint8_t back[FILE_SIZE];
		char name[64];
		size_t got;

		snprintf(path, sizeof(path), "%s/f%02d.bin", outdir, i);
		snprintf(name, sizeof(name),
			"f%02d.bin round-trips byte-for-byte", i);
		f = fopen(path, "rb");
		if (f == NULL) {
			CHECK(0, name);
			continue;
		}
		got = fread(back, 1, sizeof(back), f);
		fclose(f);
		CHECK((got == FILE_SIZE) &&
			(memcmp(back, seed[i], FILE_SIZE) == 0), name);
	}

	snprintf(path, sizeof(path), "%s/nested/sub.txt", outdir);
	f = fopen(path, "rb");
	CHECK(f != NULL, "nested/sub.txt exists");
	if (f != NULL) {
		size_t got = fread(txtback, 1, sizeof(txtback) - 1, f);

		fclose(f);
		txtback[got] = '\0';
		CHECK((got == sizeof(subtxt)) &&
			(memcmp(txtback, subtxt, sizeof(subtxt)) == 0),
			"nested/sub.txt round-trips");
	}

	/* best-effort cleanup */
	remove_tree(wdir);

	printf("%s\n", g_failures ? "WIMSPLIT TEST: FAILED" :
		"WIMSPLIT TEST: ALL PASSED");
	return g_failures ? 1 : 0;
}
