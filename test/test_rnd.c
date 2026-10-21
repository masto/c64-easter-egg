/*
 * Checks the C model (src/c64float.c, src/c64rnd.c) against the real ROM
 * routines running on the 6502 oracle.
 *
 * usage: test_rnd [SEEDS [DRAWS [FUZZ]]]
 *   SEEDS  how many seeds RND(-n) to check, n = 1..SEEDS plus as many random n
 *   DRAWS  how many RND(1) draws to follow each seed with
 *   FUZZ   how many random FMULT / FADD operand pairs to try
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c64machine.h"
#include "../src/c64rnd.h"

/* ROM entry points */
#define ROM_FMULT  0xBA28   /* FAC1 = FAC1 * (A/Y) */
#define ROM_FADD   0xB867   /* FAC1 = FAC1 + (A/Y) */
#define ROM_MOVFM  0xBBA2   /* FAC1 = (A/Y) */
#define ROM_ROUND  0xBC1B
#define ROM_QINT   0xBC9B
#define ROM_RND    0xE097

/* Scratch RAM for operands, well clear of BASIC's work areas. */
#define SCRATCH    0xC000

static c64 m;
static long failures;

static void put_fac1(c64fac f)
{
    m.ram[0x61] = f.exp;
    m.ram[0x62] = (uint8_t)(f.mant >> 24);
    m.ram[0x63] = (uint8_t)(f.mant >> 16);
    m.ram[0x64] = (uint8_t)(f.mant >> 8);
    m.ram[0x65] = (uint8_t)f.mant;
    m.ram[0x66] = f.sign;
    m.ram[0x70] = f.round;
}

static c64fac get_fac1(void)
{
    c64fac f;
    f.exp = m.ram[0x61];
    f.mant = (uint32_t)m.ram[0x62] << 24 | (uint32_t)m.ram[0x63] << 16
           | (uint32_t)m.ram[0x64] << 8 | m.ram[0x65];
    f.sign = m.ram[0x66];
    f.round = m.ram[0x70];
    return f;
}

static void put_mem(uint16_t addr, c64float f)
{
    memcpy(&m.ram[addr], f.b, 5);
}

/* Compares two FACs, treating all zeros (exponent 0) as equal and looking at
   only the sign bit. */
static int fac_equal(c64fac a, c64fac b)
{
    if (a.exp == 0 || b.exp == 0)
        return a.exp == b.exp;
    return a.exp == b.exp && a.mant == b.mant && a.round == b.round
        && ((a.sign ^ b.sign) & 0x80) == 0;
}

static void report(const char *what, c64fac want, c64fac got)
{
    if (++failures <= 20)
        fprintf(stderr, "MISMATCH %s: rom %02X %08" PRIX32 " r%02X s%02X, model %02X %08" PRIX32 " r%02X s%02X\n",
                what, want.exp, want.mant, want.round, want.sign,
                got.exp, got.mant, got.round, got.sign);
}

/* --- RND --------------------------------------------------------------- */

static void check_seed(uint32_t n, int draws)
{
    char what[96];
    c64rnd model;

    /* RND(-n) */
    put_fac1(c64_fac_from_int(-(int64_t)n));
    m.ram[0x68] = 0;
    c64_call(&m, ROM_RND, 0, 0, 0);
    c64fac rom = get_fac1();
    c64fac mine = c64_rnd_reseed(&model, n);
    snprintf(what, sizeof what, "RND(-%" PRIu32 ")", n);
    if (!fac_equal(rom, mine) || memcmp(&m.ram[0x8B], model.seed.b, 5) != 0)
        report(what, rom, mine);

    for (int d = 1; d <= draws; d++) {
        /* RND(positive): FAC1 still holds the previous (positive) result. */
        c64_call(&m, ROM_RND, 0, 0, 0);
        rom = get_fac1();
        mine = c64_rnd_positive(&model);
        snprintf(what, sizeof what, "seed %" PRIu32 " draw %d", n, d);
        if (!fac_equal(rom, mine) || memcmp(&m.ram[0x8B], model.seed.b, 5) != 0) {
            report(what, rom, mine);
            return;
        }

        /* rnd * 22, as the interpreter does it: RND value (rounded) in FAC2,
           22 in FAC1, then INT() or A%= on the product. */
        c64fac value = mine;
        c64float packed = c64_pack(&value);
        for (int mode = 0; mode < 2; mode++) {
            put_mem(SCRATCH, packed);
            put_fac1(c64_fac_from_int(22));
            c64_call(&m, ROM_FMULT, SCRATCH & 0xFF, 0, SCRATCH >> 8);
            if (mode == C64_TO_INT_ASSIGN)
                c64_call(&m, ROM_ROUND, 0, 0, 0);
            c64_call(&m, ROM_QINT, 0, 0, 0);
            uint32_t rom_k = m.ram[0x65];
            uint32_t my_k = c64_rnd_times(mine, 22, (c64_to_int_mode)mode);
            if (rom_k != my_k && ++failures <= 20)
                fprintf(stderr, "MISMATCH seed %" PRIu32 " draw %d *22 mode %d: rom %" PRIu32 " model %" PRIu32 "\n",
                        n, d, mode, rom_k, my_k);
        }
        put_fac1(mine);   /* QINT clobbered FAC1; RND only needs it positive */
    }
}

/* --- FMULT / FADD fuzzing ------------------------------------------------ */

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

/* Random mantissa bytes, often 0 to exercise FMULT's zero-byte path. */
static uint8_t rand_byte(void)
{
    return (rng() % 3 == 0) ? 0 : (uint8_t)rng();
}

static c64fac rand_fac(int with_round)
{
    c64fac f;
    f.exp = (uint8_t)(0x60 + rng() % 0x40);
    f.mant = 0x80000000u | (uint32_t)(rand_byte() & 0x7F) << 24 | (uint32_t)rand_byte() << 16
           | (uint32_t)rand_byte() << 8 | rand_byte();
    f.sign = 0;
    f.round = with_round ? rand_byte() : 0;
    return f;
}

static void fuzz(long count)
{
    for (long i = 0; i < count; i++) {
        c64fac a = rand_fac(1), b = rand_fac(0);
        c64fac b_packed = b;
        c64float bmem = c64_pack(&b_packed);
        int add = i & 1;
        if (add && rng() % 4 == 0)
            b.exp = a.exp;   /* make the equal-exponent case common */
        b_packed = b;
        bmem = c64_pack(&b_packed);

        put_fac1(a);
        m.ram[0x68] = 0;
        put_mem(SCRATCH, bmem);
        c64_call(&m, add ? ROM_FADD : ROM_FMULT, SCRATCH & 0xFF, 0, SCRATCH >> 8);
        c64fac rom = get_fac1();

        c64fac mine = a, operand = c64_unpack(bmem);
        if (add)
            c64_fadd(&mine, &operand);
        else
            c64_fmult(&mine, &operand);
        if (!fac_equal(rom, mine)) {
            char what[64];
            snprintf(what, sizeof what, "%s %02X:%08" PRIX32 ":%02X by %02X:%08" PRIX32,
                     add ? "FADD" : "FMULT", a.exp, a.mant, a.round, b.exp, b.mant);
            report(what, rom, mine);
        }
    }
}

int main(int argc, char **argv)
{
    long seeds = argc > 1 ? atol(argv[1]) : 100000;
    int draws  = argc > 2 ? atoi(argv[2]) : 8;
    long fuzzn = argc > 3 ? atol(argv[3]) : 1000000;

    c64_init(&m);

    for (long n = 1; n <= seeds; n++)
        check_seed((uint32_t)n, draws);
    for (long i = 0; i < seeds; i++)
        check_seed(rng() % 4000000000u + 1, draws);
    printf("RND: %ld seeds x %d draws checked\n", 2 * seeds, draws);

    fuzz(fuzzn);
    printf("FMULT/FADD: %ld random operand pairs checked\n", fuzzn);

    if (failures) {
        printf("FAILED: %ld mismatches\n", failures);
        return 1;
    }
    printf("all OK\n");
    return 0;
}
