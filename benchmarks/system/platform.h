#ifndef VELOX_SYSTEM_BENCH_PLATFORM_H
#define VELOX_SYSTEM_BENCH_PLATFORM_H

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

_Noreturn static void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static double clock_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER ticks, frequency;
    if (!QueryPerformanceCounter(&ticks) || !QueryPerformanceFrequency(&frequency)) fail("QPC failed");
    return 1e9 * (double)ticks.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) fail("CLOCK_MONOTONIC failed");
    return 1e9 * (double)now.tv_sec + (double)now.tv_nsec;
#endif
}

#endif
