/*
 * Checks src/fast/rndfast.h against the reference model (src/c64rnd.c).
 *
 * usage: test_fast [SEEDS [RANDOM_STATES [FILTER_LAST_SEED]]]
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/c64rnd.h"
#include "../src/fast/rndfast_neon.h"
#include "../src/fast/rndfast_block.h"

static long failures;

static c64float to_packed(fseed s)
{
    c64float f = { { (uint8_t)s.exp, (uint8_t)((s.mant >> 24) & 0x7F), (uint8_t)(s.mant >> 16),
                     (uint8_t)(s.mant >> 8), (uint8_t)s.mant } };
    return f;
}

static fseed from_fac(c64fac f)
{
    fseed s = { f.exp, f.mant };
    return s;
}

static int same(fseed a, fseed b)
{
    return a.exp == b.exp && a.mant == b.mant;
}

static void fail(const char *what, uint64_t id, fseed want, fseed got)
{
    if (++failures <= 20)
        fprintf(stderr, "MISMATCH %s %" PRIu64 ": ref %02X %08X, fast %02X %08X\n",
                what, id, want.exp, want.mant, got.exp, got.mant);
}

static fseed ref_rnd(fseed s)
{
    c64rnd r = { to_packed(s) };
    return from_fac(c64_rnd_positive(&r));
}

static void check_scale(fseed s, uint64_t id)
{
    static const uint32_t scales[] = { 1, 2, 3, 7, 22, 26, 100, 255 };
    c64fac fac = c64_unpack(to_packed(s));
    for (size_t i = 0; i < sizeof scales / sizeof scales[0]; i++) {
        uint32_t t = scales[i];
        if (fast_times_int(s, t) != c64_rnd_times(fac, t, C64_TO_INT_TRUNCATE)
            || fast_times_assign(s, t) != c64_rnd_times(fac, t, C64_TO_INT_ASSIGN)) {
            if (++failures <= 20)
                fprintf(stderr, "MISMATCH scale %u state %" PRIu64 " (%02X %08X)\n", t, id, s.exp, s.mant);
        }
    }
}

static uint64_t rng_state = 0x243F6A8885A308D3ull;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static uint8_t rand_byte(void)
{
    return (rng() % 3 == 0) ? 0 : (uint8_t)rng();
}

int main(int argc, char **argv)
{
    long seeds  = argc > 1 ? atol(argv[1]) : 2000000;
    long states = argc > 2 ? atol(argv[2]) : 20000000;
    uint64_t filter_last = argc > 3 ? strtoull(argv[3], NULL, 10) : 0xFFFFFFFFu;

    /* Real sequences: RND(-n) then 20 draws, n = 1..seeds and random n. */
    long slow = 0;
    for (long i = 0; i < 2 * seeds; i++) {
        uint32_t n = i < seeds ? (uint32_t)(i + 1) : (rng() | 1u) * (rng() % 7 + 1);
        if (n == 0)
            continue;
        c64rnd ref;
        fseed want = from_fac(c64_rnd_reseed(&ref, n));
        fseed got = fast_reseed(n);
        if (!same(want, got)) {
            fail("reseed", n, want, got);
            continue;
        }
        for (int d = 0; d < 20; d++) {
            want = from_fac(c64_rnd_positive(&ref));
            slow += (got.mant & 0x00FFFF00u) == 0 && (got.mant & 0xFF) != 0;
            got = fast_rnd(got);
            if (!same(want, got)) {
                fail("sequence for seed", n, want, got);
                break;
            }
            check_scale(got, n);
        }
    }
    printf("sequences: %ld seeds x 20 draws (%ld hit the FMULT bug)\n", 2 * seeds, slow);

    /* Arbitrary seed states, including unreachable ones, biased to zero bytes. */
    long bug = 0;
    for (long i = 0; i < states; i++) {
        fseed s;
        s.exp = 0x61 + rng() % 0x21;
        s.mant = 0x80000000u | (uint32_t)rand_byte() << 24 | (uint32_t)rand_byte() << 16
               | (uint32_t)rand_byte() << 8 | rand_byte();
        fseed want = ref_rnd(s);
        fseed got = fast_rnd(s);
        bug += (s.mant & 0x00FFFF00u) == 0 && (s.mant & 0xFF) != 0;
        if (!same(want, got))
            fail("state", (uint64_t)i, want, got);
        check_scale(s, (uint64_t)i);
    }
    printf("states: %ld random seed states (%ld hit the FMULT bug)\n", states, bug);

    /* The first-letter filter must pass every seed whose first value is the
       wanted one (it may also pass others). */
    static const uint32_t fscales[] = { 22, 3, 26, 100, 255 };
    for (size_t si = 0; si < sizeof fscales / sizeof fscales[0]; si++) {
        uint32_t t = fscales[si];
        ffilter f[257];
        for (uint32_t w = 0; w <= t; w++)   /* RND can be exactly 1.0, giving T */
            f[w] = fast_filter_init(t, w);
        uint64_t passed = 0, checked = 0;
        uint64_t step = t == 22 ? 1 : 97;    /* every seed for T=22, a sample otherwise */
        for (uint64_t n = 1; n <= filter_last; n += step) {
            fseed s0 = fast_reseed((uint32_t)n);
            fseed r = fast_rnd(s0);
            uint32_t k[2] = { fast_times_int(r, t), fast_times_assign(r, t) };
            for (int mode = 0; mode < 2; mode++) {
                if (!fast_filter_pass(&f[k[mode]], s0) && ++failures <= 20)
                    fprintf(stderr, "MISMATCH filter rejects seed %llu (T=%u, value %u, mode %d)\n",
                            (unsigned long long)n, t, k[mode], mode);
            }
            passed += fast_filter_pass(&f[t / 2], s0);
            checked++;
        }
        printf("filter T=%u: sound on %llu seeds; passes %.2f%% for value %u\n", t,
               (unsigned long long)checked, 100.0 * (double)passed / (double)checked, t / 2);
    }

    /* The block filter must produce exactly the seeds the scalar filter passes. */
    {
        static blocktab bt;
        static const uint32_t wants[] = { 11, 1, 21, 22 };
        for (size_t wi = 0; wi < sizeof wants / sizeof wants[0]; wi++) {
            block_init(&bt, fast_filter_init(22, wants[wi]));
            uint64_t emitted = 0, expected = 0;
            for (unsigned bitlen = 1; bitlen <= 32; bitlen++) {
                unsigned k = block_bits(bitlen);
                uint64_t lo = 1ull << (bitlen - 1), hi = 1ull << bitlen;
                if (lo > filter_last)
                    break;
                if (hi > filter_last + 1)
                    hi = (filter_last + 1) & ~((1ull << k) - 1);
                for (uint64_t base = lo; base < hi; base += 1ull << k) {
                    uint32_t got_n[256];
                    fseed got_s[256];
                    unsigned ng = 0;
#define COLLECT(N, S) do { got_n[ng] = (N); got_s[ng] = (S); ng++; } while (0)
                    BLOCK_FOR_EACH_CANDIDATE(&bt, (uint32_t)base, k, COLLECT);
#undef COLLECT
                    emitted += ng;
                    /* sort the (few) emitted seeds, then walk the block */
                    for (unsigned a = 1; a < ng; a++)
                        for (unsigned b = a; b > 0 && got_n[b - 1] > got_n[b]; b--) {
                            uint32_t tn = got_n[b]; got_n[b] = got_n[b - 1]; got_n[b - 1] = tn;
                            fseed ts = got_s[b]; got_s[b] = got_s[b - 1]; got_s[b - 1] = ts;
                        }
                    unsigned gi = 0;
                    for (uint64_t n = base; n < base + (1ull << k); n++) {
                        fseed s0 = fast_reseed((uint32_t)n);
                        int want = fast_filter_pass(&bt.f, s0);
                        int got = gi < ng && got_n[gi] == n;
                        expected += want;
                        if (want != got || (got && !same(s0, got_s[gi]))) {
                            if (++failures <= 20)
                                fprintf(stderr, "MISMATCH block filter (want %u) seed %llu: scalar %d block %d\n",
                                        wants[wi], (unsigned long long)n, want, got);
                        }
                        gi += got;
                    }
                }
            }
            printf("block filter (value %u): %llu candidates, same as the scalar filter's %llu\n",
                   wants[wi], (unsigned long long)emitted, (unsigned long long)expected);
        }
    }

#if defined(__ARM_NEON)
    /* The NEON filter must agree with the scalar one exactly. */
    {
        ffilter f = fast_filter_init(22, 11);
        uint32x4_t b_lo = vdupq_n_u32(f.b_lo), b_span = vdupq_n_u32(f.b_span);
        uint64_t checked = 0;
        for (uint64_t n = 1; n + 16 <= filter_last + 1; n += 16) {
            uint64_t mask = filter16((uint32_t)n, b_lo, b_span);
            for (int k = 0; k < 16; k++) {
                int want = fast_filter_pass(&f, fast_reseed((uint32_t)n + (uint32_t)k));
                if (want != (int)((mask >> (4 * k)) & 1) && ++failures <= 20)
                    fprintf(stderr, "MISMATCH NEON filter, seed %llu\n", (unsigned long long)n + (unsigned long long)k);
            }
            checked += 16;
        }
        printf("NEON filter agrees with the scalar filter on %llu seeds\n", (unsigned long long)checked);
    }
#endif

    if (failures) {
        printf("FAILED: %ld mismatches\n", failures);
        return 1;
    }
    printf("all OK\n");
    return 0;
}
