#include "c64rnd.h"

/* The two constants in ROM at $E08D and $E092. */
static const c64float RND_MULTIPLIER = { { 0x98, 0x35, 0x44, 0x7A, 0x00 } };  /* 11879546 */
static const c64float RND_ADDEND     = { { 0x68, 0x28, 0xB1, 0x46, 0x00 } };  /* 3.927677739E-8 */

/* $E0D3: the common tail of RND for x < 0 and x > 0. */
static c64fac scramble(c64rnd *r, c64fac fac)
{
    uint32_t m = fac.mant;
    fac.mant = (m & 0xFF) << 24 | (m & 0xFF00) << 8 | (m >> 8 & 0xFF00) | m >> 24;
    fac.sign = 0;
    fac.round = fac.exp;
    fac.exp = 0x80;
    c64_normalise(&fac);
    r->seed = c64_pack(&fac);   /* rounds fac too, so the return value matches the seed */
    return fac;
}

c64fac c64_rnd_negative(c64rnd *r, c64fac x)
{
    return scramble(r, x);
}

c64fac c64_rnd_positive(c64rnd *r)
{
    c64fac fac = c64_unpack(r->seed);
    c64fac multiplier = c64_unpack(RND_MULTIPLIER);
    c64fac addend = c64_unpack(RND_ADDEND);
    c64_fmult(&fac, &multiplier);
    c64_fadd(&fac, &addend);
    return scramble(r, fac);
}

c64fac c64_rnd_reseed(c64rnd *r, uint32_t n)
{
    return c64_rnd_negative(r, c64_fac_from_int(-(int64_t)n));
}

uint32_t c64_rnd_times(c64fac rnd_value, uint32_t scale, c64_to_int_mode mode)
{
    /* Pushing the left operand rounds it.  RND's result is already rounded,
       so this changes nothing, but it is what the interpreter does. */
    c64_round(&rnd_value);
    c64fac fac2 = rnd_value;
    c64fac fac1 = c64_fac_from_int(scale);
    c64_fmult(&fac1, &fac2);
    if (mode == C64_TO_INT_ASSIGN)
        c64_round(&fac1);
    return c64_qint(&fac1);
}
