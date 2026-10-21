/* Runs a tokenised BASIC .prg on the stock ROMs and prints its output.
   usage: runbasic program.prg [max-instructions] */
#include <stdio.h>
#include <stdlib.h>

#include "c64machine.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s program.prg [max-instructions]\n", argv[0]);
        return 2;
    }
    static uint8_t prg[65536];
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    size_t len = fread(prg, 1, sizeof prg, f);
    fclose(f);

    static c64 m;
    c64_init(&m);
    const char *out = c64_run_basic(&m, prg, len, argc > 2 ? strtoull(argv[2], NULL, 0) : 0);
    if (!out) {
        fprintf(stderr, "instruction limit reached\n");
        return 1;
    }
    fputs(out, stdout);
    fprintf(stderr, "(%llu instructions)\n", (unsigned long long)m.cpu.instructions);
    return 0;
}
