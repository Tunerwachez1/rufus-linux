/*
 * config.h for building the vendored libcdio on Linux.
 * Mirrors the MinGW config.h shipped next to the sources; quoted
 * "config.h" includes resolve here first because this directory comes
 * before others on the include path.
 */
#ifndef RUFUS_LINUX_LIBCDIO_CONFIG_H
#define RUFUS_LINUX_LIBCDIO_CONFIG_H

/* libcdio features we need */
#define EMPTY_ARRAY_SIZE
#define HAVE_JOLIET 1
#define HAVE_ROCK 1
#define HAVE_ICONV 1
/* glibc's iconv() takes non-const char ** - expand ICONV_CONST to nothing */
#define ICONV_CONST

/* headers present on Linux/glibc */
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LANGINFO_CODESET 1
#define HAVE_LIMITS_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_MEMORY_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1

/* functions available on Linux/glibc */
#define HAVE_CHDIR 1
#define HAVE_FTRUNCATE 1
#define HAVE_FSEEKO 1
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_LSTAT 1
#define HAVE_MEMCPY 1
#define HAVE_MEMSET 1
#define HAVE_RAND 1
#define HAVE_READLINK 1
#define HAVE_REALPATH 1
#define HAVE_SETENV 1
#define HAVE_UNSETENV 1
#define HAVE_SNPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_STRDUP 1
#define HAVE_STRNDUP 1
#define HAVE_S_ISLNK 1
#define HAVE_S_ISSOCK 1
#define HAVE_STRUCT_TIMESPEC 1
#define HAVE_STRUCT_TM_TM_ZONE 1
#define HAVE_TM_GMTOFF 1
#define HAVE_TZNAME 1
#define HAVE_TZSET 1
#define HAVE_TIMEGM 1
#define HAVE_USLEEP 1
#define HAVE_DAYLIGHT 1

/* not available / not wanted on this platform */
/* #undef HAVE_FSEEKO64 */
/* #undef HAVE_LSEEK64 */
/* #undef HAVE_SETEGID */
/* #undef HAVE_SETEUID */
/* #undef HAVE_SLEEP */
/* #undef HAVE_DRAND48 */
/* #undef HAVE_ISOC99_PRAGMA */
/* #undef HAVE_SYS_CDIO_H */
/* #undef HAVE_WIN32_CDROM */
/* #undef HAVE__STATI64 */

#endif
