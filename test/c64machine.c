#include "c64machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KERNAL_WAIT_FOR_KEY 0xE5CD   /* idle loop polling the keyboard buffer count at $C6 */
#define KERNAL_CHROUT       0xFFD2
#define CALL_RETURN_TRAP    0x0002   /* unused zero page byte; c64_call returns here */

static uint8_t mem_read(cpu6502 *c, uint16_t addr)
{
    c64 *m = c->user;
    if (addr >= 0xA000 && addr < 0xC000)
        return m->basic[addr - 0xA000];
    if (addr >= 0xE000)
        return m->kernal[addr - 0xE000];
    if (addr >= 0xD000 && addr < 0xE000) {
        /* I/O.  CIA1 port reads of $FF mean "no key pressed, no joystick".
           Everything else reads 0, which also satisfies the KERNAL's
           wait-for-raster-line-0 loop during screen setup. */
        if (addr == 0xDC00 || addr == 0xDC01)
            return 0xFF;
        return 0;
    }
    return m->ram[addr];
}

static void mem_write(cpu6502 *c, uint16_t addr, uint8_t val)
{
    c64 *m = c->user;
    if (addr >= 0xD000 && addr < 0xE000)
        return;               /* I/O writes are ignored */
    m->ram[addr] = val;       /* writes "under" ROM land in RAM, as on the real machine */
}

static void load_rom(const char *dir, const char *name, uint8_t *dst, size_t len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f || fread(dst, 1, len, f) != len) {
        fprintf(stderr, "c64machine: cannot read ROM %s\n", path);
        exit(2);
    }
    fclose(f);
}

static void capture(c64 *m, uint8_t petscii)
{
    char ch;
    if (petscii == 13)
        ch = '\n';
    else if (petscii == 29)
        ch = ' ';             /* cursor right: how PRINT's comma tabs */
    else if (petscii >= 32 && petscii < 96)
        ch = (char)petscii;   /* upper case letters, digits, punctuation match ASCII */
    else
        return;               /* colour codes, cursor moves etc. */
    if (m->out_len + 2 > m->out_cap) {
        m->out_cap = m->out_cap ? m->out_cap * 2 : 4096;
        m->out = realloc(m->out, m->out_cap);
        if (!m->out) { perror("realloc"); exit(2); }
    }
    m->out[m->out_len++] = ch;
    m->out[m->out_len] = '\0';
}

/* Runs until PC reaches stop_pc (for the keyboard wait loop: with the
   keyboard buffer empty).  Returns 0 if max_instructions ran out first. */
static int run_until(c64 *m, uint16_t stop_pc, uint64_t max_instructions)
{
    uint64_t start = m->cpu.instructions;
    while (m->cpu.pc != stop_pc || (stop_pc == KERNAL_WAIT_FOR_KEY && m->ram[0xC6] != 0)) {
        if (max_instructions && m->cpu.instructions - start >= max_instructions)
            return 0;
        if (m->cpu.pc == KERNAL_CHROUT && m->capturing)
            capture(m, m->cpu.a);
        cpu_step(&m->cpu);
    }
    return 1;
}

void c64_init(c64 *m)
{
    const char *dir = getenv("C64_ROMDIR");
    if (!dir)
        dir = "/opt/homebrew/share/vice/C64";

    memset(m, 0, sizeof *m);
    load_rom(dir, "basic-901226-01.bin", m->basic, sizeof m->basic);
    load_rom(dir, "kernal-901227-03.bin", m->kernal, sizeof m->kernal);
    m->cpu.read = mem_read;
    m->cpu.write = mem_write;
    m->cpu.user = m;

    cpu_reset(&m->cpu);
    if (!run_until(m, KERNAL_WAIT_FOR_KEY, 50000000)) {
        fprintf(stderr, "c64machine: boot did not reach READY.\n");
        exit(2);
    }
}

void c64_call(c64 *m, uint16_t addr, uint8_t a, uint8_t x, uint8_t y)
{
    cpu6502 *c = &m->cpu;
    uint16_t ret = CALL_RETURN_TRAP - 1;   /* RTS adds one */
    c->s = 0xFF;
    m->ram[0x100 + c->s--] = (uint8_t)(ret >> 8);
    m->ram[0x100 + c->s--] = (uint8_t)ret;
    c->a = a; c->x = x; c->y = y;
    c->p = FLAG_U | FLAG_I;
    c->pc = addr;
    if (!run_until(m, CALL_RETURN_TRAP, 100000000)) {
        fprintf(stderr, "c64machine: call to $%04X did not return\n", addr);
        exit(2);
    }
}

const char *c64_run_basic(c64 *m, const uint8_t *prg, size_t len, uint64_t max_instructions)
{
    if (len < 2 || (prg[0] | prg[1] << 8) != 0x0801 || len - 2 > 0x9800) {
        fprintf(stderr, "c64machine: not a BASIC program loading at $0801\n");
        exit(2);
    }
    /* Equivalent of LOAD"...",8: copy the program and point the start of
       variables ($2D/$2E) just past it.  RUN does the CLR. */
    uint16_t end = (uint16_t)(0x0801 + len - 2);
    memcpy(&m->ram[0x0801], prg + 2, len - 2);
    m->ram[0x2D] = (uint8_t)end;
    m->ram[0x2E] = (uint8_t)(end >> 8);

    /* Type RUN<return> into the keyboard buffer. */
    static const uint8_t keys[] = { 'R', 'U', 'N', 13 };
    memcpy(&m->ram[0x0277], keys, sizeof keys);
    m->ram[0xC6] = sizeof keys;

    m->out_len = 0;
    if (m->out)
        m->out[0] = '\0';
    m->capturing = 1;
    int finished = run_until(m, KERNAL_WAIT_FOR_KEY, max_instructions);
    m->capturing = 0;
    if (!finished)
        return NULL;
    return m->out ? m->out : "";
}
