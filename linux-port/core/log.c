/*
 * Rufus Linux port - logging
 * GPLv3
 */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int log_level = RLOG_INFO;

/*
 * Called before every printed line: lets a progress-bar owner break the
 * \r-overwritten bar line first, so log text never glues onto (or leaves
 * behind) a stale bar frame.
 */
static void (*g_linebreak_hook)(void);

void rlog_set_linebreak_hook(void (*fn)(void))
{
	g_linebreak_hook = fn;
}

void rlog_set_level(int level)
{
	if ((level >= RLOG_DEBUG) && (level <= RLOG_ERROR))
		log_level = level;
}

int rlog_get_level(void)
{
	return log_level;
}

void rlog(int level, const char *fmt, ...)
{
	va_list args;
	static const char prefix[4] = { 'D', 'I', 'W', 'E' };

	if (level < log_level || level > RLOG_ERROR)
		return;

	if (g_linebreak_hook != NULL)
		g_linebreak_hook();

	fprintf(stderr, "[rufus:%c] ", prefix[level]);
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fputc('\n', stderr);
	fflush(stderr);
}
