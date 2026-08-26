/*
 * Hand-written config.h for building upstream wimlib 1.14.4 on Linux/glibc
 * inside the Rufus Linux port (meson, no autotools). Mirrors the flags
 * `./configure` would set on a stock Linux system; optional external
 * compressors (zlib/lzma), FUSE, ntfs-3g, libcrypto and pthreads stay off -
 * wimlib's built-in LZX/XPRESS/LZMS codecs are self-contained C.
 */
#ifndef WIMLIB_LINUX_CONFIG_H
#define WIMLIB_LINUX_CONFIG_H

#define PACKAGE "wimlib"
#define PACKAGE_BUGREPORT "https://wimlib.net/forums/"
#define PACKAGE_NAME "wimlib"
#define PACKAGE_STRING "wimlib 1.14.4"
#define PACKAGE_TARNAME "wimlib"
#define PACKAGE_URL "https://wimlib.net"
#define PACKAGE_VERSION "1.14.4"
#define VERSION "1.14.4"

/* detected headers */
#define HAVE_ALLOCA_H 1
#define HAVE_BYTESWAP_H 1
#define HAVE_ENDIAN_H 1
#define HAVE_ERRNO_H 1
#define HAVE_GLOB_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_FILE_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_UTIME_H 1

/* detected functions (glibc) */
#define HAVE_FDOPENDIR 1
#define HAVE_FLOCK 1
#define HAVE_FSTATAT 1
#define HAVE_FUTIMENS 1
#define HAVE_GETOPT_LONG_ONLY 1
#define HAVE_MEMPCPY 1
#define HAVE_OPENAT 1
#define HAVE_POSIX_FALLOCATE 1
#define HAVE_READLINKAT 1
#define HAVE_UTIMENSAT 1

/* extended attributes: present, but only needed by extract/capture paths */
#define HAVE_SYS_XATTR_H 1
#define HAVE_FSETXATTR 1
#define HAVE_LGETXATTR 1
#define HAVE_LLISTXATTR 1
#define HAVE_LSETXATTR 1

#define HAVE_STAT_NANOSECOND_PRECISION 1

#endif /* WIMLIB_LINUX_CONFIG_H */
