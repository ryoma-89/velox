/* Android ゲストの vCPU 間の時刻逆行とホスト時計との比較に使う。 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { MONO, RAW, CLOCK_COUNT };
static const clockid_t clock_ids[CLOCK_COUNT] = {CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW};
static const uint64_t ns_per_second = UINT64_C(1000000000);

struct clocks {
    uint64_t time[CLOCK_COUNT];
    uint64_t boottime;
    uint64_t realtime;
};

struct stats {
    uint64_t reads;
    uint64_t warps;
    uint64_t max_warp;
};

struct worker {
    int cpu;
    bool idle;
    bool failed;
    struct stats pinned[CLOCK_COUNT];
};

struct migration {
    int last_cpu;
    uint64_t count;
    uint64_t previous[CLOCK_COUNT];
    struct stats stats[CLOCK_COUNT];
};

static _Atomic uint64_t latest[CLOCK_COUNT];
static atomic_bool stop;

static bool clock_ns(clockid_t clock, uint64_t *value) {
    struct timespec t;
    if (clock_gettime(clock, &t) != 0) return false;
    if (t.tv_sec < 0 || t.tv_nsec < 0 || (uint64_t)t.tv_nsec >= ns_per_second ||
        (uint64_t)t.tv_sec > (UINT64_MAX - (uint64_t)t.tv_nsec) / ns_per_second) {
        errno = EOVERFLOW;
        return false;
    }
    *value = (uint64_t)t.tv_sec * ns_per_second + (uint64_t)t.tv_nsec;
    return true;
}

static bool read_clocks(struct clocks *clocks) {
    return clock_ns(CLOCK_MONOTONIC, &clocks->time[MONO]) &&
           clock_ns(CLOCK_MONOTONIC_RAW, &clocks->time[RAW]) &&
           clock_ns(CLOCK_BOOTTIME, &clocks->boottime) &&
           clock_ns(CLOCK_REALTIME, &clocks->realtime);
}

static void print_clocks(const struct clocks *clocks) {
    printf("{\"monotonic_ns\":%" PRIu64 ",\"monotonic_raw_ns\":%" PRIu64
           ",\"boottime_ns\":%" PRIu64 ",\"realtime_ns\":%" PRIu64 "}",
           clocks->time[MONO], clocks->time[RAW], clocks->boottime, clocks->realtime);
}

static bool read_source(char source[64]) {
    FILE *f = fopen("/sys/devices/system/clocksource/clocksource0/current_clocksource", "r");
    if (!f) return false;
    const bool read = fgets(source, 64, f) != NULL;
    const int closed = fclose(f);
    if (closed != 0) return false;
    if (!read) {
        errno = EIO;
        return false;
    }
    source[strcspn(source, "\r\n")] = '\0';
    if (!source[0] || source[strspn(source, "abcdefghijklmnopqrstuvwxyz"
                                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")]) {
        errno = EINVAL;
        return false;
    }
    return true;
}

static bool sleep_for(time_t seconds, long nanoseconds) {
    struct timespec duration = {.tv_sec = seconds, .tv_nsec = nanoseconds};
    while (nanosleep(&duration, &duration) != 0) {
        if (errno != EINTR) return false;
    }
    return true;
}

static bool on_cpu(int cpu) {
    int current = sched_getcpu();
    if (current < 0) return false;
    if (current != cpu) {
        errno = EXDEV;
        return false;
    }
    return true;
}

static bool pin_cpu(int cpu) {
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    return sched_setaffinity(0, sizeof(affinity), &affinity) == 0 && on_cpu(cpu);
}

static void observe(struct stats *stats, uint64_t before, uint64_t now) {
    ++stats->reads;
    if (now < before) {
        ++stats->warps;
        const uint64_t warp = before - now;
        if (warp > stats->max_warp) stats->max_warp = warp;
    }
}

static bool observe_pinned(struct stats clocks[CLOCK_COUNT]) {
    for (int clock = 0; clock < CLOCK_COUNT; ++clock) {
        /* 比較対象の読み出しを今回の時刻取得より先に行い、取得順を確定する。 */
        const uint64_t before = atomic_load_explicit(&latest[clock], memory_order_acquire);
        uint64_t now;
        if (!clock_ns(clock_ids[clock], &now)) return false;
        observe(&clocks[clock], before, now);
        uint64_t expected = before;
        while (now > expected && !atomic_compare_exchange_weak_explicit(
                                     &latest[clock], &expected, now, memory_order_release,
                                     memory_order_relaxed)) {}
    }
    return true;
}

static void *run_worker(void *arg) {
    struct worker *worker = arg;
    bool ok = pin_cpu(worker->cpu);
    while (ok && !atomic_load_explicit(&stop, memory_order_relaxed)) {
        ok = observe_pinned(worker->pinned);
        if (ok && (worker->idle || worker->pinned[MONO].reads % 4096 == 0))
            ok = on_cpu(worker->cpu);
        if (ok && worker->idle) ok = sleep_for(0, 100000000);
    }
    worker->failed = !ok || !on_cpu(worker->cpu);
    if (worker->failed) fprintf(stderr, "vCPU %d: %s\n", worker->cpu, strerror(errno));
    return NULL;
}

static bool sweep_cpus(const struct worker *workers, int count, const cpu_set_t *original,
                       struct migration *migration) {
    bool ok = true;
    for (int index = 0; index < count && ok; ++index) {
        const int cpu = workers[index].cpu;
        uint64_t readings[CLOCK_COUNT];
        ok = pin_cpu(cpu) && clock_ns(CLOCK_MONOTONIC, &readings[MONO]) &&
             clock_ns(CLOCK_MONOTONIC_RAW, &readings[RAW]) && on_cpu(cpu);
        if (!ok) break;
        if (migration->last_cpu >= 0 && migration->last_cpu != cpu) ++migration->count;
        migration->last_cpu = cpu;
        for (int clock = 0; clock < CLOCK_COUNT; ++clock) {
            observe(&migration->stats[clock], migration->previous[clock], readings[clock]);
            migration->previous[clock] = readings[clock];
        }
    }
    if (!ok) perror("vCPU migration");
    if (sched_setaffinity(0, sizeof(*original), original) == 0) return ok;
    perror("restore affinity");
    return false;
}

static bool measure_cost(clockid_t clock, double *cost) {
    enum { CALLS = 200000 };
    uint64_t first, last;
    struct timespec reading;
    if (!clock_ns(clock, &first)) return false;
    for (int i = 0; i < CALLS; ++i) {
        if (clock_gettime(clock, &reading) != 0) return false;
    }
    if (!clock_ns(clock, &last)) return false;
    if (last <= first) {
        errno = ERANGE;
        return false;
    }
    *cost = (double)(last - first) / CALLS;
    return true;
}

static void print_stats(const struct stats clocks[CLOCK_COUNT]) {
    const char *names[] = {"monotonic", "monotonic_raw"};
    printf("{");
    for (int clock = 0; clock < CLOCK_COUNT; ++clock) {
        const struct stats *stats = &clocks[clock];
        printf("%s\"%s\":{\"reads\":%" PRIu64 ",\"warps\":%" PRIu64
               ",\"max_warp_ns\":%" PRIu64 "}", clock == MONO ? "" : ",",
               names[clock], stats->reads, stats->warps, stats->max_warp);
    }
    printf("}");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--sample") == 0) {
        struct clocks clocks;
        if (!read_clocks(&clocks)) {
            perror("clock_gettime");
            return 2;
        }
        print_clocks(&clocks);
        putchar('\n');
        return 0;
    }

    char *end = NULL;
    errno = 0;
    const long seconds = argc > 1 ? strtol(argv[1], &end, 10) : 10;
    const bool idle = argc == 3 && strcmp(argv[2], "--idle") == 0;
    if (argc > 3 || (argc == 3 && !idle) || errno || (argc > 1 && *end) ||
        seconds < 1 || seconds > 600) {
        fprintf(stderr, "usage: clockcheck [1-600 seconds [--idle]] | --sample\n");
        return 2;
    }

    cpu_set_t allowed;
    char source_before[64], source_after[64];
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online < 1) {
        fprintf(stderr, "online vCPU count is unavailable\n");
        return 2;
    }
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0 || !read_source(source_before)) {
        perror("CPU or clocksource information");
        return 2;
    }
    const int count = CPU_COUNT(&allowed);
    if (count != online) {
        fprintf(stderr, "CPU affinity covers %d of %ld online vCPUs\n", count, online);
        return 2;
    }

    struct worker workers[CPU_SETSIZE] = {0};
    pthread_t threads[CPU_SETSIZE];
    int index = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        workers[index].cpu = cpu;
        workers[index++].idle = idle;
    }
    double mono_cost, raw_cost;
    struct clocks start, finish;
    if (!pin_cpu(workers[0].cpu) || !measure_cost(CLOCK_MONOTONIC, &mono_cost) ||
        !measure_cost(CLOCK_MONOTONIC_RAW, &raw_cost) || !on_cpu(workers[0].cpu) ||
        sched_setaffinity(0, sizeof(allowed), &allowed) != 0 || !read_clocks(&start)) {
        perror("clock cost measurement");
        return 2;
    }

    int started = 0;
    for (; started < count; ++started) {
        const int error = pthread_create(&threads[started], NULL, run_worker, &workers[started]);
        if (!error) continue;
        fprintf(stderr, "pthread_create: %s\n", strerror(error));
        break;
    }
    bool ok = started == count;
    struct migration migration = {.last_cpu = -1};
    for (long i = 0; i <= seconds && ok; ++i) {
        if (i > 0 && !sleep_for(1, 0)) {
            perror("nanosleep");
            ok = false;
            break;
        }
        ok = sweep_cpus(workers, count, &allowed, &migration);
    }
    atomic_store_explicit(&stop, true, memory_order_relaxed);
    struct stats pinned[CLOCK_COUNT] = {0};
    for (int i = 0; i < started; ++i) {
        const int error = pthread_join(threads[i], NULL);
        if (error) {
            fprintf(stderr, "pthread_join: %s\n", strerror(error));
            return 2;
        }
        if (workers[i].failed || !workers[i].pinned[MONO].reads) ok = false;
        for (int clock = 0; clock < CLOCK_COUNT; ++clock) {
            const struct stats *sample = &workers[i].pinned[clock];
            pinned[clock].reads += sample->reads;
            pinned[clock].warps += sample->warps;
            if (sample->max_warp > pinned[clock].max_warp) pinned[clock].max_warp = sample->max_warp;
        }
    }
    if (!read_clocks(&finish) || !read_source(source_after)) {
        perror("final clock or clocksource");
        return 2;
    }
    printf("{\"schema_version\":2,\"clocksource\":\"%s\",\"clocksource_end\":\"%s\","
           "\"cpus\":%d,\"seconds\":%ld,\"idle\":%s,\"clock_gettime_ns\":%.2f,"
           "\"clock_gettime_raw_ns\":%.2f,\"migrations\":%" PRIu64 ",\"start\":",
           source_before, source_after, count, seconds, idle ? "true" : "false",
           mono_cost, raw_cost, migration.count);
    print_clocks(&start);
    printf(",\"end\":");
    print_clocks(&finish);
    printf(",\"pinned\":");
    print_stats(pinned);
    printf(",\"migrating\":");
    print_stats(migration.stats);
    printf(",\"vcpus\":[");
    for (int i = 0; i < count; ++i)
        printf("%s{\"cpu\":%d,\"reads\":%" PRIu64 "}", i ? "," : "",
               workers[i].cpu, workers[i].pinned[MONO].reads);
    puts("]}");

    if (!ok) return 2;
    for (int clock = 0; clock < CLOCK_COUNT; ++clock) {
        if (pinned[clock].warps || migration.stats[clock].warps ||
            finish.time[clock] < start.time[clock]) return 1;
    }
    return strcmp(source_before, source_after) != 0;
}
