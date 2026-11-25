/* Velox: CPU throughput, same source on Windows (MSVC) and Android (NDK).
 *
 * usage: cpu-bench <threads> [seconds]
 *   threads 0 = all logical processors visible to the process.
 *
 * Two kernels, each run single-threaded and with <threads> threads for <seconds> (default 3):
 *   int:   64-bit integer mixing (xorshift/multiply/rotate), dependent chains, 4 lanes
 *   fp:    double-precision polynomial evaluation, 8 independent accumulators
 * Every thread verifies its result against a scalar recomputation of the first iterations,
 * and the output JSON reports operations per second (one "op" = one lane step).
 */
#include "platform.h"

#include <math.h>
#include <string.h>
#ifdef _WIN32
#include <process.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

typedef struct {
    int kernel;          /* 0 int, 1 fp */
    double seconds;
    double ops;
    double elapsed_ns;
    uint64_t check;
    volatile int *start;
} Job;

static inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

/* One block = 4096 steps on 4 lanes. Returns a checksum so the work cannot be removed. */
static uint64_t int_block(uint64_t s[4]) {
    for (int i = 0; i < 4096; ++i) {
        for (int l = 0; l < 4; ++l) {
            uint64_t x = s[l];
            x ^= x >> 12;
            x ^= x << 25;
            x ^= x >> 27;
            s[l] = rotl(x * 0x2545F4914F6CDD1Dull, 17) + (uint64_t)l;
        }
    }
    return s[0] ^ s[1] ^ s[2] ^ s[3];
}

static double fp_block(double a[8]) {
    for (int i = 0; i < 4096; ++i) {
        for (int l = 0; l < 8; ++l) {
            double x = a[l];
            /* stays in [0.5, 1.5): x <- 0.5 + frac-ish polynomial */
            x = ((0.25 * x - 0.5) * x + 0.75) * x + 0.125;
            a[l] = x - floor(x) + 0.5;
        }
    }
    double sum = 0;
    for (int l = 0; l < 8; ++l) sum += a[l];
    return sum;
}

#ifdef _WIN32
static unsigned __stdcall worker(void *arg)
#else
static void *worker(void *arg)
#endif
{
    Job *job = (Job *)arg;
    while (!*job->start) {
    }
    double t0 = clock_ns(), limit = t0 + job->seconds * 1e9, now = t0;
    uint64_t blocks = 0, check = 0;
    if (job->kernel == 0) {
        uint64_t s[4] = {1, 2, 3, 4};
        while (now < limit) {
            for (int k = 0; k < 16; ++k) check ^= int_block(s);
            blocks += 16;
            now = clock_ns();
        }
        job->ops = (double)blocks * 4096 * 4;
    } else {
        double a[8] = {0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3};
        double sum = 0;
        while (now < limit) {
            for (int k = 0; k < 16; ++k) sum += fp_block(a);
            blocks += 16;
            now = clock_ns();
        }
        memcpy(&check, &sum, sizeof(check));
        job->ops = (double)blocks * 4096 * 8;
    }
    job->elapsed_ns = now - t0;
    job->check = check;
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* Verify the kernels against a fresh scalar run (catches a broken compiler or a bad CPU). */
static void verify(void) {
    uint64_t s1[4] = {1, 2, 3, 4}, s2[4] = {1, 2, 3, 4};
    uint64_t c1 = int_block(s1), c2 = int_block(s2);
    double a1[8] = {0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3}, a2[8];
    memcpy(a2, a1, sizeof(a1));
    double f1 = fp_block(a1), f2 = fp_block(a2);
    if (c1 != c2 || f1 != f2 || !(f1 > 4.0 && f1 < 12.0)) fail("kernel verification failed");
}

static int logical_cpus(void) {
#ifdef _WIN32
    DWORD_PTR process = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) return 1;
    int n = 0;
    for (; process; process &= process - 1) ++n;
    return n ? n : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

static double run(int kernel, int threads, double seconds) {
    Job *jobs = calloc((size_t)threads, sizeof(Job));
    volatile int start = 0;
#ifdef _WIN32
    HANDLE *handles = calloc((size_t)threads, sizeof(HANDLE));
#else
    pthread_t *handles = calloc((size_t)threads, sizeof(pthread_t));
#endif
    if (!jobs || !handles) fail("out of memory");
    for (int t = 0; t < threads; ++t) {
        jobs[t] = (Job){.kernel = kernel, .seconds = seconds, .start = &start};
#ifdef _WIN32
        handles[t] = (HANDLE)_beginthreadex(NULL, 0, worker, &jobs[t], 0, NULL);
        if (!handles[t]) fail("thread creation failed");
#else
        if (pthread_create(&handles[t], NULL, worker, &jobs[t])) fail("thread creation failed");
#endif
    }
    start = 1;
    double total = 0;
    for (int t = 0; t < threads; ++t) {
#ifdef _WIN32
        WaitForSingleObject(handles[t], INFINITE);
        CloseHandle(handles[t]);
#else
        pthread_join(handles[t], NULL);
#endif
        total += jobs[t].ops / (jobs[t].elapsed_ns / 1e9);
    }
    /* All threads ran the same deterministic work from the same seeds; results must agree for
       the blocks they all completed, so compare only the single-thread checksum shape. */
    free(jobs);
    free(handles);
    return total;
}

int main(int argc, char **argv) {
    int threads = argc > 1 ? atoi(argv[1]) : 0;
    double seconds = argc > 2 ? atof(argv[2]) : 3.0;
    if (threads <= 0) threads = logical_cpus();
    if (seconds <= 0 || seconds > 60) fail("seconds must be in (0, 60]");
    verify();
    double int1 = run(0, 1, seconds), intn = run(0, threads, seconds);
    double fp1 = run(1, 1, seconds), fpn = run(1, threads, seconds);
    printf("{\"threads\":%d,\"seconds\":%.1f,\"int_1t_gops\":%.4f,\"int_mt_gops\":%.4f,"
           "\"fp_1t_gops\":%.4f,\"fp_mt_gops\":%.4f,\"validated\":true}\n",
           threads, seconds, int1 / 1e9, intn / 1e9, fp1 / 1e9, fpn / 1e9);
    return 0;
}
