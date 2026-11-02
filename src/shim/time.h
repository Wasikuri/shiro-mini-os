#ifndef SHIM_TIME_H
#define SHIM_TIME_H
#include <stddef.h>
#include <sys/types.h>
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};
struct timespec { time_t tv_sec; long tv_nsec; };
struct timeval { time_t tv_sec; long tv_usec; };
#define CLOCKS_PER_SEC 1000000
time_t time(time_t *t);
clock_t clock(void);
double difftime(time_t a, time_t b);
time_t mktime(struct tm *tm);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *tm);
struct tm *localtime_r(const time_t *t, struct tm *tm);
char *strptime(const char *s, const char *fmt, struct tm *tm);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
char *asctime(const struct tm *tm);
char *ctime(const time_t *t);
int gettimeofday(struct timeval *tv, void *tz);
int clock_gettime(int clk, struct timespec *ts);
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#endif
