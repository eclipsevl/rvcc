/*
 * test_runner — standalone uvm32 host for testing rvcc output.
 * Loads a flat binary, runs it in mini-rv32ima, handles syscalls.
 *
 * Usage: test_runner <binary.bin>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* VM memory size — generous for tests */
#define MINI_RV32_RAM_SIZE  (64 * 1024)
#define RAM_BASE            0x80000000U

static uint8_t ram[MINI_RV32_RAM_SIZE];

/* Syscall IDs (must match uvm32_target.h / vm_config.h) */
#define SYSCALL_PUTC        0x00000000
#define SYSCALL_GETC        0x00000001
#define SYSCALL_PRINT       0x00000002
#define SYSCALL_PRINTLN     0x00000003
#define SYSCALL_PRINTDEC    0x00000004
#define SYSCALL_PRINTHEX    0x00000005
#define SYSCALL_MILLIS      0x00000006
#define SYSCALL_I2C_WRITE   0x00000010
#define SYSCALL_I2C_READ    0x00000011
#define SYSCALL_YIELD       0x01000001
#define SYSCALL_HALT        0x01000000

/* Configure mini-rv32ima */
#define MINIRV32_DECORATE static
#define MINIRV32_RETURN_TRAP
#define MINIRV32_NO_TIMERS_NO_CYCLES
#define MINIRV32_NO_ZICSR
#define MINIRV32_NO_ATOMICS
#define MINIRV32_NO_BREAKPOINT_NO_INTERRUPTS
#define MINIRV32_POSTEXEC(pc, ir, retval) { if (retval > 0) return retval; }
#define MINIRV32_HANDLE_MEM_LOAD_CONTROL(addy, rval) { rval = 0; }
#define MINIRV32_HANDLE_MEM_STORE_CONTROL(addy, val) { }

#include "mini-rv32ima.h"

/* Read a NUL-terminated string from VM memory */
static const char *vm_get_string(uint32_t addr) {
    if (addr < RAM_BASE) return "(bad addr)";
    uint32_t off = addr - RAM_BASE;
    if (off >= MINI_RV32_RAM_SIZE) return "(bad addr)";
    return (const char *)&ram[off];
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: test_runner <binary.bin>\n");
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > MINI_RV32_RAM_SIZE) {
        fprintf(stderr, "Binary too large (%ld bytes)\n", len);
        fclose(f);
        return 1;
    }
    memset(ram, 0, sizeof(ram));
    fread(ram, 1, len, f);
    fclose(f);

    /* Init CPU state */
    struct MiniRV32IMAState core;
    memset(&core, 0, sizeof(core));
    core.pc = RAM_BASE;
    core.regs[2] = ((RAM_BASE + MINI_RV32_RAM_SIZE) & ~0xFU) - 16; /* sp */
    core.extraflags |= 3; /* machine mode */

    int trace = (getenv("TRACE") != NULL);
    int max_steps = 1000000;
    for (int step = 0; step < max_steps; step++) {
        if (trace)
            fprintf(stderr, "[%04d] pc=%08x ra=%08x sp=%08x a0=%08x\n",
                    step, core.pc, core.regs[1], core.regs[2], core.regs[10]);
        int32_t ret = MiniRV32IMAStep(NULL, &core, ram, 1);
        if (ret == 0) continue;

        if (ret == 12) {
            /* ecall */
            uint32_t syscall_id = core.regs[17]; /* a7 */
            uint32_t a0 = core.regs[10];
            uint32_t a1 = core.regs[11];
            (void)a1;
            core.pc += 4;

            switch (syscall_id) {
            case SYSCALL_HALT:
                return 0;
            case SYSCALL_PUTC:
                putchar((char)a0);
                break;
            case SYSCALL_PRINT:
                printf("%s", vm_get_string(a0));
                break;
            case SYSCALL_PRINTLN:
                printf("%s\n", vm_get_string(a0));
                break;
            case SYSCALL_PRINTDEC:
                printf("%d", (int32_t)a0);
                break;
            case SYSCALL_PRINTHEX:
                printf("%02X", a0);
                break;
            case SYSCALL_MILLIS:
                core.regs[12] = 0; /* a2 = return 0 */
                break;
            case SYSCALL_YIELD:
                break;
            case SYSCALL_I2C_WRITE:
            case SYSCALL_I2C_READ:
                /* Stub: return ACK (0) — simulate successful I2C */
                core.regs[12] = 0;
                break;
            default:
                fprintf(stderr, "Unknown syscall 0x%08x at pc=0x%08x\n",
                        syscall_id, core.pc - 4);
                return 2;
            }
        } else {
            fprintf(stderr, "CPU trap %d at pc=0x%08x\n", ret, core.pc);
            return 2;
        }
    }

    fprintf(stderr, "Exceeded max steps (%d)\n", max_steps);
    return 3;
}
