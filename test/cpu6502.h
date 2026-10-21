/*
 * Minimal NMOS 6502 interpreter, used only as a test oracle: it runs the
 * real C64 BASIC/KERNAL ROM code so the hand-written C port can be checked
 * against it bit for bit.  Documented opcodes only; decimal mode is not
 * implemented (neither BASIC's float code nor the KERNAL paths we use need it).
 */
#ifndef CPU6502_H
#define CPU6502_H

#include <stdint.h>

typedef struct cpu6502 {
    uint16_t pc;
    uint8_t  a, x, y, s;
    uint8_t  p;               /* NV-BDIZC, bit 5 always reads as 1 */
    uint64_t instructions;    /* executed so far */

    /* Memory interface, supplied by the machine. */
    uint8_t (*read)(struct cpu6502 *, uint16_t addr);
    void    (*write)(struct cpu6502 *, uint16_t addr, uint8_t val);
    void     *user;
} cpu6502;

enum { FLAG_C = 0x01, FLAG_Z = 0x02, FLAG_I = 0x04, FLAG_D = 0x08,
       FLAG_B = 0x10, FLAG_U = 0x20, FLAG_V = 0x40, FLAG_N = 0x80 };

void cpu_reset(cpu6502 *c);   /* loads PC from the reset vector at $FFFC */
void cpu_step(cpu6502 *c);    /* executes one instruction; aborts on an illegal opcode */

#endif
