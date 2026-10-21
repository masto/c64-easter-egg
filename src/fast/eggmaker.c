/*
 * eggmaker: write a fake easter egg for any text.
 *
 * usage: eggmaker [--scale T] [--offset S] [--max N] [-j THREADS] [-o FILE.prg] TEXT
 *
 * Prints a BASIC program in the style of Robin's (8-Bit Show and Tell):
 *
 *     10 PRINT:A=148094359:GOSUB 20:A=105819249:GOSUB 20:A=12573293:GOSUB 20:END
 *     20 A=RND(-A)
 *     30 A=INT(RND(A)*22):IF A THEN PRINT CHR$(A+64);:GOTO30
 *     40 PRINT" ";:RETURN
 *
 * Each word of TEXT gets the smallest integer seed that spells it.  A word
 * with no seed is split into the fewest chunks that all have one, longest
 * chunks first ("SUBSCRIBE" -> "SUBSC RIBE"); each chunk prints with a space
 * after it, as in Robin's finale.
 *
 * How: every substring of every word goes into a trie, and one pass over all
 * seeds 1..N walks each seed's letters down the trie, stopping as soon as
 * they leave it, and keeps the smallest seed for each substring.
 *
 * -o also writes the program as a tokenised .prg (LOAD"FILE",8 / x64sc FILE).
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

#include "rndfast.h"
#include "../c64rnd.h"

#define CHUNK (1u << 20)
#define NONE  0xFFFFFFFFu

/* ---- the words and their substrings ------------------------------------ */

typedef struct {
    int32_t *child;      /* [node * (scale + 1) + value] -> node, or -1 */
    int32_t *target;     /* [node] -> substring id, or -1 */
    size_t   nodes, cap;
    uint32_t width;      /* scale + 1 */
} trie;

static int32_t trie_new_node(trie *t)
{
    if (t->nodes == t->cap) {
        t->cap = t->cap ? 2 * t->cap : 256;
        t->child = realloc(t->child, t->cap * t->width * sizeof *t->child);
        t->target = realloc(t->target, t->cap * sizeof *t->target);
        if (!t->child || !t->target) { perror("realloc"); exit(2); }
    }
    for (uint32_t v = 0; v < t->width; v++)
        t->child[t->nodes * t->width + v] = -1;
    t->target[t->nodes] = -1;
    return (int32_t)t->nodes++;
}

/* Adds a substring (as RND*T values) and returns its id, reusing duplicates. */
static int32_t trie_add(trie *t, const uint8_t *vals, size_t len, int32_t *ntargets)
{
    int32_t node = 0;
    for (size_t i = 0; i < len; i++) {
        int32_t *slot = &t->child[(size_t)node * t->width + vals[i]];
        if (*slot < 0) {
            int32_t fresh = trie_new_node(t);
            slot = &t->child[(size_t)node * t->width + vals[i]];   /* realloc may move it */
            *slot = fresh;
        }
        node = *slot;
    }
    if (t->target[node] < 0)
        t->target[node] = (*ntargets)++;
    return t->target[node];
}

/* ---- the scan ------------------------------------------------------------ */

typedef struct {
    const trie *t;
    uint32_t scale;
    uint8_t  first_ok[256];  /* filter byte b can give some wanted first letter */
    uint64_t last;
    int32_t  ntargets;
    _Atomic uint64_t next;   /* next chunk */
    uint32_t **best;         /* per thread: smallest seed per substring */
} scan;

/* fast_filter_pass, but for a set of wanted first letters. */
static inline int first_pass(const scan *sc, fseed s0)
{
    uint32_t lo = s0.mant * FAST_RND_MULT;
    uint32_t b = s0.mant >= FAST_NORM_LIMIT ? lo >> 24 : (lo >> 23) & 0xFF;
    int carry = s0.exp <= 0x78;          /* the real byte may be b + 1 */
    int exact_needed = s0.exp <= 0x70 || s0.mant == FAST_NORM_LIMIT - 1
                    || ((s0.mant & 0x00FFFF00u) == 0 && (s0.mant & 0xFF) != 0);
    return sc->first_ok[b] || (carry && sc->first_ok[(b + 1) & 0xFF]) || exact_needed;
}

typedef struct { scan *sc; int id; } worker_arg;

static void *worker(void *arg)
{
    scan *sc = ((worker_arg *)arg)->sc;
    uint32_t *best = sc->best[((worker_arg *)arg)->id];
    const int32_t *child = sc->t->child, *target = sc->t->target;
    const uint32_t width = sc->t->width, scale = sc->scale;
    for (;;) {
        uint64_t c = atomic_fetch_add(&sc->next, 1);
        uint64_t lo = c * CHUNK, hi = lo + CHUNK;
        if (lo > sc->last)
            break;
        if (lo == 0)
            lo = 1;
        if (hi > sc->last + 1)
            hi = sc->last + 1;
        for (uint64_t n = lo; n < hi; n++) {
            fseed s = fast_reseed((uint32_t)n);
            if (!first_pass(sc, s))
                continue;
            int32_t node = 0;
            for (;;) {
                s = fast_rnd(s);
                uint32_t k = fast_times_int(s, scale);
                if (k == 0) {
                    int32_t id = target[node];
                    if (id >= 0 && (uint32_t)n < best[id])
                        best[id] = (uint32_t)n;
                    break;
                }
                node = child[(size_t)node * width + k];
                if (node < 0)
                    break;
            }
        }
    }
    return NULL;
}

/* ---- output -------------------------------------------------------------- */

/* BASIC V2 tokens for the keywords the program uses. */
static const struct { const char *word; uint8_t token; } keywords[] = {
    { "END", 0x80 }, { "GOTO", 0x89 }, { "IF", 0x8B }, { "GOSUB", 0x8D },
    { "RETURN", 0x8E }, { "PRINT", 0x99 }, { "THEN", 0xA7 }, { "+", 0xAA },
    { "-", 0xAB }, { "*", 0xAC }, { "=", 0xB2 }, { "INT", 0xB5 },
    { "RND", 0xBB }, { "CHR$", 0xC7 },
};

/* Tokenises one line of the program into out (link, line number, tokens, 0). */
static size_t tokenise(const char *line, uint8_t *out)
{
    char *end;
    unsigned long number = strtoul(line, &end, 10);
    size_t n = 4;
    out[2] = (uint8_t)number;
    out[3] = (uint8_t)(number >> 8);
    const char *p = end;
    while (*p == ' ')
        p++;
    int quoted = 0;
    while (*p) {
        if (*p == '"')
            quoted = !quoted;
        if (!quoted) {
            size_t k;
            for (k = 0; k < sizeof keywords / sizeof keywords[0]; k++) {
                size_t len = strlen(keywords[k].word);
                if (strncmp(p, keywords[k].word, len) == 0) {
                    out[n++] = keywords[k].token;
                    p += len;
                    break;
                }
            }
            if (k < sizeof keywords / sizeof keywords[0])
                continue;
        }
        out[n++] = (uint8_t)*p++;     /* upper case ASCII = unshifted PETSCII */
    }
    out[n++] = 0;
    return n;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [--scale T] [--offset S] [--max N] [-j THREADS] [-o FILE.prg] TEXT\n"
        "  --scale T   letters are CHR$(S + INT(RND*T)) (default 22: A-U, and V rarely)\n"
        "  --offset S  (default 64); --scale 27 gives A-Z\n"
        "  --max N     largest seed to use (default 4294967295; 999999999 keeps 9 digits)\n"
        "  -o FILE     also write the program as a tokenised .prg\n", prog);
    exit(2);
}

static uint64_t parse_number(const char *s, uint64_t max, const char *prog)
{
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v > max)
        usage(prog);
    return v;
}

int main(int argc, char **argv)
{
    const char *prog = argv[0], *prg_path = NULL;
    uint64_t scale = 22, offset = 64, max = 0xFFFFFFFFu, threads = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
        if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
            scale = parse_number(argv[++i], 255, prog);
        else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc)
            offset = parse_number(argv[++i], 255, prog);
        else if (strcmp(argv[i], "--max") == 0 && i + 1 < argc)
            max = parse_number(argv[++i], 0xFFFFFFFFu, prog);
        else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc)
            threads = parse_number(argv[++i], 1024, prog);
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            prg_path = argv[++i];
        else
            usage(prog);
    }
    if (i != argc - 1 || scale < 2 || max == 0)
        usage(prog);

    /* Split the text into upper-case words and check every letter can appear. */
    char text[4096];
    size_t tlen = strlen(argv[i]);
    if (tlen >= sizeof text)
        usage(prog);
    int bad = 0;
    char reported[256] = { 0 };
    for (size_t k = 0; k <= tlen; k++) {
        char ch = argv[i][k];
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 'a' + 'A');
        if (ch == '\t' || ch == '\n')
            ch = ' ';
        text[k] = ch;
        int v = (unsigned char)ch - (int)offset;
        if (ch != ' ' && ch != '\0' && (v < 1 || v > (int)scale) && !reported[(unsigned char)ch]++) {
            fprintf(stderr, "%s: '%c' can't be printed with CHR$(%llu+INT(RND*%llu))\n", prog, ch,
                    (unsigned long long)offset, (unsigned long long)scale);
            bad = 1;
        }
    }
    if (bad) {
        if (offset == 64 && scale < 27)
            fprintf(stderr, "(--scale 27 allows A-Z)\n");
        return 1;
    }
    char *words[512];
    size_t nwords = 0;
    for (char *w = strtok(text, " "); w; w = strtok(NULL, " ")) {
        if (nwords == sizeof words / sizeof words[0])
            usage(prog);
        words[nwords++] = w;
    }
    if (nwords == 0)
        usage(prog);

    /* Every substring of every word goes in the trie. */
    trie t = { .width = (uint32_t)scale + 1 };
    trie_new_node(&t);
    int32_t ntargets = 0;
    int32_t *ids[512];
    scan sc = { .t = &t, .scale = (uint32_t)scale, .last = max };
    for (size_t w = 0; w < nwords; w++) {
        size_t len = strlen(words[w]);
        uint8_t vals[4096];
        for (size_t k = 0; k < len; k++)
            vals[k] = (uint8_t)((unsigned char)words[w][k] - offset);
        ids[w] = malloc(len * len * sizeof *ids[w]);   /* [start * len + end - 1] */
        for (size_t a = 0; a < len; a++) {
            for (size_t b = a + 1; b <= len; b++)
                ids[w][a * len + b - 1] = trie_add(&t, vals + a, b - a, &ntargets);
            /* the scan's first-letter filter: which filter bytes allow vals[a] */
            for (uint32_t byte = 0; byte < 256; byte++)
                if (scale * byte / 256 <= vals[a] && vals[a] <= scale * (byte + 1) / 256)
                    sc.first_ok[byte] = 1;
        }
    }

    /* One pass over every seed. */
    if (threads == 0) {
        int ncpu = 0;
        size_t sz = sizeof ncpu;
        if (sysctlbyname("hw.ncpu", &ncpu, &sz, NULL, 0) != 0 || ncpu < 1)
            ncpu = 1;
        threads = (uint64_t)ncpu;
    }
    sc.ntargets = ntargets;
    sc.best = malloc(threads * sizeof *sc.best);
    for (uint64_t k = 0; k < threads; k++) {
        sc.best[k] = malloc((size_t)ntargets * sizeof **sc.best);
        memset(sc.best[k], 0xFF, (size_t)ntargets * sizeof **sc.best);
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_t tid[1024];
    worker_arg args[1024];
    for (uint64_t k = 0; k < threads; k++) {
        args[k] = (worker_arg){ &sc, (int)k };
        pthread_create(&tid[k], NULL, worker, &args[k]);
    }
    for (uint64_t k = 0; k < threads; k++)
        pthread_join(tid[k], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t *best = sc.best[0];
    for (uint64_t k = 1; k < threads; k++)
        for (int32_t id = 0; id < ntargets; id++)
            if (sc.best[k][id] < best[id])
                best[id] = sc.best[k][id];
    fprintf(stderr, "searched seeds 1..%llu for %d substrings in %.2f s\n\n", (unsigned long long)max,
            ntargets, (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9);

    /* Per word: fewest chunks, longest chunks first. */
    uint32_t seeds[4096];
    size_t nseeds = 0;
    for (size_t w = 0; w < nwords; w++) {
        const char *word = words[w];
        size_t len = strlen(word);
        int need[4097];                  /* fewest chunks for word[a..] */
        need[len] = 0;
        for (size_t a = len; a-- > 0;) {
            need[a] = 1 << 30;
            for (size_t b = a + 1; b <= len; b++)
                if (best[ids[w][a * len + b - 1]] != NONE && 1 + need[b] < need[a])
                    need[a] = 1 + need[b];
        }
        if (need[0] >= 1 << 30) {
            fprintf(stderr, "%s: no seeds up to %llu can spell \"%s\"\n", prog, (unsigned long long)max, word);
            return 1;
        }
        fprintf(stderr, "%-16s ->", word);
        for (size_t a = 0; a < len;) {
            size_t b = len;
            while (best[ids[w][a * len + b - 1]] == NONE || 1 + need[b] != need[a])
                b--;
            uint32_t seed = best[ids[w][a * len + b - 1]];

            /* Double-check with the reference model. */
            c64rnd r;
            c64_rnd_reseed(&r, seed);
            for (size_t k = a; k <= b; k++) {
                uint32_t v = c64_rnd_times(c64_rnd_positive(&r), (uint32_t)scale, C64_TO_INT_TRUNCATE);
                uint32_t want = k < b ? (uint32_t)((unsigned char)word[k] - offset) : 0;
                if (v != want) {
                    fprintf(stderr, "\ninternal error: seed %u does not spell %.*s\n", seed, (int)(b - a), word + a);
                    return 2;
                }
            }
            fprintf(stderr, " %.*s (%u)", (int)(b - a), word + a, seed);
            seeds[nseeds++] = seed;
            a = b;
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");

    /* The program.  Typed lines must fit the C64's 80-column logical lines. */
    char lines[4200][96];
    size_t nlines = 0;
    unsigned lineno = 10;
    char cur[96];
    snprintf(cur, sizeof cur, "%u PRINT", lineno);
    for (size_t k = 0; k < nseeds; k++) {
        char part[40];
        snprintf(part, sizeof part, ":A=%u:GOSUB 20", seeds[k]);
        if (strlen(cur) + strlen(part) + 4 > 80) {   /* room for ":END" */
            memcpy(lines[nlines++], cur, sizeof cur);
            if (++lineno == 20) {
                fprintf(stderr, "%s: too much text for lines 10-19\n", prog);
                return 1;
            }
            snprintf(cur, sizeof cur, "%u %s", lineno, part + 1);
        } else {
            strcat(cur, part);
        }
    }
    strcat(cur, ":END");
    memcpy(lines[nlines++], cur, sizeof cur);
    snprintf(lines[nlines++], 96, "20 A=RND(-A)");
    snprintf(lines[nlines++], 96, "30 A=INT(RND(A)*%llu):IF A THEN PRINT CHR$(A+%llu);:GOTO30",
             (unsigned long long)scale, (unsigned long long)offset);
    snprintf(lines[nlines++], 96, "40 PRINT\" \";:RETURN");
    for (size_t k = 0; k < nlines; k++)
        puts(lines[k]);

    if (prg_path) {
        uint8_t prg[65536];
        size_t n = 0;
        uint16_t addr = 0x0801;
        prg[n++] = (uint8_t)addr;
        prg[n++] = (uint8_t)(addr >> 8);
        for (size_t k = 0; k < nlines; k++) {
            size_t len = tokenise(lines[k], prg + n);
            addr = (uint16_t)(addr + len);
            prg[n] = (uint8_t)addr;          /* link to the next line */
            prg[n + 1] = (uint8_t)(addr >> 8);
            n += len;
        }
        prg[n++] = 0;
        prg[n++] = 0;
        FILE *f = fopen(prg_path, "wb");
        if (!f || fwrite(prg, 1, n, f) != n || fclose(f) != 0) {
            perror(prg_path);
            return 2;
        }
    }
    return 0;
}
