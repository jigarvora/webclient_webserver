#ifndef __LOG_H__
#define __LOG_H__

/*
 * Log errors
 */
void log_err(const char *format, ...);

/*
 * Log warnings
 */
void log_warn(const char *format, ...);

/*
 * Log debug messages
 */
void log_debug(const char *format, ...);

#endif