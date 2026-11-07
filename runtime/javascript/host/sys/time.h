// QuickJS's clock wrapper for host tests on Windows.
#pragma once
#include <time.h>
struct timeval { long long tv_sec; long tv_usec; };
static inline int gettimeofday(struct timeval *out, void *zone) {
    (void)zone; struct timespec now; timespec_get(&now, TIME_UTC);
    out->tv_sec = now.tv_sec; out->tv_usec = (long)(now.tv_nsec / 1000); return 0;
}
