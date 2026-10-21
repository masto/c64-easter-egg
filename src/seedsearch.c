/*
 * seedsearch: find RND seeds that spell a word, the slow and obvious way.
 *
 * This is the reference implementation that faster versions are tested
 * against.  It does exactly what Robin's BASIC search program does, using a
 * bit-exact model of the C64's floating point and RND routines:
 *
 *     10 B$="AN":T=22:S=64
 *     20 FORB=1TO99999999
 *     100 A=RND(-B):A$=""
 *     110 A%=RND(A)*T:IFA%THENA$=A$+CHR$(A%+S):IFLEN(A$)<=LEN(B$)THEN110
 *     120 IFA$=B$THENPRINTB,B$
 *     130 NEXT
 *
 * That is: for each seed n, call RND(-n), then keep drawing letters
 * CHR$(S + RND*T) until RND*T is 0.  The seed is a match if the letters drawn
 * before that 0 are exactly the word.
 *
 * The easter egg prints with A=INT(RND(A)*22), which can differ from
 * A%=RND(A)*T for rare RND values (see c64rnd.h).  By default this searches
 * for seeds that work in the easter egg; --assign reproduces Robin's program.
 *
 * usage: seedsearch [--assign] [--scale T] [--offset S] WORD [FIRST [LAST]]
 *
 * Prints one line per matching seed: "<seed> <word>".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c64rnd.h"

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [--assign] [--scale T] [--offset S] WORD [FIRST [LAST]]\n"
        "  WORD      letters to search for (case-insensitive)\n"
        "  FIRST     first seed to try (default 1)\n"
        "  LAST      last seed to try (default 99999999)\n"
        "  --scale   T in CHR$(S + RND*T) (default 22)\n"
        "  --offset  S in CHR$(S + RND*T) (default 64)\n"
        "  --assign  A%%=RND(A)*T, as in Robin's search, instead of the easter\n"
        "            egg's A=INT(RND(A)*T)\n",
        prog);
    exit(2);
}

static uint32_t parse_number(const char *s, const char *prog)
{
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v > 0xFFFFFFFFull)
        usage(prog);
    return (uint32_t)v;
}

/* Does seed n spell exactly `word`, then stop? */
static int seed_spells(uint32_t n, const char *word, uint32_t scale, uint32_t offset,
                       c64_to_int_mode mode)
{
    c64rnd rnd;
    c64_rnd_reseed(&rnd, n);
    for (size_t i = 0; ; i++) {
        uint32_t k = c64_rnd_times(c64_rnd_positive(&rnd), scale, mode);
        if (k == 0)
            return word[i] == '\0';   /* the word ends here: a match if we used all of it */
        if (word[i] == '\0' || (uint32_t)(unsigned char)word[i] != k + offset)
            return 0;                 /* a letter too many, or the wrong letter */
    }
}

int main(int argc, char **argv)
{
    const char *prog = argv[0];
    uint32_t scale = 22, offset = 64;
    c64_to_int_mode mode = C64_TO_INT_TRUNCATE;

    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (strcmp(argv[i], "--assign") == 0)
            mode = C64_TO_INT_ASSIGN;
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
            scale = parse_number(argv[++i], prog);
        else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc)
            offset = parse_number(argv[++i], prog);
        else
            usage(prog);
    }
    if (i >= argc || argc - i > 3)
        usage(prog);

    char word[256];
    size_t len = strlen(argv[i]);
    if (len == 0 || len >= sizeof word)
        usage(prog);
    for (size_t j = 0; j <= len; j++) {
        char ch = argv[i][j];
        word[j] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
    }
    uint32_t first = argc - i > 1 ? parse_number(argv[i + 1], prog) : 1;
    uint32_t last  = argc - i > 2 ? parse_number(argv[i + 2], prog) : 99999999;

    for (uint32_t n = first; n <= last; n++) {
        if (seed_spells(n, word, scale, offset, mode))
            printf("%u %s\n", n, word);
        if (n == UINT32_MAX)
            break;
    }
    return 0;
}
