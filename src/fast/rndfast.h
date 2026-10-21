/*
 * RND() reduced to a handful of integer operations.
 *
 * Derived from the bit-exact model in ../c64float.c by specialising each ROM
 * routine to the operands RND actually feeds it (see the derivations inline),
 * and checked against that model by test/test_fast.c.
 *
 * A seed is the packed RNDX value: exponent byte and a 32-bit mantissa with
 * the top bit set (the sign bit is always 0).
 */
#ifndef RNDFAST_H
#define RNDFAST_H

#include <stdint.h>

typedef struct {
    uint32_t exp;
    uint32_t mant;
} fseed;

#define FAST_RND_MULT   0xB5447Au              /* 11879546's mantissa, without its zero low byte */
#define FAST_RND_ADD40  (0xA8B14600ull << 8)   /* 3.927677739E-8's mantissa as a 40-bit window */
#define FAST_RND_ADDEXP 0x68u

/*
 * The scramble at $E0D3 applied to (mantissa M, exponent E): byte-reverse M,
 * put E in the rounding byte, exponent $80, normalise, round.  M has its top
 * bit set, so the reversed mantissa is never 0 and normalising shifts by
 * z = clz(reversed) < 32 bits through the 40-bit (mantissa:rounding byte) window.
 */
static inline fseed fast_scramble(uint32_t M, uint32_t E)
{
    uint32_t m = __builtin_bswap32(M);
    unsigned z = (unsigned)__builtin_clz(m);
    uint64_t v = ((uint64_t)m << 8 | (E & 0xFF)) << z;   /* bit 39 is now set */
    fseed s;
    s.mant = (uint32_t)(v >> 8);
    s.exp = 0x80 - z;
    if (v & 0x80) {                                      /* ROUND */
        if (++s.mant == 0) {
            s.mant = 0x80000000u;
            s.exp++;
        }
    }
    return s;
}

/* RND(-n) for a whole number n >= 1: n as a float is exponent $80+bits(n)
   and mantissa n shifted up to bit 31.  The sign plays no part. */
static inline fseed fast_reseed(uint32_t n)
{
    unsigned lz = (unsigned)__builtin_clz(n);
    return fast_scramble(n << lz, 0x80 + 32 - lz);
}

/*
 * FMULT's 40-bit window for seed mantissa * 11879546 (see fast_rnd).
 *
 * Normally this is floor(full product / 2^32): shift-and-add with a 40-bit
 * window that drops its low bits is a nest of floor-divisions by 2, which
 * compose.  With the multiplier's rounding byte 0 and the multiplicand's low
 * byte 0 that is (mant * $B5447A) >> 16.
 *
 * The exception is FMULT's 9-bit-shift bug: a zero multiplier byte following
 * another zero byte, with something already in the product, shifts the
 * product 9 bits instead of 8.  The multiplier bytes are the rounding byte (0)
 * and then the mantissa from low to high, so that only happens for
 * mantissa = yy 00 00 xx with xx != 0.  Replayed byte by byte:
 *   xx:  window = xx * M                (M = $B5447A00, added at bit 8)
 *   00:  shift 8 bits
 *   00:  shift 9 bits; the rounding byte only gets the first 8
 *   yy:  window = (window + yy * M << 8) >> 8
 */
static inline uint64_t fast_product40(uint32_t mant)
{
    if (__builtin_expect((mant & 0x00FFFF00u) != 0 || (mant & 0xFF) == 0, 1))
        return ((uint64_t)mant * FAST_RND_MULT) >> 16;
    uint64_t acc = ((uint64_t)(mant & 0xFF) * (FAST_RND_MULT << 8)) >> 16;
    uint64_t w = (acc >> 9) << 8 | (acc & 0xFF);
    return (w + (uint64_t)(mant >> 24) * ((uint64_t)FAST_RND_MULT << 16)) >> 8;
}

/*
 * RND(1).
 *
 * FMULT: see fast_product40.  Both mantissas are >= 2^31, so the window's
 * top bit is bit 39 or bit 38:
 * normalising shifts by at most 1.  The exponent comes out as s.exp + $18
 * minus that shift, whichever way MULDIV's carry went.
 *
 * FADD: the product's exponent is always above $68, so the constant is the
 * operand shifted right (by 16..49 bits) and added into the 40-bit window;
 * a carry out of the top shifts right one.  The rounding byte of the result
 * is then discarded, because the scramble overwrites it with the exponent.
 */
static inline fseed fast_rnd(fseed s)
{
    uint64_t w = fast_product40(s.mant);
    uint32_t e = s.exp + 0x18;
    if (!(w >> 39)) {
        w <<= 1;
        e--;
    }
    unsigned sh = e - FAST_RND_ADDEXP;
    if (sh < 40) {
        w += FAST_RND_ADD40 >> sh;
        if (w >> 40) {
            w >>= 1;
            e++;
        }
    }
    return fast_scramble((uint32_t)(w >> 8), e);
}

/*
 * INT(RND * T) for 1 <= T <= 255.  T in FAC1 has a single non-zero mantissa
 * byte, so FMULT's product T * mant is exact in its 40-bit window and INT
 * just truncates it.  The value is T * mant * 2^(exp - $80 - 32).
 */
static inline uint32_t fast_times_int(fseed r, uint32_t t)
{
    unsigned sh = 0xA0 - r.exp;                          /* 31..63 */
    return (uint32_t)(((uint64_t)t * r.mant) >> sh);
}

/* A% = RND * T: the same product, first rounded half-up to 32 significant bits. */
static inline uint32_t fast_times_assign(fseed r, uint32_t t)
{
    uint64_t p = (uint64_t)t * r.mant;
    unsigned bits = 64 - (unsigned)__builtin_clzll(p);
    if (bits > 32) {
        unsigned d = bits - 32;
        p = ((p >> (d - 1)) + 1) >> 1 << d;
    }
    unsigned sh = 0xA0 - r.exp;
    return (uint32_t)(p >> sh);
}

/*
 * A cheap, conservative test of the first letter after a reseed.
 *
 * The first RND value is the scrambled (byte-reversed) product mantissa, so
 * its top byte b is the product mantissa's low byte: bits 24..31 of
 * mant * $B5447A, or bits 23..30 when the product needed its 1-bit normalise
 * shift, which happens exactly when mant < ceil(2^55 / $B5447A).  Only the
 * low 32 bits of the multiply are needed.  Then RND is in [b/256, (b+1)/256]
 * (rounding cannot carry past (b+1)/256), so INT or A% of RND*T is in
 * [floor(T*b/256), floor(T*(b+1)/256)].  Those bounds rise with b, so the b
 * that can give a wanted value form one range, precomputed by fast_filter_init.
 *
 * The 3.9E-8 addend, shifted right by exp - $50 - (normalise shift), is 0
 * for seed exponents >= $79 and below 2^8 for exponents $71..$78.  An addend
 * below 2^8 can only carry 1 into b, so for those b-1 also passes.  Seeds
 * with smaller exponents, that hit FMULT's bug, or with the one mantissa
 * (FAST_NORM_LIMIT - 1) whose product mantissa is all ones, so that the add
 * could carry out of the top, pass unfiltered.
 */
#define FAST_NORM_LIMIT 0xB4C582ACu   /* ceil(2^55 / $B5447A) */

typedef struct {
    uint32_t b_lo, b_span;            /* pass if b - b_lo <= b_span (unsigned) */
} ffilter;

static inline ffilter fast_filter_init(uint32_t t, uint32_t want)
{
    ffilter f = { 0, 0 };
    int lo = -1, hi = -1;
    for (uint32_t b = 0; b < 256; b++) {
        if (t * b / 256 <= want && want <= t * (b + 1) / 256) {
            if (lo < 0)
                lo = (int)b;
            hi = (int)b;
        }
    }
    if (lo < 0) {                     /* impossible: pass nothing */
        f.b_lo = 0x100;
        f.b_span = 0;
    } else {
        f.b_lo = (uint32_t)lo;
        f.b_span = (uint32_t)(hi - lo);
    }
    return f;
}

static inline int fast_filter_pass(const ffilter *f, fseed s0)
{
    uint32_t lo = s0.mant * FAST_RND_MULT;
    uint32_t b = s0.mant >= FAST_NORM_LIMIT ? lo >> 24 : (lo >> 23) & 0xFF;
    int carry = s0.exp <= 0x78;                       /* the addend may carry 1 into b */
    int exact_needed = s0.exp <= 0x70 || s0.mant == FAST_NORM_LIMIT - 1
                    || ((s0.mant & 0x00FFFF00u) == 0 && (s0.mant & 0xFF) != 0);
    return ((b - f->b_lo + (uint32_t)carry) & 0xFF) <= f->b_span + (uint32_t)carry || exact_needed;
}

#endif
