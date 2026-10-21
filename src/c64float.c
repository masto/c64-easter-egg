#include "c64float.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void fatal(const char *msg)
{
    fprintf(stderr, "c64float: %s\n", msg);
    abort();
}

c64fac c64_unpack(c64float f)
{
    c64fac fac;
    fac.exp   = f.b[0];
    fac.sign  = f.b[1];   /* the ROM copies the whole byte; only bit 7 is used */
    fac.mant  = (uint32_t)(f.b[1] | 0x80) << 24 | (uint32_t)f.b[2] << 16
              | (uint32_t)f.b[3] << 8 | f.b[4];
    fac.round = 0;
    return fac;
}

c64float c64_pack(c64fac *fac)
{
    c64float f;
    c64_round(fac);
    f.b[0] = fac->exp;
    f.b[1] = (uint8_t)((fac->mant >> 24) & ((fac->sign & 0x80) | 0x7F));
    f.b[2] = (uint8_t)(fac->mant >> 16);
    f.b[3] = (uint8_t)(fac->mant >> 8);
    f.b[4] = (uint8_t)fac->mant;
    fac->round = 0;
    return f;
}

c64fac c64_fac_from_int(int64_t n)
{
    c64fac fac = { 0, 0, 0, 0 };
    uint64_t mag = n < 0 ? (uint64_t)-n : (uint64_t)n;
    if (mag >= (1ull << 32))
        fatal("integer too large to be exact");
    if (mag == 0)
        return fac;
    fac.exp = 0x80;
    while (!(mag & 0x80000000u)) {   /* normalise: an integer with k bits has exponent $80+k */
        mag <<= 1;
        fac.exp--;
    }
    fac.exp += 32;
    fac.mant = (uint32_t)mag;
    fac.sign = n < 0 ? 0xFF : 0x00;
    return fac;
}

void c64_normalise(c64fac *fac)
{
    unsigned shift = 0;

    /* Whole bytes first.  After four byte shifts the value is taken to be
       zero, without looking at what came in from the rounding byte. */
    while ((fac->mant >> 24) == 0) {
        fac->mant = fac->mant << 8 | fac->round;
        fac->round = 0;
        shift += 8;
        if (shift == 32) {
            fac->exp = 0;
            fac->sign = 0;
            return;
        }
    }
    /* Then single bits. */
    while (!(fac->mant & 0x80000000u)) {
        fac->mant = fac->mant << 1 | fac->round >> 7;
        fac->round = (uint8_t)(fac->round << 1);
        shift++;
    }
    /* Underflow becomes zero (the mantissa bytes are left as they are). */
    if (shift >= fac->exp) {
        fac->exp = 0;
        fac->sign = 0;
        return;
    }
    fac->exp = (uint8_t)(fac->exp - shift);
}

void c64_round(c64fac *fac)
{
    if (fac->exp == 0)
        return;
    int round_up = fac->round >> 7;
    fac->round = (uint8_t)(fac->round << 1);   /* the ROM shifts the byte to test it */
    if (!round_up)
        return;
    if (++fac->mant == 0) {
        /* Carried out of the top: mantissa becomes $80000000 at the next exponent. */
        if (++fac->exp == 0)
            fatal("overflow in ROUND");
        fac->mant = 0x80000000u;
        fac->round >>= 1;
    }
}

/*
 * FMULT.  This is shift-and-add, one multiplier byte at a time, least
 * significant byte (FAC1's rounding byte) first.  The 32-bit running product
 * lives at $26-$29 (RESHO) and the bits shifted out of it go into $70, so the
 * result has 40 significant bits; anything below that is lost (truncated).
 *
 * The quirk: a zero multiplier byte is handled by jumping into FADD's
 * right-shift routine ($B983) to shift the product down 8 bits in one go.
 * That routine expects the carry flag to be set on entry and shifts one bit
 * further if it is clear.  It also exits with the carry clear.  A normal byte
 * leaves the carry set, so one zero byte is harmless, but a zero byte that
 * follows another zero byte shifts the product 9 bits instead of 8, and the
 * extra bit is simply lost.  The carry flag going into the first byte is left
 * over from the exponent arithmetic, so it is tracked here.
 *
 * (The shift routine also fills the top byte from $68, FAC1's overflow byte,
 * rather than with 0.  Only QINT ever sets $68 non-zero, and it always clears
 * it again before returning, so here it is always 0.)
 */
static void fmult_shift_zero_byte(uint32_t *product, uint8_t *round, int *carry)
{
    *round = (uint8_t)*product;
    *product >>= 8;
    if (!*carry)
        *product >>= 1;   /* the extra 9th bit; $70 does not receive it */
    *carry = 0;
}

static void fmult_add_shift_byte(uint32_t *product, uint8_t *round, uint32_t multiplicand,
                                 uint8_t multiplier, int *carry)
{
    for (int bit = 0; bit < 8; bit++) {
        uint64_t sum = *product;
        if (multiplier & (1u << bit))
            sum += multiplicand;
        /* Shift the 33-bit sum right one into $26-$29, and its low bit into $70. */
        *round = (uint8_t)(*round >> 1 | (sum & 1) << 7);
        *product = (uint32_t)(sum >> 1);
    }
    *carry = 1;   /* left set by the loop's final LSR of its sentinel bit */
}

void c64_fmult(c64fac *fac1, const c64fac *fac2)
{
    if (fac1->exp == 0)
        return;                       /* 0 * x = 0 */

    /* MULDIV ($BAB7): add the exponents. */
    if (fac2->exp == 0) {
        fac1->exp = 0;                /* x * 0 = 0 */
        fac1->sign = 0;
        return;
    }
    unsigned sum = (unsigned)fac2->exp + fac1->exp;
    int carry;
    uint8_t exp;
    if (sum >= 0x100) {
        if (sum - 0x100 >= 0x80)
            fatal("overflow in FMULT");
        exp = (uint8_t)(sum - 0x100 + 0x80);
        carry = 0;
    } else {
        if (sum < 0x80) {             /* underflow: result is zero */
            fac1->exp = 0;
            fac1->sign = 0;
            return;
        }
        exp = (uint8_t)(sum - 0x80);
        carry = 1;
    }
    uint8_t sign = (fac1->sign ^ fac2->sign) & 0x80;
    if (exp == 0) {
        fac1->exp = 0;
        fac1->sign = 0;
        return;
    }

    /* The multiplier bytes, read before $70 starts being overwritten. */
    uint8_t multiplier[5] = {
        fac1->round,
        (uint8_t)fac1->mant,
        (uint8_t)(fac1->mant >> 8),
        (uint8_t)(fac1->mant >> 16),
        (uint8_t)(fac1->mant >> 24),
    };
    uint32_t product = 0;
    uint8_t round = fac1->round;      /* gets shifted out completely below */

    for (int i = 0; i < 5; i++) {
        /* The top byte always goes through the add-and-shift loop. */
        if (multiplier[i] == 0 && i < 4)
            fmult_shift_zero_byte(&product, &round, &carry);
        else
            fmult_add_shift_byte(&product, &round, fac2->mant, multiplier[i], &carry);
    }

    fac1->exp = exp;
    fac1->sign = sign;
    fac1->mant = product;
    fac1->round = round;
    c64_normalise(fac1);
}

/* Shift a 40-bit mantissa+extension right by n bits, filling with zeros.
   (For FADD's alignment step: the ROM's $B983/$B9B0 shifter, which here only
   ever brings zeros in at the top.) */
static void shift_right_40(uint32_t *mant, uint8_t *ext, unsigned n)
{
    uint64_t v = (uint64_t)*mant << 8 | *ext;
    v = n >= 40 ? 0 : v >> n;
    *mant = (uint32_t)(v >> 8);
    *ext = (uint8_t)v;
}

void c64_fadd(c64fac *fac1, const c64fac *fac2)
{
    if (fac1->exp == 0) {             /* 0 + x = x */
        *fac1 = *fac2;
        fac1->round = 0;
        return;
    }
    if (fac2->exp == 0)
        return;                       /* x + 0 = x */
    if ((fac1->sign ^ fac2->sign) & 0x80)
        fatal("FADD of operands with different signs is not modelled");

    /* Align the operand with the smaller exponent to the larger one.  The bits
       shifted out of it land in an extension byte that lines up with FAC1's
       rounding byte.  The rounding byte of whichever operand is NOT shifted
       ($56) is added in alongside it. */
    uint32_t addend = fac2->mant;
    uint8_t ext;                      /* the 6502's A register in the ROM */
    uint8_t unshifted_round;          /* $56 */
    int carry = 0;

    if (fac2->exp == fac1->exp) {
        /* Quirk: with equal exponents, the carry flag left over from comparing
           the exponents is still set, so the rounding byte gets an extra +1. */
        ext = 0;
        unshifted_round = fac1->round;
        carry = 1;
    } else if (fac2->exp > fac1->exp) {
        unsigned n = fac2->exp - fac1->exp;
        ext = fac1->round;
        shift_right_40(&fac1->mant, &ext, n);
        fac1->exp = fac2->exp;
        unshifted_round = 0;          /* FAC2 has no rounding byte */
    } else {
        unsigned n = fac1->exp - fac2->exp;
        ext = 0;
        shift_right_40(&addend, &ext, n);
        unshifted_round = fac1->round;
    }

    /* 40-bit add: rounding bytes first, then the mantissas. */
    unsigned r = ext + unshifted_round + carry;
    fac1->round = (uint8_t)r;
    uint64_t sum = (uint64_t)fac1->mant + addend + (r >> 8);
    fac1->mant = (uint32_t)sum;
    if (sum >> 32) {
        /* Carry out of the top: shift right one and bump the exponent. */
        if (++fac1->exp == 0)
            fatal("overflow in FADD");
        fac1->round = (uint8_t)(fac1->round >> 1 | (fac1->mant & 1) << 7);
        fac1->mant = fac1->mant >> 1 | 0x80000000u;
    }
}

uint32_t c64_qint(const c64fac *fac)
{
    if (fac->sign & 0x80)
        fatal("QINT of a negative number is not modelled");
    if (fac->exp >= 0xA0)
        fatal("QINT argument too large");
    if (fac->exp <= 0x80)
        return 0;                     /* below 1 */
    return fac->mant >> (0xA0 - fac->exp);
}

double c64_to_double(c64fac fac)
{
    if (fac.exp == 0)
        return 0.0;
    double v = ldexp((double)fac.mant + fac.round / 256.0, fac.exp - 0x80 - 32);
    return (fac.sign & 0x80) ? -v : v;
}
