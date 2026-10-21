/*
 * fastsearch: seedsearch, but fast.  Same arguments and output, plus -j.
 *
 * usage: fastsearch [--assign] [--scale T] [--offset S] [-j THREADS] [--per-seed] WORD [FIRST [LAST]]
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

#include "rndfast.h"
#include "rndfast_block.h"
#include "rndfast_neon.h"

#define CHUNK (1u << 20)

/* Blocks of fewer seeds than 2^block_min_bits use the per-seed filter. */
static unsigned block_min_bits = 4;

typedef struct {
    /* search */
    uint8_t  target[256];    /* wanted RND*T values, ending with 0 */
    size_t   len;            /* letters in the word (target[len] == 0) */
    uint32_t scale;
    int      assign;
    ffilter  filter;         /* cheap test for target[0] */
    blocktab blocks;         /* the same test for blocks of seeds */
    int      method;         /* 0 = block filter, 1 = NEON per-seed filter */
    uint64_t first, last;

    /* work distribution */
    _Atomic uint64_t next;

    /* results */
    pthread_mutex_t lock;
    uint32_t *found;
    size_t    nfound, capfound;
} job;

static void add_result(job *j, uint32_t n)
{
    pthread_mutex_lock(&j->lock);
    if (j->nfound == j->capfound) {
        j->capfound = j->capfound ? 2 * j->capfound : 1024;
        j->found = realloc(j->found, j->capfound * sizeof *j->found);
        if (!j->found) { perror("realloc"); exit(2); }
    }
    j->found[j->nfound++] = n;
    pthread_mutex_unlock(&j->lock);
}


#define SCAN(NAME, TIMES)                                                        \
    static inline int NAME##_spells_from(const uint8_t *t, uint32_t scale, fseed s) \
    {                                                                            \
        for (size_t i = 0;; i++) {                                               \
            s = fast_rnd(s);                                                     \
            uint32_t v = TIMES(s, scale);                                        \
            if (v != t[i])                                                       \
                return 0;                                                        \
            if (v == 0)                                                          \
                return 1;                                                        \
        }                                                                        \
    }                                                                            \
    static inline int NAME##_spells(const uint8_t *t, uint32_t scale, uint32_t n) \
    {                                                                            \
        return NAME##_spells_from(t, scale, fast_reseed(n));                     \
    }                                                                            \
    static void NAME(job *j, uint64_t lo, uint64_t hi);                          \
    /* Whole blocks via the block filter, the rest one seed at a time. */        \
    static void NAME##_blocks(job *j, uint64_t lo, uint64_t hi)                  \
    {                                                                            \
        const uint8_t *t = j->target;                                            \
        const uint32_t scale = j->scale;                                         \
        uint64_t n = lo;                                                         \
        while (n < hi) {                                                         \
            unsigned bitlen = 64 - (unsigned)__builtin_clzll(n);                 \
            unsigned k = block_bits(bitlen);                                     \
            uint64_t size = 1ull << k;                                           \
            if (k < block_min_bits) {                                            \
                /* small blocks don't pay: per-seed filter to the next bit length */ \
                uint64_t end = 1ull << bitlen;                                   \
                NAME(j, n, end < hi ? end : hi);                                 \
                n = end < hi ? end : hi;                                         \
            } else if ((n & (size - 1)) == 0 && n + size <= hi) {                \
                _Pragma("clang diagnostic push")                                 \
                _Pragma("clang diagnostic ignored \"-Wshadow\"")               \
                BLOCK_FOR_EACH_CANDIDATE(&j->blocks, (uint32_t)n, k, NAME##_EMIT); \
                _Pragma("clang diagnostic pop")                                  \
                n += size;                                                       \
            } else {                                                             \
                fseed s0 = fast_reseed((uint32_t)n);                             \
                if (fast_filter_pass(&j->filter, s0)                             \
                    && NAME##_spells_from(t, scale, s0))                         \
                    add_result(j, (uint32_t)n);                                  \
                n++;                                                             \
            }                                                                    \
        }                                                                        \
    }                                                                            \
    static void NAME(job *j, uint64_t lo, uint64_t hi)                           \
    {                                                                            \
        const uint8_t *t = j->target;                                            \
        const uint32_t scale = j->scale;                                         \
        const ffilter f = j->filter;                                             \
        uint64_t n = lo;                                                         \
        NEON_LOOP(NAME)                                                          \
        for (; n < hi; n++) {                                                    \
            if (fast_filter_pass(&f, fast_reseed((uint32_t)n))                   \
                && NAME##_spells(t, scale, (uint32_t)n))                         \
                add_result(j, (uint32_t)n);                                      \
        }                                                                        \
    }

#if defined(__ARM_NEON)
#define NEON_LOOP(NAME)                                                          \
        {                                                                        \
            uint32x4_t b_lo = vdupq_n_u32(f.b_lo), b_span = vdupq_n_u32(f.b_span); \
            for (; n + 16 <= hi; n += 16) {                                      \
                uint64_t mask = filter16((uint32_t)n, b_lo, b_span);             \
                while (mask) {                                                   \
                    uint32_t c = (uint32_t)n + (uint32_t)(__builtin_ctzll(mask) >> 2); \
                    mask &= ~(0xFull << (__builtin_ctzll(mask) & ~3));           \
                    if (NAME##_spells(t, scale, c))                              \
                        add_result(j, c);                                        \
                }                                                                \
            }                                                                    \
        }
#else
#define NEON_LOOP(NAME)
#endif

#define scan_int_EMIT(N, S)    do { if (scan_int_spells_from(t, scale, (S))) add_result(j, (N)); } while (0)
#define scan_assign_EMIT(N, S) do { if (scan_assign_spells_from(t, scale, (S))) add_result(j, (N)); } while (0)
SCAN(scan_int, fast_times_int)
SCAN(scan_assign, fast_times_assign)

static void *worker(void *arg)
{
    job *j = arg;
    for (;;) {
        /* Chunks are aligned to CHUNK, so blocks never straddle two of them. */
        uint64_t c = atomic_fetch_add(&j->next, 1);
        uint64_t lo = c * CHUNK, hi = lo + CHUNK;
        if (lo > j->last)
            break;
        if (lo < j->first)
            lo = j->first;
        if (hi > j->last + 1)
            hi = j->last + 1;
        if (j->method == 0)
            j->assign ? scan_assign_blocks(j, lo, hi) : scan_int_blocks(j, lo, hi);
        else
            j->assign ? scan_assign(j, lo, hi) : scan_int(j, lo, hi);
    }
    return NULL;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [--assign] [--scale T] [--offset S] [-j THREADS] [--per-seed] WORD [FIRST [LAST]]\n"
        "  Same as seedsearch; FIRST and LAST may go up to 4294967295.\n"
        "  -j THREADS   default: all cores\n"
        "  --per-seed   filter seed by seed (NEON) instead of by blocks\n", prog);
    exit(2);
}

static uint64_t parse_number(const char *s, uint64_t max, const char *prog)
{
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v > max)
        usage(prog);
    return v;
}

int main(int argc, char **argv)
{
    const char *prog = argv[0];
    static job j;
    uint64_t scale = 22, offset = 64, threads = 0;

    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "--assign") == 0)
            j.assign = 1;
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
            scale = parse_number(argv[++i], 255, prog);
        else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc)
            offset = parse_number(argv[++i], 255, prog);
        else if (strcmp(argv[i], "--per-seed") == 0)
            j.method = 1;
        else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc)
            threads = parse_number(argv[++i], 1024, prog);
        else
            usage(prog);
    }
    if (i >= argc || argc - i > 3 || scale == 0)
        usage(prog);

    const char *word = argv[i];
    j.len = strlen(word);
    if (j.len == 0 || j.len >= sizeof j.target)
        usage(prog);
    char upper[256];
    for (size_t k = 0; k <= j.len; k++) {
        char ch = word[k];
        upper[k] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
        if (k < j.len) {
            int v = (unsigned char)upper[k] - (int)offset;
            if (v <= 0 || v > (int)scale) {   /* v == scale needs RND = 1.0, which happens */
                fprintf(stderr, "%s: '%c' can never be produced with --scale %llu --offset %llu\n",
                        prog, ch, (unsigned long long)scale, (unsigned long long)offset);
                return 1;
            }
            j.target[k] = (uint8_t)v;
        }
    }
    j.target[j.len] = 0;
    j.scale = (uint32_t)scale;
    j.filter = fast_filter_init(j.scale, j.target[0]);
    block_init(&j.blocks, j.filter);
    j.first = argc - i > 1 ? parse_number(argv[i + 1], 0xFFFFFFFFu, prog) : 1;
    j.last  = argc - i > 2 ? parse_number(argv[i + 2], 0xFFFFFFFFu, prog) : 99999999;
    if (j.first == 0)
        j.first = 1;    /* RND(-0) is RND(0), which reads the clock */
    atomic_store(&j.next, j.first / CHUNK);
    pthread_mutex_init(&j.lock, NULL);

    if (threads == 0) {
        int ncpu = 0;
        size_t sz = sizeof ncpu;
        if (sysctlbyname("hw.ncpu", &ncpu, &sz, NULL, 0) != 0 || ncpu < 1)
            ncpu = 1;
        threads = (uint64_t)ncpu;
    }

    if (getenv("BLOCK_MIN_BITS"))
        block_min_bits = (unsigned)atoi(getenv("BLOCK_MIN_BITS"));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_t tid[1024];
    for (uint64_t k = 0; k < threads; k++)
        pthread_create(&tid[k], NULL, worker, &j);
    for (uint64_t k = 0; k < threads; k++)
        pthread_join(tid[k], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    qsort(j.found, j.nfound, sizeof *j.found, cmp_u32);
    for (size_t k = 0; k < j.nfound; k++)
        printf("%u %s\n", j.found[k], upper);

    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    double seeds = (double)(j.last - j.first + 1);
    fprintf(stderr, "%.0f seeds in %.3f s: %.2f Gseeds/s (%llu threads)\n",
            seeds, secs, seeds / secs / 1e9, (unsigned long long)threads);
    return 0;
}
