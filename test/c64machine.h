/*
 * Just enough of a C64 to run the stock BASIC and KERNAL ROMs on the
 * cpu6502 core: 64K RAM with BASIC, I/O and KERNAL banked in (the power-on
 * layout), I/O reads returning constants, and no interrupts.  That is enough
 * to boot to READY., type RUN into the keyboard buffer, and capture what the
 * program PRINTs.  It also lets tests call individual ROM routines directly.
 *
 * The ROM images are read from $C64_ROMDIR, defaulting to VICE's copies under
 * /opt/homebrew/share/vice/C64.
 */
#ifndef C64MACHINE_H
#define C64MACHINE_H

#include <stddef.h>
#include <stdint.h>

#include "cpu6502.h"

typedef struct {
    cpu6502 cpu;
    uint8_t ram[0x10000];
    uint8_t basic[0x2000];    /* $A000-$BFFF */
    uint8_t kernal[0x2000];   /* $E000-$FFFF */

    /* Text sent to CHROUT ($FFD2) while capturing, in PETSCII. */
    char  *out;
    size_t out_len, out_cap;
    int    capturing;
} c64;

/* Loads the ROMs and boots to the READY. prompt.  Exits on failure. */
void c64_init(c64 *m);

/* Calls the machine-language routine at addr with the current RAM contents
   and returns when it executes its final RTS.  Registers come back in m->cpu. */
void c64_call(c64 *m, uint16_t addr, uint8_t a, uint8_t x, uint8_t y);

/* Loads a .prg (two-byte load address first) as a BASIC program, types RUN,
   and runs until BASIC is waiting for keyboard input again (or until
   max_instructions; 0 = no limit).  Returns the PRINT output as a
   NUL-terminated ASCII string owned by m, or NULL if the limit was hit. */
const char *c64_run_basic(c64 *m, const uint8_t *prg, size_t len, uint64_t max_instructions);

#endif
