#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/*
 * Log errors
 */
void log_err(const char *format, ...)
{
	va_list arglist;

	printf("E: ");
	va_start(arglist, format);
	vprintf(format, arglist);
	printf("\n");
	va_end(arglist);
}

/*
 * Log warnings
 */
void log_warn(const char *format, ...)
{
	va_list arglist;

	printf("W: ");
	va_start(arglist, format);
	vprintf(format, arglist);
	printf("\n");
	va_end(arglist);
}

/*
 * Log debug messages
 */
void log_debug(const char *format, ...)
{
	va_list arglist;

	printf("D: ");
	va_start(arglist, format);
	vprintf(format, arglist);
	printf("\n");
	va_end(arglist);
}