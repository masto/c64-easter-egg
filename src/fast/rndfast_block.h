/*
 * The first-letter filter for a whole block of seeds at once.
 *
 * RND(-n) byte-reverses n's mantissa, so n's lowest bits end up at the top of
 * the seed mantissa.  Take a block of seeds that differ only in their low k
 * bits v (k <= 8, and all with the same bit length, so the same exponent byte
 * E).  For the v with bit length j (class j: v in [2^(j-1), 2^j)) the
 * normalise shift is the same, and v's bits land exactly in the top j bits
 * of the seed mantissa:
 *
 *     mant(v) = (v << (32 - j)) + C_j
 *
 * where C_j (the other bits plus the rounding increment) is the same for the
 * whole class, unless rounding carries into v's bits, which is checked for.
 * The filter byte (see fast_filter_pass) is then
 *
 *     b = (C_j * K >> 24) + v * (K << (8 - j))        (mod 256), or
 *     b = (C_j * K >> 23) + v * (K << (9 - j))        (mod 256)
 *
 * depending on whether mant(v) >= FAST_NORM_LIMIT, which holds for v above a
 * threshold.  So for each class and each value of the first term, the v that
 * pass are a fixed set: precomputed bitmasks.  The exponent, and with it the
 * "addend may carry" and "pass unfiltered" decisions, is also per class, and
 * so is FMULT's bug, because it depends only on the low 24 bits of mant.
 *
 * The result is exactly the set of seeds fast_filter_pass passes (checked by
 * test/test_fast.c), plus each candidate's seed state.
 */
#ifndef RNDFAST_BLOCK_H
#define RNDFAST_BLOCK_H

#include <string.h>

#include "rndfast.h"

typedef struct {
    uint64_t m[2];                  /* bit i: v = 2^(j-1) + i passes */
} vmask;

typedef struct {
    /* [j][carry][first term] for the two alignments */
    vmask a[9][2][256];
    vmask b[9][2][256];
    ffilter f;
} blocktab;

static inline void mask_set(vmask *m, unsigned i) { m->m[i >> 6] |= 1ull << (i & 63); }

static void block_init(blocktab *t, ffilter f)
{
    memset(t, 0, sizeof *t);
    t->f = f;
    for (unsigned j = 1; j <= 8; j++) {
        uint32_t mul_a = (FAST_RND_MULT << (8 - j)) & 0xFF;
        uint32_t mul_b = (FAST_RND_MULT << (9 - j)) & 0xFF;
        for (unsigned carry = 0; carry < 2; carry++) {
            for (uint32_t c = 0; c < 256; c++) {
                for (uint32_t v = 1u << (j - 1); v < 1u << j; v++) {
                    uint32_t ba = (c + v * mul_a) & 0xFF, bb = (c + v * mul_b) & 0xFF;
                    if (((ba - f.b_lo + carry) & 0xFF) <= f.b_span + carry)
                        mask_set(&t->a[j][carry][c], v - (1u << (j - 1)));
                    if (((bb - f.b_lo + carry) & 0xFF) <= f.b_span + carry)
                        mask_set(&t->b[j][carry][c], v - (1u << (j - 1)));
                }
            }
        }
    }
}

/* Bits [lo, 128) of a 128-bit mask. */
static inline vmask mask_from(unsigned lo)
{
    vmask m;
    m.m[0] = lo >= 64 ? 0 : ~0ull << lo;
    m.m[1] = lo >= 128 ? 0 : lo <= 64 ? ~0ull : ~0ull << (lo - 64);
    return m;
}

/* How many low bits of seeds with this bit length can form a block: the
   bits of n in the lowest non-zero byte of its mantissa.  Below 256 that byte
   also holds n's leading 1, above v's bits, so no blocking there. */
static inline unsigned block_bits(unsigned bitlen)
{
    return bitlen <= 8 ? 0 : 8 - ((32 - bitlen) & 7);
}

/*
 * Calls emit(ctx, n, s0) for every seed n in the block base .. base + 2^k - 1
 * that passes fast_filter_pass, with s0 = fast_reseed(n).  base must be a
 * multiple of 2^k, and every seed in the block must have the same bit length.
 */
#define BLOCK_FOR_EACH_CANDIDATE(T, BASE, K, EMIT)                                       \
    do {                                                                                 \
        const blocktab *bt_ = (T);                                                       \
        uint32_t base_ = (BASE);                                                         \
        unsigned k_ = (K);                                                               \
        {   /* v = 0: the next byte up is at the top; just test it */                    \
            fseed s_ = fast_reseed(base_);                                               \
            if (fast_filter_pass(&bt_->f, s_))                                           \
                EMIT(base_, s_);                                                         \
        }                                                                                \
        for (unsigned j_ = 1; j_ <= k_; j_++) {                                          \
            uint32_t vmin_ = 1u << (j_ - 1), nv_ = vmin_;                                \
            fseed s_ = fast_reseed(base_ | vmin_);                                       \
            uint32_t c_ = s_.mant - 0x80000000u;                                         \
            unsigned low_ = 32 - j_;                                                     \
            int pass_all_ = s_.exp <= 0x70                                               \
                || ((c_ & 0x00FFFF00u) == 0 && (c_ & 0xFF) != 0);                        \
            if (c_ >> low_) {                                                            \
                /* rounding carried into v's bits: not linear, test each seed */         \
                for (uint32_t v_ = vmin_; v_ < 2 * vmin_; v_++) {                        \
                    fseed t_ = fast_reseed(base_ | v_);                                  \
                    if (fast_filter_pass(&bt_->f, t_))                                   \
                        EMIT(base_ | v_, t_);                                            \
                }                                                                        \
                continue;                                                                \
            }                                                                            \
            vmask m_;                                                                    \
            if (pass_all_) {                                                             \
                m_ = mask_from(0);                                                       \
            } else {                                                                     \
                unsigned carry_ = s_.exp <= 0x78;                                        \
                uint32_t lo_ = c_ * FAST_RND_MULT;                                       \
                const vmask *ma_ = &bt_->a[j_][carry_][lo_ >> 24];                       \
                const vmask *mb_ = &bt_->b[j_][carry_][(lo_ >> 23) & 0xFF];              \
                /* v >= vt: mant(v) >= FAST_NORM_LIMIT */                                \
                uint32_t vt_ = FAST_NORM_LIMIT >> low_;                                  \
                if (c_ < (FAST_NORM_LIMIT & ((1u << low_) - 1)))                         \
                    vt_++;                                                               \
                vmask ge_ = mask_from(vt_ < vmin_ ? 0 : vt_ - vmin_);                    \
                m_.m[0] = (ma_->m[0] & ge_.m[0]) | (mb_->m[0] & ~ge_.m[0]);              \
                m_.m[1] = (ma_->m[1] & ge_.m[1]) | (mb_->m[1] & ~ge_.m[1]);              \
                /* the one mantissa whose product could carry out of the top */          \
                uint32_t lim_ = FAST_NORM_LIMIT - 1;                                     \
                if ((lim_ >> low_) >= vmin_ && (lim_ >> low_) < 2 * vmin_                \
                    && c_ == (lim_ & ((1u << low_) - 1)))                                \
                    mask_set(&m_, (lim_ >> low_) - vmin_);                               \
            }                                                                            \
            for (int w_ = 0; w_ < 2; w_++) {                                             \
                uint64_t bits_ = m_.m[w_];                                               \
                if (nv_ < 64 * (unsigned)(w_ + 1))                                       \
                    bits_ &= nv_ - 64 * (unsigned)w_ >= 64 ? ~0ull                       \
                           : (1ull << (nv_ - 64 * (unsigned)w_)) - 1;                    \
                if (nv_ <= 64 * (unsigned)w_)                                            \
                    bits_ = 0;                                                           \
                while (bits_) {                                                          \
                    uint32_t v_ = vmin_ + 64 * (uint32_t)w_                              \
                                + (uint32_t)__builtin_ctzll(bits_);                      \
                    bits_ &= bits_ - 1;                                                  \
                    fseed t_ = { s_.exp, (v_ << low_) + c_ };                            \
                    EMIT(base_ | v_, t_);                                                \
                }                                                                        \
            }                                                                            \
        }                                                                                \
    } while (0)

#endif
