/*
 * fs_workload: Direct-block filesystem comparison helper (ext4 vs F2FS on Android).
 * Measures on a given mount directory:
 *   1. small_files: 500x (create + write 4 KiB + fsync + close), then read+verify, then unlink
 *   2. durable_tx: 200x SQLite-like WAL-style durable transactions (append 8 KiB to wal + fdatasync,
 *      periodic 64 KiB checkpoint to db + fdatasync every 20 tx)
 *   3. app_install_read: write 64 files x 512 KiB (32 MiB) + fsync + dir fsync, then drop_caches + read all
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void drop_caches(void) {
    sync();
    int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (fd >= 0) {
        if (write(fd, "3\n", 2) < 0) {}
        close(fd);
    }
}

static void die(const char *msg) {
    fprintf(stderr, "%s (errno=%d: %s)\n", msg, errno, strerror(errno));
    exit(1);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: fs_workload <mount_dir>\n");
        return 1;
    }
    const char *dir = argv[1];
    char work[512];
    snprintf(work, sizeof(work), "%s/wl", dir);
    mkdir(work, 0755);

    /* 1. Small files: 500 x 4 KiB create + write + fsync */
    enum { NSMALL = 500, SMALL_SZ = 4096 };
    uint8_t sbuf[SMALL_SZ];
    for (int i = 0; i < SMALL_SZ; ++i) sbuf[i] = (uint8_t)((i * 37 + 11) & 0xFF);

    double t0 = now_ms();
    char path[600];
    for (int i = 0; i < NSMALL; ++i) {
        snprintf(path, sizeof(path), "%s/sm_%04d.dat", work, i);
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) die("small open write");
        sbuf[0] = (uint8_t)(i & 0xFF);
        if (write(fd, sbuf, SMALL_SZ) != SMALL_SZ) die("small write");
        if (fsync(fd) != 0) die("small fsync");
        close(fd);
    }
    int dfd = open(work, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    double small_create_ms = now_ms() - t0;

    drop_caches();
    t0 = now_ms();
    uint8_t rbuf[SMALL_SZ];
    for (int i = 0; i < NSMALL; ++i) {
        snprintf(path, sizeof(path), "%s/sm_%04d.dat", work, i);
        int fd = open(path, O_RDONLY);
        if (fd < 0) die("small open read");
        if (read(fd, rbuf, SMALL_SZ) != SMALL_SZ) die("small read");
        if (rbuf[0] != (uint8_t)(i & 0xFF)) die("small verify mismatch");
        close(fd);
    }
    double small_read_ms = now_ms() - t0;

    t0 = now_ms();
    for (int i = 0; i < NSMALL; ++i) {
        snprintf(path, sizeof(path), "%s/sm_%04d.dat", work, i);
        if (unlink(path) != 0) die("small unlink");
    }
    dfd = open(work, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    double small_unlink_ms = now_ms() - t0;

    /* 2. Durable WAL-style transactions (200 tx x 8 KiB WAL fdatasync + 10 checkpoints x 64 KiB DB fdatasync) */
    enum { NTX = 200, WAL_SZ = 8192, DB_SZ = 65536 };
    uint8_t wal_buf[WAL_SZ];
    uint8_t db_buf[DB_SZ];
    memset(wal_buf, 0x5A, WAL_SZ);
    memset(db_buf, 0xA5, DB_SZ);
    char wal_p[600], db_p[600];
    snprintf(wal_p, sizeof(wal_p), "%s/tx.wal", work);
    snprintf(db_p, sizeof(db_p), "%s/tx.db", work);
    int wfd = open(wal_p, O_CREAT | O_RDWR | O_TRUNC, 0644);
    int bfd = open(db_p, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (wfd < 0 || bfd < 0) die("tx open");
    if (write(bfd, db_buf, DB_SZ) != DB_SZ || fsync(bfd) != 0) die("tx init db");

    t0 = now_ms();
    for (int tx = 0; tx < NTX; ++tx) {
        wal_buf[0] = (uint8_t)(tx & 0xFF);
        if (write(wfd, wal_buf, WAL_SZ) != WAL_SZ) die("wal write");
        if (fdatasync(wfd) != 0) die("wal fdatasync");
        if (((tx + 1) % 20) == 0) {
            db_buf[0] = (uint8_t)(tx & 0xFF);
            if (pwrite(bfd, db_buf, DB_SZ, 0) != DB_SZ) die("db checkpoint write");
            if (fdatasync(bfd) != 0) die("db checkpoint fdatasync");
        }
    }
    double tx_ms = now_ms() - t0;
    close(wfd);
    close(bfd);
    unlink(wal_p);
    unlink(db_p);

    /* 3. Simulated app install/extract (64 files x 512 KiB = 32 MiB) + cold read */
    enum { NAPP = 64, APP_SZ = 512 * 1024 };
    uint8_t *abuf = malloc(APP_SZ);
    if (!abuf) die("malloc");
    for (int i = 0; i < APP_SZ; ++i) abuf[i] = (uint8_t)((i * 13 + 7) & 0xFF);

    t0 = now_ms();
    for (int i = 0; i < NAPP; ++i) {
        snprintf(path, sizeof(path), "%s/pkg_%03d.bin", work, i);
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) die("app open write");
        abuf[0] = (uint8_t)i;
        if (write(fd, abuf, APP_SZ) != APP_SZ) die("app write");
        if (fsync(fd) != 0) die("app fsync");
        close(fd);
    }
    dfd = open(work, O_RDONLY);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    double app_install_ms = now_ms() - t0;

    drop_caches();
    t0 = now_ms();
    uint64_t sum = 0;
    for (int i = 0; i < NAPP; ++i) {
        snprintf(path, sizeof(path), "%s/pkg_%03d.bin", work, i);
        int fd = open(path, O_RDONLY);
        if (fd < 0) die("app open read");
        if (read(fd, abuf, APP_SZ) != APP_SZ) die("app read");
        if (abuf[0] != (uint8_t)i) die("app verify mismatch");
        sum += abuf[APP_SZ - 1];
        close(fd);
    }
    double app_read_ms = now_ms() - t0;

    for (int i = 0; i < NAPP; ++i) {
        snprintf(path, sizeof(path), "%s/pkg_%03d.bin", work, i);
        unlink(path);
    }
    rmdir(work);
    free(abuf);

    printf("{\"validated\":true,\"small_files\":%d,\"small_create_fsync_ms\":%.2f,\"small_create_per_s\":%.1f,"
           "\"small_cold_read_ms\":%.2f,\"small_unlink_ms\":%.2f,"
           "\"durable_tx_count\":%d,\"durable_tx_ms\":%.2f,\"durable_tx_per_s\":%.1f,\"durable_tx_avg_ms\":%.3f,"
           "\"app_32m_install_ms\":%.2f,\"app_32m_install_mbs\":%.1f,"
           "\"app_32m_cold_read_ms\":%.2f,\"app_32m_cold_read_mbs\":%.1f,\"sum\":%" PRIu64 "}\n",
           NSMALL, small_create_ms, NSMALL / (small_create_ms / 1e3),
           small_read_ms, small_unlink_ms,
           NTX, tx_ms, NTX / (tx_ms / 1e3), tx_ms / NTX,
           app_install_ms, 32.0 / (app_install_ms / 1e3),
           app_read_ms, 32.0 / (app_read_ms / 1e3), sum);
    return 0;
}
