#ifndef BUZZOS_TIME_COMPAT_H
#define BUZZOS_TIME_COMPAT_H

#include <sys/types.h>
#include "libc.h"

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

struct tm *gmtime(const time_t *value);
struct tm *localtime(const time_t *value);
time_t mktime(struct tm *value);
size_t strftime(char *buffer, size_t size, const char *format,
                const struct tm *value);
char *strptime(const char *text, const char *format, struct tm *value);
double difftime(time_t end, time_t beginning);

#endif
