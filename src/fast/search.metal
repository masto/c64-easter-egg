// GPU version of fastsearch's scan: rndfast.h rewritten in 32-bit operations.
#include <metal_stdlib>
using namespace metal;

struct Seed { uint e; uint m; };

static inline uint bswap(uint x) { return as_type<uint>(as_type<uchar4>(x).wzyx); }

// $E0D3: byte-reverse M, E becomes the rounding byte, exponent $80, normalise, round.
static inline Seed scramble(uint M, uint E)
{
    uint m = bswap(M);
    uint z = clz(m);
    // v = (m:E) << z in a 40-bit window; mantissa = v >> 8, rounding bit = bit 7 of v
    uint mant = (m << z) | (z <= 8 ? E >> (8 - z) : E << (z - 8));
    uint rb = z < 8 ? ((E << z) >> 7) & 1 : 0;
    uint e = 0x80 - z;
    mant += rb;
    if (mant == 0 && rb) { mant = 0x80000000u; e += 1; }
    return Seed{ e, mant };
}

static inline Seed reseed(uint n)
{
    uint lz = clz(n);
    return scramble(n << lz, 0x80 + 32 - lz);
}

constant uint K = 0xB5447Au;          // 11879546's mantissa without its zero low byte
constant uint ADD_MANT = 0xA8B14600u;  // 3.927677739E-8

static inline Seed rnd(Seed s)
{
    uint mant, r;
    if ((s.m & 0x00FFFF00u) != 0 || (s.m & 0xFF) == 0) {
        // 40-bit window (s.m * K) >> 16: mantissa = bits 24..55, rounding byte = bits 16..23
        uint lo = s.m * K, hi = mulhi(s.m, K);
        mant = (hi << 8) | (lo >> 24);
        r = (lo >> 16) & 0xFF;
    } else {
        // FMULT's 9-bit-shift bug, mantissa = yy 00 00 xx (see rndfast.h)
        ulong acc = (ulong(s.m & 0xFF) * (ulong(K) << 8)) >> 16;
        ulong w = ((acc >> 9) << 8) | (acc & 0xFF);
        w = (w + ulong(s.m >> 24) * (ulong(K) << 16)) >> 8;
        mant = uint(w >> 8);
        r = uint(w) & 0xFF;
    }
    uint e = s.e + 0x18;
    if (!(mant >> 31)) {
        mant = (mant << 1) | (r >> 7);
        r = (r << 1) & 0xFF;
        e -= 1;
    }
    uint sh = e - 0x68;               // 16..49
    if (sh < 40) {
        uint am = sh < 32 ? ADD_MANT >> sh : 0;
        uint ar = (ADD_MANT >> (sh - 8)) & 0xFF;
        uint carry = (r + ar) >> 8;   // the result's rounding byte itself is discarded
        uint m2 = mant + am + carry;
        if (m2 < mant) {
            m2 = (m2 >> 1) | 0x80000000u;
            e += 1;
        }
        mant = m2;
    }
    return scramble(mant, e);
}

static inline uint times(Seed s, uint t, uint assign)
{
    uint sh = 0xA0 - s.e;             // 31..63
    if (!assign) {
        uint hi = mulhi(s.m, t), lo = s.m * t;
        return sh >= 32 ? hi >> (sh - 32) : (hi << 1) | (lo >> 31);
    }
    ulong p = ulong(t) * s.m;
    uint bits = 64 - clz(p);
    if (bits > 32) {
        uint d = bits - 32;
        p = ((p >> (d - 1)) + 1) >> 1 << d;
    }
    return uint(p >> sh);
}

// Cheap conservative test of the first letter (see fast_filter_pass in rndfast.h).
constant uint NORM_LIMIT = 0xB4C582ACu;   // ceil(2^55 / $B5447A)

static inline bool filter_pass(Seed s0, uint b_lo, uint b_span)
{
    uint lo = s0.m * K;
    uint b = s0.m >= NORM_LIMIT ? lo >> 24 : (lo >> 23) & 0xFF;
    uint carry = s0.e <= 0x78 ? 1 : 0;       // the addend may carry 1 into b
    bool exact_needed = s0.e <= 0x70 || s0.m == NORM_LIMIT - 1
                     || ((s0.m & 0x00FFFF00u) == 0 && (s0.m & 0xFF) != 0);
    return ((b - b_lo + carry) & 0xFF) <= b_span + carry || exact_needed;
}

struct Params {
    uint first;        // first seed of this dispatch
    uint count;        // seeds in this dispatch
    uint per_thread;   // loop iterations per thread (seeds strided by the grid size)
    uint b_lo, b_span; // filter range for the first letter
    uint scale;        // T
    uint assign;       // A%= rounding instead of INT()
    uint capacity;     // size of the results buffer
};

// Exact test of one seed: does it spell target[] and then stop?
static inline bool spells(uint n, constant uint *target, uint scale, uint assign)
{
    Seed s = reseed(n);
    for (uint i = 0;; i++) {
        s = rnd(s);
        uint v = times(s, scale, assign);
        if (v != target[i])
            return false;
        if (v == 0)
            return true;
    }
}

#define SIMD 32
#define MAX_SIMDGROUPS 32   // per threadgroup (1024 threads)

// Each SIMD group filters 32 seeds at a time and queues the ones that pass
// in threadgroup memory.  Whenever 32 are queued, the whole group checks
// them exactly, one per lane, so no lane idles and no global atomics are
// needed except for the (rare) results.
kernel void search(constant Params &p        [[buffer(0)]],
                   constant uint *target     [[buffer(1)]],
                   device atomic_uint *found [[buffer(2)]],
                   device uint *results      [[buffer(3)]],
                   uint tid   [[thread_position_in_grid]],
                   uint grid  [[threads_per_grid]],
                   uint lane  [[thread_index_in_simdgroup]],
                   uint sg    [[simdgroup_index_in_threadgroup]])
{
    threadgroup uint queues[MAX_SIMDGROUPS][2 * SIMD];
    threadgroup uint *queue = queues[sg];
    uint queued = 0;   // the same in every lane

    for (uint k = 0; k <= p.per_thread; k++) {
        bool last = k == p.per_thread;
        uint n = 0;
        bool pass = false;
        if (!last) {
            uint idx = k * grid + tid;
            n = p.first + idx;
            pass = idx < p.count && filter_pass(reseed(n), p.b_lo, p.b_span);
            if (pass)
                queue[queued + simd_prefix_exclusive_sum(1u)] = n;
            queued += simd_sum(pass ? 1u : 0u);
        }
        if (queued >= SIMD || (last && queued > 0)) {
            simdgroup_barrier(mem_flags::mem_threadgroup);
            bool have = lane < queued;
            uint c = have ? queue[lane] : 0;
            uint rest = queued > SIMD ? queued - SIMD : 0;
            uint moved = lane < rest ? queue[SIMD + lane] : 0;
            simdgroup_barrier(mem_flags::mem_threadgroup);
            if (lane < rest)
                queue[lane] = moved;
            queued = rest;
            if (have && spells(c, target, p.scale, p.assign)) {
                uint slot = atomic_fetch_add_explicit(found, 1, memory_order_relaxed);
                if (slot < p.capacity)
                    results[slot] = c;
            }
        }
    }
}
