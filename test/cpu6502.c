#include "cpu6502.h"

#include <stdio.h>
#include <stdlib.h>

#define RD(addr)      c->read(c, (uint16_t)(addr))
#define WR(addr, v)   c->write(c, (uint16_t)(addr), (uint8_t)(v))

static void set_nz(cpu6502 *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~(FLAG_N | FLAG_Z)) | (v & 0x80) | (v ? 0 : FLAG_Z));
}

static void set_flag(cpu6502 *c, uint8_t flag, int on)
{
    c->p = on ? (c->p | flag) : (c->p & ~flag);
}

static uint16_t rd16(cpu6502 *c, uint16_t addr)
{
    return (uint16_t)(RD(addr) | RD(addr + 1) << 8);
}

/* (zp) pointer read wraps within the zero page, as on the real CPU. */
static uint16_t rd16_zp(cpu6502 *c, uint8_t zp)
{
    return (uint16_t)(RD(zp) | RD((uint8_t)(zp + 1)) << 8);
}

static void push(cpu6502 *c, uint8_t v) { WR(0x100 + c->s, v); c->s--; }
static uint8_t pull(cpu6502 *c)         { c->s++; return RD(0x100 + c->s); }

static void adc(cpu6502 *c, uint8_t m)
{
    if (c->p & FLAG_D) {
        fprintf(stderr, "cpu6502: decimal mode ADC at $%04X not supported\n", c->pc);
        exit(2);
    }
    unsigned sum = c->a + m + (c->p & FLAG_C);
    set_flag(c, FLAG_V, (~(c->a ^ m) & (c->a ^ sum) & 0x80) != 0);
    set_flag(c, FLAG_C, sum > 0xFF);
    c->a = (uint8_t)sum;
    set_nz(c, c->a);
}

static void sbc(cpu6502 *c, uint8_t m)
{
    if (c->p & FLAG_D) {
        fprintf(stderr, "cpu6502: decimal mode SBC at $%04X not supported\n", c->pc);
        exit(2);
    }
    adc(c, (uint8_t)~m);   /* binary SBC is ADC of the complement */
}

static void cmp(cpu6502 *c, uint8_t reg, uint8_t m)
{
    set_flag(c, FLAG_C, reg >= m);
    set_nz(c, (uint8_t)(reg - m));
}

static void branch(cpu6502 *c, int cond)
{
    int8_t off = (int8_t)RD(c->pc++);
    if (cond)
        c->pc = (uint16_t)(c->pc + off);
}

void cpu_reset(cpu6502 *c)
{
    c->a = c->x = c->y = 0;
    c->s = 0xFD;
    c->p = FLAG_U | FLAG_I;
    c->pc = rd16(c, 0xFFFC);
    c->instructions = 0;
}

void cpu_step(cpu6502 *c)
{
    uint16_t at = c->pc;
    uint8_t op = RD(c->pc++);
    uint16_t ea = 0;
    c->instructions++;

    /* Effective address for the memory-operand addressing modes.
       Opcode layout: aaabbbcc, bbb selects the mode within groups cc=01/10/00. */
    switch (op) {
    /* immediate */
    case 0x09: case 0x29: case 0x49: case 0x69: case 0xA9: case 0xC9: case 0xE9:
    case 0xA0: case 0xA2: case 0xC0: case 0xE0:
        ea = c->pc++; break;
    /* zero page */
    case 0x05: case 0x06: case 0x24: case 0x25: case 0x26: case 0x45: case 0x46:
    case 0x65: case 0x66: case 0x84: case 0x85: case 0x86: case 0xA4: case 0xA5:
    case 0xA6: case 0xC4: case 0xC5: case 0xC6: case 0xE4: case 0xE5: case 0xE6:
        ea = RD(c->pc++); break;
    /* zero page,X */
    case 0x15: case 0x16: case 0x35: case 0x36: case 0x55: case 0x56: case 0x75:
    case 0x76: case 0x94: case 0x95: case 0xB4: case 0xB5: case 0xD5: case 0xD6:
    case 0xF5: case 0xF6:
        ea = (uint8_t)(RD(c->pc++) + c->x); break;
    /* zero page,Y */
    case 0x96: case 0xB6:
        ea = (uint8_t)(RD(c->pc++) + c->y); break;
    /* absolute */
    case 0x0D: case 0x0E: case 0x20: case 0x2C: case 0x2D: case 0x2E: case 0x4C:
    case 0x4D: case 0x4E: case 0x6D: case 0x6E: case 0x8C: case 0x8D: case 0x8E:
    case 0xAC: case 0xAD: case 0xAE: case 0xCC: case 0xCD: case 0xCE: case 0xEC:
    case 0xED: case 0xEE:
        ea = rd16(c, c->pc); c->pc += 2; break;
    /* absolute,X */
    case 0x1D: case 0x1E: case 0x3D: case 0x3E: case 0x5D: case 0x5E: case 0x7D:
    case 0x7E: case 0x9D: case 0xBC: case 0xBD: case 0xDD: case 0xDE: case 0xFD:
    case 0xFE:
        ea = (uint16_t)(rd16(c, c->pc) + c->x); c->pc += 2; break;
    /* absolute,Y */
    case 0x19: case 0x39: case 0x59: case 0x79: case 0x99: case 0xB9: case 0xBE:
    case 0xD9: case 0xF9:
        ea = (uint16_t)(rd16(c, c->pc) + c->y); c->pc += 2; break;
    /* (zp,X) */
    case 0x01: case 0x21: case 0x41: case 0x61: case 0x81: case 0xA1: case 0xC1:
    case 0xE1:
        ea = rd16_zp(c, (uint8_t)(RD(c->pc++) + c->x)); break;
    /* (zp),Y */
    case 0x11: case 0x31: case 0x51: case 0x71: case 0x91: case 0xB1: case 0xD1:
    case 0xF1:
        ea = (uint16_t)(rd16_zp(c, RD(c->pc++)) + c->y); break;
    default:
        break;
    }

    uint8_t m, r;
    switch (op) {
    /* loads and stores */
    case 0xA9: case 0xA5: case 0xB5: case 0xAD: case 0xBD: case 0xB9: case 0xA1: case 0xB1:
        c->a = RD(ea); set_nz(c, c->a); break;
    case 0xA2: case 0xA6: case 0xB6: case 0xAE: case 0xBE:
        c->x = RD(ea); set_nz(c, c->x); break;
    case 0xA0: case 0xA4: case 0xB4: case 0xAC: case 0xBC:
        c->y = RD(ea); set_nz(c, c->y); break;
    case 0x85: case 0x95: case 0x8D: case 0x9D: case 0x99: case 0x81: case 0x91:
        WR(ea, c->a); break;
    case 0x86: case 0x96: case 0x8E:
        WR(ea, c->x); break;
    case 0x84: case 0x94: case 0x8C:
        WR(ea, c->y); break;

    /* arithmetic and logic */
    case 0x69: case 0x65: case 0x75: case 0x6D: case 0x7D: case 0x79: case 0x61: case 0x71:
        adc(c, RD(ea)); break;
    case 0xE9: case 0xE5: case 0xF5: case 0xED: case 0xFD: case 0xF9: case 0xE1: case 0xF1:
        sbc(c, RD(ea)); break;
    case 0x29: case 0x25: case 0x35: case 0x2D: case 0x3D: case 0x39: case 0x21: case 0x31:
        c->a &= RD(ea); set_nz(c, c->a); break;
    case 0x09: case 0x05: case 0x15: case 0x0D: case 0x1D: case 0x19: case 0x01: case 0x11:
        c->a |= RD(ea); set_nz(c, c->a); break;
    case 0x49: case 0x45: case 0x55: case 0x4D: case 0x5D: case 0x59: case 0x41: case 0x51:
        c->a ^= RD(ea); set_nz(c, c->a); break;
    case 0xC9: case 0xC5: case 0xD5: case 0xCD: case 0xDD: case 0xD9: case 0xC1: case 0xD1:
        cmp(c, c->a, RD(ea)); break;
    case 0xE0: case 0xE4: case 0xEC:
        cmp(c, c->x, RD(ea)); break;
    case 0xC0: case 0xC4: case 0xCC:
        cmp(c, c->y, RD(ea)); break;
    case 0x24: case 0x2C:
        m = RD(ea);
        set_flag(c, FLAG_Z, (c->a & m) == 0);
        c->p = (uint8_t)((c->p & ~(FLAG_N | FLAG_V)) | (m & (FLAG_N | FLAG_V)));
        break;

    /* read-modify-write */
    case 0x0A: set_flag(c, FLAG_C, c->a & 0x80); c->a <<= 1; set_nz(c, c->a); break;
    case 0x4A: set_flag(c, FLAG_C, c->a & 0x01); c->a >>= 1; set_nz(c, c->a); break;
    case 0x2A: r = (uint8_t)(c->a << 1 | (c->p & FLAG_C));
               set_flag(c, FLAG_C, c->a & 0x80); c->a = r; set_nz(c, r); break;
    case 0x6A: r = (uint8_t)(c->a >> 1 | (c->p & FLAG_C) << 7);
               set_flag(c, FLAG_C, c->a & 0x01); c->a = r; set_nz(c, r); break;
    case 0x06: case 0x16: case 0x0E: case 0x1E:
        m = RD(ea); set_flag(c, FLAG_C, m & 0x80); r = (uint8_t)(m << 1);
        WR(ea, r); set_nz(c, r); break;
    case 0x46: case 0x56: case 0x4E: case 0x5E:
        m = RD(ea); set_flag(c, FLAG_C, m & 0x01); r = m >> 1;
        WR(ea, r); set_nz(c, r); break;
    case 0x26: case 0x36: case 0x2E: case 0x3E:
        m = RD(ea); r = (uint8_t)(m << 1 | (c->p & FLAG_C)); set_flag(c, FLAG_C, m & 0x80);
        WR(ea, r); set_nz(c, r); break;
    case 0x66: case 0x76: case 0x6E: case 0x7E:
        m = RD(ea); r = (uint8_t)(m >> 1 | (c->p & FLAG_C) << 7); set_flag(c, FLAG_C, m & 0x01);
        WR(ea, r); set_nz(c, r); break;
    case 0xE6: case 0xF6: case 0xEE: case 0xFE:
        r = (uint8_t)(RD(ea) + 1); WR(ea, r); set_nz(c, r); break;
    case 0xC6: case 0xD6: case 0xCE: case 0xDE:
        r = (uint8_t)(RD(ea) - 1); WR(ea, r); set_nz(c, r); break;

    /* register transfers, increments */
    case 0xAA: c->x = c->a; set_nz(c, c->x); break;
    case 0x8A: c->a = c->x; set_nz(c, c->a); break;
    case 0xA8: c->y = c->a; set_nz(c, c->y); break;
    case 0x98: c->a = c->y; set_nz(c, c->a); break;
    case 0xBA: c->x = c->s; set_nz(c, c->x); break;
    case 0x9A: c->s = c->x; break;
    case 0xE8: c->x++; set_nz(c, c->x); break;
    case 0xCA: c->x--; set_nz(c, c->x); break;
    case 0xC8: c->y++; set_nz(c, c->y); break;
    case 0x88: c->y--; set_nz(c, c->y); break;

    /* stack */
    case 0x48: push(c, c->a); break;
    case 0x68: c->a = pull(c); set_nz(c, c->a); break;
    case 0x08: push(c, c->p | FLAG_B | FLAG_U); break;
    case 0x28: c->p = (uint8_t)((pull(c) & ~FLAG_B) | FLAG_U); break;

    /* flags */
    case 0x18: c->p &= ~FLAG_C; break;
    case 0x38: c->p |= FLAG_C; break;
    case 0x58: c->p &= ~FLAG_I; break;
    case 0x78: c->p |= FLAG_I; break;
    case 0xB8: c->p &= ~FLAG_V; break;
    case 0xD8: c->p &= ~FLAG_D; break;
    case 0xF8: c->p |= FLAG_D; break;

    /* branches */
    case 0x10: branch(c, !(c->p & FLAG_N)); break;
    case 0x30: branch(c,  (c->p & FLAG_N)); break;
    case 0x50: branch(c, !(c->p & FLAG_V)); break;
    case 0x70: branch(c,  (c->p & FLAG_V)); break;
    case 0x90: branch(c, !(c->p & FLAG_C)); break;
    case 0xB0: branch(c,  (c->p & FLAG_C)); break;
    case 0xD0: branch(c, !(c->p & FLAG_Z)); break;
    case 0xF0: branch(c,  (c->p & FLAG_Z)); break;

    /* jumps and subroutines */
    case 0x4C: c->pc = ea; break;
    case 0x6C: {
        uint16_t ptr = rd16(c, c->pc);
        /* NMOS bug: the high byte is fetched without carrying into the page. */
        c->pc = (uint16_t)(RD(ptr) | RD((ptr & 0xFF00) | ((ptr + 1) & 0xFF)) << 8);
        break;
    }
    case 0x20:
        push(c, (uint8_t)((c->pc - 1) >> 8));
        push(c, (uint8_t)(c->pc - 1));
        c->pc = ea;
        break;
    case 0x60: {
        uint8_t lo = pull(c), hi = pull(c);   /* separate statements: C leaves operand order unspecified */
        c->pc = (uint16_t)((lo | hi << 8) + 1);
        break;
    }
    case 0x40: {
        c->p = (uint8_t)((pull(c) & ~FLAG_B) | FLAG_U);
        uint8_t lo = pull(c), hi = pull(c);
        c->pc = (uint16_t)(lo | hi << 8);
        break;
    }
    case 0x00:
        c->pc++;
        push(c, (uint8_t)(c->pc >> 8)); push(c, (uint8_t)c->pc);
        push(c, c->p | FLAG_B | FLAG_U);
        c->p |= FLAG_I;
        c->pc = rd16(c, 0xFFFE);
        break;
    case 0xEA: break;

    default:
        fprintf(stderr, "cpu6502: illegal opcode $%02X at $%04X\n", op, at);
        exit(2);
    }
}
