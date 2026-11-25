/* Windows と Android で memcpy、依存ランダム読み取り、確保後のページ書き込みを測る。 */
#include "platform.h"
#include <string.h>
#ifdef _WIN32
#include <process.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#endif

static void *(*volatile copy_memory)(void *, const void *, size_t) = memcpy;

static uint32_t random_u32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static void maybe_advise_huge(void *ptr, size_t bytes, int use_huge) {
#if !defined(_WIN32) && defined(MADV_HUGEPAGE)
    if (use_huge && bytes >= 2 * 1024 * 1024) {
        (void)madvise(ptr, bytes, MADV_HUGEPAGE);
    }
#else
    (void)ptr; (void)bytes; (void)use_huge;
#endif
}

typedef struct {
    volatile long arrived;
    volatile long go;
    unsigned total;
} MemBarrier;

static void mem_barrier_wait(MemBarrier *b) {
#ifdef _WIN32
    long n = InterlockedIncrement(&b->arrived);
    if ((unsigned)n >= b->total) InterlockedExchange(&b->go, 1);
    else while (!InterlockedCompareExchange(&b->go, 0, 0)) YieldProcessor();
#else
    long n = __atomic_add_fetch(&b->arrived, 1, __ATOMIC_ACQ_REL);
    if ((unsigned)n >= b->total) __atomic_store_n(&b->go, 1, __ATOMIC_RELEASE);
    else while (!__atomic_load_n(&b->go, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
#endif
}

typedef struct {
    size_t bytes;
    size_t copies;
    int use_huge;
    unsigned tid;
    MemBarrier *barrier;
    double elapsed_ns;
    int ok;
} MtCopyJob;

#ifdef _WIN32
static unsigned __stdcall mt_copy_worker(void *arg)
#else
static void *mt_copy_worker(void *arg)
#endif
{
    MtCopyJob *j = (MtCopyJob *)arg;
    uint8_t *src = malloc(j->bytes);
    uint8_t *dst = malloc(j->bytes);
    if (!src || !dst) fail("MT allocation failed");
    maybe_advise_huge(src, j->bytes, j->use_huge);
    maybe_advise_huge(dst, j->bytes, j->use_huge);
    uint32_t seed = 1u + j->tid * 977u;
    for (size_t i = 0; i < j->bytes / sizeof(uint32_t); ++i) {
        uint32_t s = random_u32(&seed);
        memcpy(src + i * sizeof(s), &s, sizeof(s));
    }
    copy_memory(dst, src, j->bytes);
    mem_barrier_wait(j->barrier);
    double t0 = clock_ns();
    for (size_t c = 0; c < j->copies; ++c) copy_memory(dst, src, j->bytes);
    j->elapsed_ns = clock_ns() - t0;
    j->ok = (memcmp(dst, src, j->bytes) == 0);
    free(dst);
    free(src);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int main(int argc, char **argv) {
    unsigned long kib = 0;
    unsigned threads = 1;
    int use_huge = 0;
    int pos = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--madvise-hugepage")) {
            use_huge = 1;
        } else if (pos == 0) {
            char *end;
            kib = strtoul(argv[i], &end, 10);
            if (*end || kib < 16 || kib > 524288) fail("Invalid working-set size");
            pos = 1;
        } else if (pos == 1) {
            char *end;
            unsigned long t = strtoul(argv[i], &end, 10);
            if (*end || t < 1 || t > 32) fail("Invalid thread count (1..32)");
            threads = (unsigned)t;
            pos = 2;
        } else {
            fail("usage: memory-bench working-set-KiB [threads] [--madvise-hugepage]");
        }
    }
    if (pos == 0) fail("usage: memory-bench working-set-KiB [threads] [--madvise-hugepage]");

    const size_t bytes = (size_t)kib * 1024;
    if (threads > 1) {
        size_t per_thread = bytes / threads;
        if (per_thread < 4096) per_thread = 4096;
        per_thread &= ~(size_t)4095;
        size_t copies = (size_t)536870912 / per_thread;
        if (copies < 2) copies = 2;
        if (copies > 128) copies = 128;
        MtCopyJob jobs[32];
        MemBarrier barrier = {0, 0, threads};
        for (unsigned t = 0; t < threads; ++t) {
            jobs[t].bytes = per_thread;
            jobs[t].copies = copies;
            jobs[t].use_huge = use_huge;
            jobs[t].tid = t;
            jobs[t].barrier = &barrier;
            jobs[t].elapsed_ns = 0.0;
            jobs[t].ok = 0;
        }
#ifdef _WIN32
        HANDLE h[32];
        for (unsigned t = 0; t < threads; ++t) {
            h[t] = (HANDLE)_beginthreadex(NULL, 0, mt_copy_worker, &jobs[t], 0, NULL);
            if (!h[t]) fail("_beginthreadex failed");
        }
        for (unsigned t = 0; t < threads; ++t) { WaitForSingleObject(h[t], INFINITE); CloseHandle(h[t]); }
#else
        pthread_t th[32];
        for (unsigned t = 0; t < threads; ++t) {
            if (pthread_create(&th[t], NULL, mt_copy_worker, &jobs[t])) fail("pthread_create failed");
        }
        for (unsigned t = 0; t < threads; ++t) pthread_join(th[t], NULL);
#endif
        double max_ns = 0.0;
        for (unsigned t = 0; t < threads; ++t) {
            if (!jobs[t].ok) fail("MT copy verification failed");
            if (jobs[t].elapsed_ns > max_ns) max_ns = jobs[t].elapsed_ns;
        }
        const double total_gib = (double)threads * (double)per_thread * (double)copies / 1073741824.0;
        printf("{\"working_set_kib\":%lu,\"threads\":%u,\"madvise_hugepage\":%s,"
               "\"copy_gib_s\":%.5f,\"wall_ms\":%.3f,\"validated\":true}\n",
               kib, threads, use_huge ? "true" : "false", total_gib * 1e9 / max_ns, max_ns / 1e6);
        return 0;
    }

    uint8_t *source = malloc(bytes), *target = malloc(bytes);
    if (!source || !target) fail("Allocation failed");
    maybe_advise_huge(source, bytes, use_huge);
    maybe_advise_huge(target, bytes, use_huge);
    uint32_t seed = 1;
    for (size_t i = 0; i < bytes / sizeof(uint32_t); ++i) {
        uint32_t sample = random_u32(&seed);
        memcpy(source + i * sizeof(sample), &sample, sizeof(sample));
    }
    copy_memory(target, source, bytes);
    const size_t copies = (size_t)536870912 / bytes;
    double begin = clock_ns();
    for (size_t i = 0; i < copies; ++i) copy_memory(target, source, bytes);
    const double copy_ns = clock_ns() - begin;
    if (memcmp(target, source, bytes)) fail("Copy verification failed");

    /* 確保済みの 2 バッファを再利用し、訪問先が一巡するランダムな環を組む。 */
    uint32_t *order = (uint32_t *)source, *next = (uint32_t *)target;
    const uint32_t nodes = (uint32_t)(bytes / sizeof(*next));
    for (uint32_t i = 0; i < nodes; ++i) order[i] = i;
    for (uint32_t i = nodes - 1; i > 0; --i) {
        const uint32_t j = random_u32(&seed) % (i + 1);
        const uint32_t value = order[i];
        order[i] = order[j];
        order[j] = value;
    }
    for (uint32_t i = 1; i < nodes; ++i) next[order[i - 1]] = order[i];
    next[order[nodes - 1]] = order[0];
    const uint64_t steps = UINT64_C(4000000);
    uint32_t current = order[0];
    for (uint32_t i = 0; i < nodes; ++i) current = next[current];
    if (current != order[0]) fail("Random cycle warmup failed");
    begin = clock_ns();
    for (uint64_t i = 0; i < steps; ++i) current = next[current];
    const double read_ns = clock_ns() - begin;
    if (current != order[steps % nodes]) fail("Random cycle verification failed");

    /* 4 KiB ごとの初回書き込み。アロケーターの再利用と OS のページ割り当てを含む。 */
    begin = clock_ns();
    volatile uint8_t *allocation = malloc(bytes);
    if (!allocation) fail("Page benchmark allocation failed");
    maybe_advise_huge((void *)allocation, bytes, use_huge);
    for (size_t i = 0; i < bytes; i += 4096) allocation[i] = (uint8_t)(i / 4096);
    const double touch_ns = clock_ns() - begin;
    for (size_t i = 0; i < bytes; i += 4096)
        if (allocation[i] != (uint8_t)(i / 4096)) fail("Page touch verification failed");
    free((void *)allocation);
    free(target);
    free(source);

    if (!(copy_ns > 0 && read_ns > 0 && touch_ns > 0)) fail("Timer resolution too low");
    const double gib = (double)bytes * (double)copies / 1073741824.0;
    printf("{\"working_set_kib\":%lu,\"threads\":1,\"madvise_hugepage\":%s,\"copy_gib_s\":%.5f,"
           "\"random_load_ns\":%.5f,\"alloc_touch_ns_per_4k\":%.5f,\"random_loads\":%" PRIu64 ",\"validated\":true}\n",
           kib, use_huge ? "true" : "false", gib * 1e9 / copy_ns, read_ns / (double)steps,
           touch_ns / (double)(bytes / 4096), steps);
    return 0;
}
