/* Velox Phase 19 Independent ARM64 vs x86_64 Translation Benchmark.
 * Single portable C source built for both x86_64 and aarch64 Android targets.
 * Verifies exact output equivalence between x86_64 native and ARM64 Native Bridge.
 */
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#define ARCH_NAME "aarch64"
#elif defined(__x86_64__)
#include <emmintrin.h>
#define ARCH_NAME "x86_64"
#else
#define ARCH_NAME "other"
#endif

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t fnv1a64(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* 1. Integer arithmetic */
static void bench_int(double *gops, uint64_t *csum) {
    const uint64_t N = 40000000ULL;
    uint64_t a = 0x123456789abcdef0ULL, b = 0xfedcba9876543211ULL;
    double t0 = now_s();
    for (uint64_t i = 0; i < N; ++i) {
        a = (a ^ (b >> 7)) * 6364136223846793005ULL + 1442695040888963407ULL;
        b = (b + (a >> 13)) ^ (i * 2654435761ULL);
    }
    double dt = now_s() - t0;
    *gops = (double)(N * 6ULL) / dt / 1e9;
    *csum = a ^ b;
}

/* 2. FP32 & FP64 */
static void bench_fp32(double *gflops, uint64_t *csum) {
    const int N = 4096, REPS = 25000;
    float *a = (float *)aligned_alloc(64, (size_t)N * sizeof(float));
    float *b = (float *)aligned_alloc(64, (size_t)N * sizeof(float));
    for (int i = 0; i < N; ++i) { a[i] = 1.0f + (float)(i & 63) * 0.015625f; b[i] = 0.5f + (float)(i & 31) * 0.03125f; }
    double t0 = now_s();
    float acc = 0.0f;
    for (int r = 0; r < REPS; ++r) {
        float s0 = 0.f, s1 = 0.f, s2 = 0.f, s3 = 0.f;
        for (int i = 0; i < N; i += 4) {
            s0 += a[i] * b[i]; s1 += a[i+1] * b[i+1]; s2 += a[i+2] * b[i+2]; s3 += a[i+3] * b[i+3];
        }
        acc += (s0 + s1) + (s2 + s3);
    }
    double dt = now_s() - t0;
    *gflops = (double)((uint64_t)N * (uint64_t)REPS * 2ULL) / dt / 1e9;
    *csum = (uint64_t)llround((double)acc);
    free(a); free(b);
}

static void bench_fp64(double *gflops, uint64_t *csum) {
    const int N = 4096, REPS = 15000;
    double *a = (double *)aligned_alloc(64, (size_t)N * sizeof(double));
    double *b = (double *)aligned_alloc(64, (size_t)N * sizeof(double));
    for (int i = 0; i < N; ++i) { a[i] = 1.0 + (double)(i & 63) * 0.015625; b[i] = 0.5 + (double)(i & 31) * 0.03125; }
    double t0 = now_s();
    double acc = 0.0;
    for (int r = 0; r < REPS; ++r) {
        double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
        for (int i = 0; i < N; i += 4) {
            s0 += a[i] * b[i]; s1 += a[i+1] * b[i+1]; s2 += a[i+2] * b[i+2]; s3 += a[i+3] * b[i+3];
        }
        acc += (s0 + s1) + (s2 + s3);
    }
    double dt = now_s() - t0;
    *gflops = (double)((uint64_t)N * (uint64_t)REPS * 2ULL) / dt / 1e9;
    *csum = (uint64_t)llround(acc);
    free(a); free(b);
}

/* 3. Explicit 128-bit SIMD (ARM64 NEON vs x86_64 SSE2): exact bit-identical integer vector mix */
static void bench_simd(double *gb_s, uint64_t *csum) {
    const size_t BYTES = 4 * 1024 * 1024;
    const int REPS = 80;
    uint8_t *src = (uint8_t *)aligned_alloc(64, BYTES);
    uint8_t *dst = (uint8_t *)aligned_alloc(64, BYTES);
    for (size_t i = 0; i < BYTES; ++i) { src[i] = (uint8_t)((i * 37 + 13) & 255); dst[i] = (uint8_t)((i * 17 + 7) & 255); }
    double t0 = now_s();
    for (int r = 0; r < REPS; ++r) {
        for (size_t i = 0; i < BYTES; i += 16) {
#if defined(__aarch64__)
            uint8x16_t va = vld1q_u8(src + i);
            uint8x16_t vb = vld1q_u8(dst + i);
            uint8x16_t vc = vaddq_u8(veorq_u8(va, vb), vdupq_n_u8(0x5a));
            vst1q_u8(dst + i, vc);
#elif defined(__x86_64__)
            __m128i va = _mm_load_si128((const __m128i *)(src + i));
            __m128i vb = _mm_load_si128((const __m128i *)(dst + i));
            __m128i vc = _mm_add_epi8(_mm_xor_si128(va, vb), _mm_set1_epi8(0x5a));
            _mm_store_si128((__m128i *)(dst + i), vc);
#else
            for (size_t k = 0; k < 16; ++k) dst[i + k] = (uint8_t)((src[i + k] ^ dst[i + k]) + 0x5a);
#endif
        }
    }
    double dt = now_s() - t0;
    *gb_s = ((double)BYTES * 3.0 * REPS) / dt / (1024.0 * 1024.0 * 1024.0);
    *csum = fnv1a64(dst, BYTES);
    free(src); free(dst);
}

/* 4. 64 MiB memcpy */
static void bench_memcpy(double *gib_s, uint64_t *csum) {
    const size_t SZ = 64 * 1024 * 1024;
    const int REPS = 20;
    uint8_t *a = (uint8_t *)aligned_alloc(64, SZ);
    uint8_t *b = (uint8_t *)aligned_alloc(64, SZ);
    for (size_t i = 0; i < SZ; i += 4096) { a[i] = (uint8_t)(i >> 12); b[i] = 0; }
    memcpy(b, a, SZ);
    double t0 = now_s();
    for (int r = 0; r < REPS; ++r) {
        a[0] = (uint8_t)r;
        memcpy(b, a, SZ);
    }
    double dt = now_s() - t0;
    *gib_s = ((double)SZ * REPS) / dt / (1024.0 * 1024.0 * 1024.0);
    *csum = fnv1a64(b, 65536);
    free(a); free(b);
}

/* 5. RLE/LZ compression + decompression round-trip */
static void bench_compress(double *mib_s, int *valid) {
    const size_t N = 2 * 1024 * 1024;
    uint8_t *raw = (uint8_t *)malloc(N);
    uint8_t *cmp = (uint8_t *)malloc(N * 2);
    uint8_t *dec = (uint8_t *)malloc(N);
    for (size_t i = 0; i < N; ++i) raw[i] = (uint8_t)(((i / 7) ^ (i >> 9)) & 255);
    const int REPS = 15;
    double t0 = now_s();
    int ok = 1;
    for (int r = 0; r < REPS; ++r) {
        size_t o = 0, i = 0;
        while (i < N) {
            uint8_t v = raw[i];
            size_t run = 1;
            while (i + run < N && raw[i + run] == v && run < 255) run++;
            cmp[o++] = (uint8_t)run;
            cmp[o++] = v;
            i += run;
        }
        size_t d = 0;
        for (size_t k = 0; k < o; k += 2) {
            memset(dec + d, cmp[k + 1], cmp[k]);
            d += cmp[k];
        }
        if (d != N || memcmp(raw, dec, N) != 0) ok = 0;
    }
    double dt = now_s() - t0;
    *mib_s = ((double)N * 2.0 * REPS) / dt / (1024.0 * 1024.0);
    *valid = ok;
    free(raw); free(cmp); free(dec);
}

/* 6. Portable SHA-256 */
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static inline uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t st[8], const uint8_t blk[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)blk[i*4]<<24)|((uint32_t)blk[i*4+1]<<16)|((uint32_t)blk[i*4+2]<<8)|blk[i*4+3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ror32(w[i-15],7)^ror32(w[i-15],18)^(w[i-15]>>3);
        uint32_t s1 = ror32(w[i-2],17)^ror32(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=st[0],b=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = ror32(e,6)^ror32(e,11)^ror32(e,25);
        uint32_t ch = (e&f)^((~e)&g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ror32(a,2)^ror32(a,13)^ror32(a,22);
        uint32_t maj = (a&b)^(a&c)^(b&c);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    st[0]+=a; st[1]+=b; st[2]+=c; st[3]+=d; st[4]+=e; st[5]+=f; st[6]+=g; st[7]+=h;
}

static void bench_sha256(double *mib_s, uint64_t *csum) {
    const size_t SZ = 16 * 1024 * 1024;
    uint8_t *buf = (uint8_t *)malloc(SZ);
    for (size_t i = 0; i < SZ; ++i) buf[i] = (uint8_t)(i * 131 + 17);
    uint32_t st[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    double t0 = now_s();
    for (size_t off = 0; off < SZ; off += 64) sha256_block(st, buf + off);
    double dt = now_s() - t0;
    *mib_s = (double)SZ / dt / (1024.0 * 1024.0);
    *csum = ((uint64_t)st[0] << 32) | st[1];
    free(buf);
}

/* 7. Multi-thread Atomics & Memory Barriers */
static volatile uint64_t g_atomic_ctr = 0;
static void *atomic_worker(void *arg) {
    int iters = *(int *)arg;
    for (int i = 0; i < iters; ++i) __atomic_fetch_add(&g_atomic_ctr, 1, __ATOMIC_ACQ_REL);
    return NULL;
}

static void bench_atomic_mt(double *mops, int *valid) {
    g_atomic_ctr = 0;
    int iters = 2000000;
    pthread_t th[4];
    double t0 = now_s();
    for (int i = 0; i < 4; ++i) pthread_create(&th[i], NULL, atomic_worker, &iters);
    for (int i = 0; i < 4; ++i) pthread_join(th[i], NULL);
    double dt = now_s() - t0;
    *mops = (double)(4ULL * (uint64_t)iters) / dt / 1e6;
    *valid = (g_atomic_ctr == 4ULL * (uint64_t)iters);
}

/* 8. Multi-thread 4T compute */
typedef struct { uint64_t seed; uint64_t out; } MtCompArg;
static void *mt_comp_worker(void *arg) {
    MtCompArg *a = (MtCompArg *)arg;
    uint64_t x = a->seed;
    for (uint64_t i = 0; i < 25000000ULL; ++i)
        x = (x ^ (x >> 11)) * 6364136223846793005ULL + 1442695040888963407ULL;
    a->out = x;
    return NULL;
}

static void bench_mt4(double *gops, uint64_t *csum) {
    MtCompArg args[4];
    pthread_t th[4];
    for (int i = 0; i < 4; ++i) args[i].seed = 0x9e3779b97f4a7c15ULL * (uint64_t)(i + 1);
    double t0 = now_s();
    for (int i = 0; i < 4; ++i) pthread_create(&th[i], NULL, mt_comp_worker, &args[i]);
    for (int i = 0; i < 4; ++i) pthread_join(th[i], NULL);
    double dt = now_s() - t0;
    uint64_t h = 0;
    for (int i = 0; i < 4; ++i) h ^= args[i].out;
    *gops = (4.0 * 25000000.0 * 4.0) / dt / 1e9;
    *csum = h;
}

int main(void) {
    double int_gops, fp32_gf, fp64_gf, simd_gbs, mem_gibs, cmp_mibs, sha_mibs, atom_mops, mt4_gops;
    uint64_t c_int, c_fp32, c_fp64, c_simd, c_mem, c_sha, c_mt4;
    int ok_cmp = 0, ok_atom = 0;
    bench_int(&int_gops, &c_int);
    bench_fp32(&fp32_gf, &c_fp32);
    bench_fp64(&fp64_gf, &c_fp64);
    bench_simd(&simd_gbs, &c_simd);
    bench_memcpy(&mem_gibs, &c_mem);
    bench_compress(&cmp_mibs, &ok_cmp);
    bench_sha256(&sha_mibs, &c_sha);
    bench_atomic_mt(&atom_mops, &ok_atom);
    bench_mt4(&mt4_gops, &c_mt4);

    printf("{\n"
           "  \"arch\": \"%s\",\n"
           "  \"int_gops\": %.3f, \"int_csum\": \"%016" PRIx64 "\",\n"
           "  \"fp32_gflops\": %.3f, \"fp32_csum\": \"%" PRIu64 "\",\n"
           "  \"fp64_gflops\": %.3f, \"fp64_csum\": \"%" PRIu64 "\",\n"
           "  \"simd_gib_s\": %.3f, \"simd_csum\": \"%016" PRIx64 "\",\n"
           "  \"memcpy64m_gib_s\": %.3f, \"memcpy_csum\": \"%016" PRIx64 "\",\n"
           "  \"compress_mib_s\": %.3f, \"compress_ok\": %s,\n"
           "  \"sha256_mib_s\": %.3f, \"sha256_csum\": \"%016" PRIx64 "\",\n"
           "  \"atomic_mt4_mops\": %.3f, \"atomic_ok\": %s,\n"
           "  \"mt4_gops\": %.3f, \"mt4_csum\": \"%016" PRIx64 "\"\n"
           "}\n",
           ARCH_NAME,
           int_gops, c_int, fp32_gf, c_fp32, fp64_gf, c_fp64,
           simd_gbs, c_simd, mem_gibs, c_mem,
           cmp_mibs, ok_cmp ? "true" : "false",
           sha_mibs, c_sha, atom_mops, ok_atom ? "true" : "false",
           mt4_gops, c_mt4);
    return 0;
}
