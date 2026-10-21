/*
 * NEON (AArch64 Advanced SIMD) version of fast_reseed + fast_filter_pass,
 * 16 seeds at a time.  Checked against the scalar filter by test/test_fast.c.
 */
#ifndef RNDFAST_NEON_H
#define RNDFAST_NEON_H

#include "rndfast.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>

/* fast_reseed + fast_filter_pass for the 4 seeds in n, in NEON lanes.
   Returns all-ones in the lanes that pass. */
static inline uint32x4_t filter4(uint32x4_t n, uint32x4_t b_lo, uint32x4_t b_span)
{
    /* reseed: M = n << clz(n), E = $80 + bits(n) */
    uint32x4_t lz = vclzq_u32(n);
    uint32x4_t M = vshlq_u32(n, vreinterpretq_s32_u32(lz));
    uint32x4_t E = vsubq_u32(vdupq_n_u32(0xA0), lz);
    /* scramble: m = bswap(M), normalise (m:E) by z = clz(m), round */
    uint32x4_t m = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(M)));
    uint32x4_t z = vclzq_u32(m);
    int32x4_t zs = vreinterpretq_s32_u32(z);
    /* (E << (z - 8)) is a right shift for z < 8: NEON's signed shift counts */
    uint32x4_t mant = vorrq_u32(vshlq_u32(m, zs), vshlq_u32(E, vsubq_s32(zs, vdupq_n_s32(8))));
    uint32x4_t rb = vshrq_n_u32(vandq_u32(vshlq_u32(E, zs), vdupq_n_u32(0x80)), 7);
    mant = vaddq_u32(mant, rb);
    uint32x4_t ovf = vandq_u32(vceqzq_u32(mant), vtstq_u32(rb, rb));   /* carried out of the top */
    mant = vorrq_u32(mant, vandq_u32(ovf, vdupq_n_u32(0x80000000u)));
    uint32x4_t exp = vsubq_u32(vsubq_u32(vdupq_n_u32(0x80), z), ovf);   /* ovf is -1 */

    /* filter */
    uint32x4_t lo = vmulq_u32(mant, vdupq_n_u32(FAST_RND_MULT));
    uint32x4_t b = vbslq_u32(vcgeq_u32(mant, vdupq_n_u32(FAST_NORM_LIMIT)),
                             vshrq_n_u32(lo, 24),
                             vandq_u32(vshrq_n_u32(lo, 23), vdupq_n_u32(0xFF)));
    uint32x4_t carry = vshrq_n_u32(vcleq_u32(exp, vdupq_n_u32(0x78)), 31);   /* 1 where b may be 1 low */
    uint32x4_t d = vandq_u32(vaddq_u32(vsubq_u32(b, b_lo), carry), vdupq_n_u32(0xFF));
    uint32x4_t pass = vcleq_u32(d, vaddq_u32(b_span, carry));
    pass = vorrq_u32(pass, vcleq_u32(exp, vdupq_n_u32(0x70)));
    pass = vorrq_u32(pass, vceqq_u32(mant, vdupq_n_u32(FAST_NORM_LIMIT - 1)));
    uint32x4_t quirk = vandq_u32(vceqzq_u32(vandq_u32(mant, vdupq_n_u32(0x00FFFF00u))),
                                 vtstq_u32(mant, vdupq_n_u32(0xFF)));
    return vorrq_u32(pass, quirk);
}

/* Filters seeds n .. n+15; returns 4 bits per seed (0xF = passes). */
static inline uint64_t filter16(uint32_t n, uint32x4_t b_lo, uint32x4_t b_span)
{
    static const uint32_t lane[4] = { 0, 1, 2, 3 };
    uint32x4_t v = vaddq_u32(vdupq_n_u32(n), vld1q_u32(lane));
    uint32x4_t p0 = filter4(v, b_lo, b_span);
    uint32x4_t p1 = filter4(vaddq_u32(v, vdupq_n_u32(4)), b_lo, b_span);
    uint32x4_t p2 = filter4(vaddq_u32(v, vdupq_n_u32(8)), b_lo, b_span);
    uint32x4_t p3 = filter4(vaddq_u32(v, vdupq_n_u32(12)), b_lo, b_span);
    uint16x8_t h0 = vcombine_u16(vmovn_u32(p0), vmovn_u32(p1));
    uint16x8_t h1 = vcombine_u16(vmovn_u32(p2), vmovn_u32(p3));
    uint8x16_t bytes = vcombine_u8(vmovn_u16(h0), vmovn_u16(h1));
    return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(bytes), 4)), 0);
}
#endif

#endif
