/*
 * Rufus Linux port - logging
 * GPLv3
 */
#ifndef RUFUS_LOG_H
#define RUFUS_LOG_H

enum {
	RLOG_DEBUG = 0,
	RLOG_INFO,
	RLOG_WARN,
	RLOG_ERROR
};

void rlog_set_level(int level);
int rlog_get_level(void);
/* break the current progress-bar line before the next log line prints */
void rlog_set_linebreak_hook(void (*fn)(void));
void rlog(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define rlog_dbg(...) rlog(RLOG_DEBUG, __VA_ARGS__)
#define rlog_info(...) rlog(RLOG_INFO, __VA_ARGS__)
#define rlog_warn(...) rlog(RLOG_WARN, __VA_ARGS__)
#define rlog_err(...) rlog(RLOG_ERROR, __VA_ARGS__)

#endif
