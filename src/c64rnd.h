/*
 * Commodore 64 BASIC RND(), bit for bit.
 *
 * The ROM routine (at $E097) works like this:
 *
 *   RND(x), x < 0:  take x itself as the new seed (see "scramble" below).
 *   RND(x), x > 0:  seed = seed * 11879546 + 3.927677739E-8, then scramble.
 *                   The value of x does not matter.
 *   RND(0):         seed from the CIA timers and clock (not modelled).
 *
 *   scramble:       swap the mantissa bytes 1<->4 and 2<->3, force the sign
 *                   positive, use the old exponent as the rounding byte, set
 *                   the exponent to $80 (so the value is below 1), normalise,
 *                   and store the rounded result as the seed (RNDX, $8B-$8F).
 *                   That value is also what RND returns.
 *
 * So RND(-n) followed by RND(1), RND(1), ... is a fixed sequence for every n,
 * which is what the easter egg programs rely on.
 */
#ifndef C64RND_H
#define C64RND_H

#include <stdint.h>

#include "c64float.h"

typedef struct {
    c64float seed;   /* RNDX, $8B-$8F */
} c64rnd;

/* RND(x) for x < 0: reseed from x.  Returns the result (also the new seed). */
c64fac c64_rnd_negative(c64rnd *r, c64fac x);

/* RND(x) for x > 0: the next number in the sequence. */
c64fac c64_rnd_positive(c64rnd *r);

/* Shorthand for RND(-n), with n a whole number as BASIC would hold it. */
c64fac c64_rnd_reseed(c64rnd *r, uint32_t n);

/*
 * Scaling RND to a whole number.  The programs do it in two slightly
 * different ways, which can disagree for rare values:
 *
 *   A=INT(RND(A)*22)    (the easter egg)  - INT() truncates the full 40-bit
 *                                           product.
 *   A%=RND(A)*T         (Robin's search)  - assigning to an integer variable
 *                                           first rounds the product to 32 bits.
 *
 * In both, the left operand (the RND value) is the one pushed on the stack,
 * so it ends up in FAC2 and the scale factor in FAC1 is the multiplier.
 */
typedef enum {
    C64_TO_INT_TRUNCATE,   /* INT(x) */
    C64_TO_INT_ASSIGN,     /* A% = x */
} c64_to_int_mode;

uint32_t c64_rnd_times(c64fac rnd_value, uint32_t scale, c64_to_int_mode mode);

#endif
