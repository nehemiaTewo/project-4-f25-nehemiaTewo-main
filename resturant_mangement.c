#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <errno.h>
#include <semaphore.h>

#define ORDER_Q_CAP  256
#define DONE_Q_CAP   256

// Timestamp helper for logs
static void now_str(char *buf, size_t n) {
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm tm; localtime_r(&tv.tv_sec, &tm);
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d.%06ld",
             tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, (long)tv.tv_usec);
}

// Single-line, flushed logging
static void log_event(const char *role, unsigned long tid, const char *fmt, ...) {
    char ts[64]; now_str(ts, sizeof ts);
    fprintf(stdout, "%s | %s %lu | ", ts, role, tid);
    va_list ap; va_start(ap, fmt); vfprintf(stdout, fmt, ap); va_end(ap);
    fputc('\n', stdout); fflush(stdout);
}