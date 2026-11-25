/* 試験用ファイルを排他的に作り、ページキャッシュ有効時の I/O と永続化待ちを測る。 */
#include "platform.h"
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

enum { MIB = 1048576, BLOCK = 4096, READS = 4096, WRITES = 128 };
static uint8_t input[MIB], output[MIB];
static const char *created_path;
static FILE *file;

static void cleanup(void) {
    if (file) fclose(file);
    if (created_path && remove(created_path)) fprintf(stderr, "Cleanup failed for %s\n", created_path);
}

static void persist(void) {
    if (fflush(file)) fail("fflush failed");
#ifdef _WIN32
    if (_commit(_fileno(file))) fail("_commit failed");
#else
    if (fsync(fileno(file))) fail("fsync failed");
#endif
}

static void seek(long offset) {
    if (fseek(file, offset, SEEK_SET)) fail("File seek failed");
}

static int compare_double(const void *a, const void *b) {
    const double first = *(const double *)a, second = *(const double *)b;
    return (first > second) - (first < second);
}

static long random_offset(uint32_t *state, long bytes) {
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return (long)((*state >> 8) % (uint32_t)(bytes / BLOCK)) * BLOCK;
}

int main(int argc, char **argv) {
    if (argc != 3) fail("usage: storage-bench path-to-new-file size-MiB (16 to 512)");
    char *end;
    const unsigned long mib = strtoul(argv[2], &end, 10);
    if (*end || mib < 16 || mib > 512) fail("Invalid file size");
    const long bytes = (long)mib * MIB;
    if (atexit(cleanup)) fail("Could not register temporary-file cleanup");
#ifdef _WIN32
    int fd;
    if (_sopen_s(&fd, argv[1], _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _SH_DENYRW, _S_IREAD | _S_IWRITE)) {
        fail("Could not exclusively create output file");
    }
    created_path = argv[1];
    file = _fdopen(fd, "w+b");
    if (!file) {
        _close(fd);
        fail("Could not open the new file stream");
    }
#else
    file = fopen(argv[1], "w+bx");
    if (!file) fail("Could not exclusively create output file");
    created_path = argv[1];
#endif
    if (setvbuf(file, NULL, _IONBF, 0)) fail("Could not disable stdio buffering");

    uint32_t pattern = 1;
    for (size_t i = 0; i < sizeof(input); ++i) {
        pattern = pattern * UINT32_C(1664525) + UINT32_C(1013904223);
        input[i] = (uint8_t)(pattern >> 24);
    }
    double start = clock_ns();
    for (unsigned long i = 0; i < mib; ++i) {
        uint64_t marker = i;
        memcpy(input, &marker, sizeof(marker));
        if (fwrite(input, 1, MIB, file) != MIB) fail("Sequential write failed");
    }
    const double write_ns = clock_ns() - start;
    start = clock_ns();
    persist();
    const double flush_ns = clock_ns() - start;

    seek(0);
    start = clock_ns();
    for (unsigned long i = 0; i < mib; ++i) {
        if (fread(output, 1, MIB, file) != MIB) fail("Sequential read failed");
        uint64_t observed;
        memcpy(&observed, output, sizeof(observed));
        if (observed != i) fail("Sequential read position mismatch");
    }
    const double read_ns = clock_ns() - start;
    seek(0);
    for (unsigned long i = 0; i < mib; ++i) {
        uint64_t marker = i;
        memcpy(input, &marker, sizeof(marker));
        if (fread(output, 1, MIB, file) != MIB || memcmp(input, output, MIB)) fail("File verification failed");
    }

    uint32_t state = 31;
    start = clock_ns();
    for (unsigned i = 0; i < READS; ++i) {
        const long position = random_offset(&state, bytes);
        seek(position);
        if (fread(output, 1, BLOCK, file) != BLOCK) fail("Random read failed");
        const size_t offset = (size_t)position % MIB;
        const uint64_t marker = (uint64_t)position / MIB;
        memcpy(input, &marker, sizeof(marker));
        if (output[0] != input[offset] || output[BLOCK - 1] != input[offset + BLOCK - 1]) {
            fail("Random read verification failed");
        }
    }
    const double random_read_ns = clock_ns() - start;

    double durable_ns[WRITES];
    for (unsigned i = 0; i < WRITES; ++i) {
        const long position = random_offset(&state, bytes);
        const uint64_t marker = UINT64_C(0x9876543200000000) | i;
        memcpy(input, &marker, sizeof(marker));
        start = clock_ns();
        seek(position);
        if (fwrite(input, 1, BLOCK, file) != BLOCK) fail("Random write failed");
        persist();
        durable_ns[i] = clock_ns() - start;
        seek(position);
        if (fread(output, 1, BLOCK, file) != BLOCK || memcmp(input, output, BLOCK)) fail("Random write verification failed");
    }
    qsort(durable_ns, WRITES, sizeof(*durable_ns), compare_double);

    if (fclose(file)) {
        file = NULL;
        fail("Closing the test file failed");
    }
    file = NULL;
    if (remove(created_path)) fail("Removing the test file failed");
    created_path = NULL;
    if (!(write_ns > 0 && flush_ns > 0 && read_ns > 0 && random_read_ns > 0)) fail("Timer resolution too low");
    printf("{\"file_mib\":%lu,\"page_cache\":\"enabled\",\"seq_write_and_flush_mib_s\":%.5f,"
           "\"seq_cached_read_mib_s\":%.5f,\"flush_ms\":%.5f,\"random_cached_4k_read_iops\":%.5f,"
           "\"random_durable_4k_write_p50_ms\":%.5f,\"random_durable_4k_write_p95_ms\":%.5f,\"validated\":true}\n",
           mib, (double)mib * 1e9 / (write_ns + flush_ns), (double)mib * 1e9 / read_ns, flush_ns / 1e6,
           READS * 1e9 / random_read_ns, (durable_ns[WRITES / 2 - 1] + durable_ns[WRITES / 2]) / 2e6,
           durable_ns[(WRITES * 95 + 99) / 100 - 1] / 1e6);
    return 0;
}
