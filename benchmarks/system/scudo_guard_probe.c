/* Independent allocator / page-boundary probe (read-only observation).
 * Measures malloc() VMA mapping, alignment, distance to end of rw-p VMA,
 * next-page permissions (e.g. Scudo PROT_NONE guard page), and safely checks
 * via SIGSEGV/sigsetjmp whether an 8-byte read at (end - 4) faults.
 * Does NOT disable Scudo or weaken any memory protection.
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_fault_sig = 0;
static volatile sig_atomic_t g_fault_code = 0;
static volatile uintptr_t g_fault_addr = 0;

static void segv_handler(int sig, siginfo_t *si, void *ucontext) {
    (void)ucontext;
    g_fault_sig = sig;
    g_fault_code = si ? si->si_code : 0;
    g_fault_addr = si ? (uintptr_t)si->si_addr : 0;
    siglongjmp(g_jmp, 1);
}

static void find_vma(uintptr_t addr, char *cur_line, size_t cur_sz, char *next_line, size_t next_sz) {
    cur_line[0] = '\0';
    next_line[0] = '\0';
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (found) {
            snprintf(next_line, next_sz, "%s", line);
            break;
        }
        unsigned long long lo = 0, hi = 0;
        if (sscanf(line, "%llx-%llx", &lo, &hi) == 2) {
            if ((uintptr_t)lo <= addr && addr < (uintptr_t)hi) {
                snprintf(cur_line, cur_sz, "%s", line);
                found = 1;
            }
        }
    }
    fclose(f);
}

static int probe_8byte_read(const uint8_t *p, uint64_t *out_val) {
    g_fault_sig = 0;
    g_fault_code = 0;
    g_fault_addr = 0;
    if (sigsetjmp(g_jmp, 1) == 0) {
        volatile uint64_t v = 0;
        memcpy((void *)&v, p, 8);
        *out_val = v;
        return 0; /* no fault */
    }
    return 1; /* faulted */
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);

    long page_size = sysconf(_SC_PAGESIZE);
    const size_t sizes[] = {
        4096, 16384, 32768, 65536, 65536 + 4096,
        106496, 131072, 135168, 262144, 524288
    };
    const size_t n = sizeof(sizes) / sizeof(sizes[0]);

    printf("{\n  \"page_size\": %ld,\n  \"probes\": [\n", page_size);
    for (size_t i = 0; i < n; ++i) {
        size_t sz = sizes[i];
        uint8_t *buf = (uint8_t *)malloc(sz);
        if (!buf) continue;
        memset(buf, 0x5A, sz);
        uintptr_t p = (uintptr_t)buf;
        uintptr_t end = p + sz;
        char cur[512], nxt[512];
        find_vma(p, cur, sizeof(cur), nxt, sizeof(nxt));
        unsigned long long vma_lo = 0, vma_hi = 0;
        sscanf(cur, "%llx-%llx", &vma_lo, &vma_hi);
        uintptr_t gap_to_vma_end = (vma_hi >= end) ? ((uintptr_t)vma_hi - end) : 0;

        uint64_t val = 0;
        int fault_at_end_minus_4 = probe_8byte_read(buf + sz - 4, &val);
        int fault_at_vma_end_minus_4 = 0;
        uintptr_t f_addr = g_fault_addr;
        int f_code = g_fault_code;
        if (!fault_at_end_minus_4 && vma_hi >= p + 4) {
            fault_at_vma_end_minus_4 = probe_8byte_read((const uint8_t *)(uintptr_t)(vma_hi - 4), &val);
            if (fault_at_vma_end_minus_4) {
                f_addr = g_fault_addr;
                f_code = g_fault_code;
            }
        }

        printf("    {\"size\": %zu, \"ptr\": \"0x%" PRIxPTR "\", \"end\": \"0x%" PRIxPTR "\", "
               "\"end_mod_page\": %zu, \"gap_to_vma_end\": %zu, "
               "\"fault_at_alloc_end_minus_4\": %s, \"fault_at_vma_end_minus_4\": %s, "
               "\"fault_code\": %d, \"fault_addr\": \"0x%" PRIxPTR "\", "
               "\"vma\": \"%s\", \"next_vma\": \"%s\"}%s\n",
               sz, p, end, (size_t)(end % (uintptr_t)page_size), (size_t)gap_to_vma_end,
               fault_at_end_minus_4 ? "true" : "false",
               fault_at_vma_end_minus_4 ? "true" : "false",
               f_code, f_addr, cur, nxt, (i + 1 < n) ? "," : "");
        free(buf);
    }
    printf("  ]\n}\n");
    return 0;
}
