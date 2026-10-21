/*
 * Bit-exact model of the Commodore 64 BASIC V2 floating point routines that
 * RND() and the "easter egg" programs depend on.
 *
 * Commodore (Microsoft) BASIC stores a number in 5 bytes:
 *
 *     byte 0      exponent, biased by $80 ($81 means 2^1); 0 means the value is 0
 *     bytes 1-4   32-bit mantissa, most significant byte first.  The top bit of
 *                 a normalised mantissa is always 1, so in memory it holds the sign.
 *
 * value = mantissa / 2^32 * 2^(exponent - $80), i.e. the mantissa is a fraction
 * in [0.5, 1).
 *
 * Arithmetic happens in "floating point accumulators" in zero page.  FAC1 has an
 * extra 8 bits below the mantissa, the rounding byte at $70, which catches bits
 * shifted out during arithmetic.  Rounding to 32 bits happens only when FAC1 is
 * stored to memory or pushed as an operand, and how often that happens is
 * visible in the results.  This model keeps that byte and reproduces the ROM's
 * quirks, so every function is named after, and follows, the ROM routine it
 * mirrors.
 */
#ifndef C64FLOAT_H
#define C64FLOAT_H

#include <stdint.h>

/* A floating point accumulator ($61-$66 and $70 for FAC1, $69-$6E for FAC2). */
typedef struct {
    uint8_t  exp;      /* $61: biased exponent, 0 = zero */
    uint32_t mant;     /* $62-$65: mantissa, top bit set when normalised */
    uint8_t  sign;     /* $66: only bit 7 matters, set = negative */
    uint8_t  round;    /* $70: the next 8 bits below the mantissa (FAC1 only) */
} c64fac;

/* A number as stored in a variable or in ROM. */
typedef struct {
    uint8_t b[5];
} c64float;

/* MOVFM ($BBA2): load FAC from a 5-byte number.  Clears the rounding byte. */
c64fac c64_unpack(c64float f);

/* MOVMF ($BBD4): round FAC (which modifies it, as in the ROM) and return it
   in 5-byte form.  Afterwards the rounding byte is 0. */
c64float c64_pack(c64fac *fac);

/* The value BASIC holds for an integer such as the literal 125708 or a FOR
   loop counter (exact for |n| < 2^32). */
c64fac c64_fac_from_int(int64_t n);

/* NORMAL ($B8D7): shift left until the mantissa's top bit is set, pulling in
   bits from the rounding byte. */
void c64_normalise(c64fac *fac);

/* ROUND ($BC1B): round the 40-bit mantissa+rounding byte to 32 bits. */
void c64_round(c64fac *fac);

/* FMULT ($BA28/$BA2B): fac1 = fac1 * fac2.  The bytes of fac1 (rounding byte
   first) are the multiplier and fac2's mantissa is the multiplicand, so the
   operand order matters to the exact result. */
void c64_fmult(c64fac *fac1, const c64fac *fac2);

/* FADD ($B867): fac1 = fac1 + fac2, for operands with the same sign (the only
   case RND needs; anything else aborts). */
void c64_fadd(c64fac *fac1, const c64fac *fac2);

/* QINT ($BC9B) for a non-negative FAC below 2^31: the integer part, truncated.
   This is what INT() does. */
uint32_t c64_qint(const c64fac *fac);

/* Converting to a double, for display and debugging only. */
double c64_to_double(c64fac fac);

#endif
