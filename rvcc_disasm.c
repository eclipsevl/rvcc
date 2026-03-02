/*
 * rvcc_disasm.c — Standalone RV32IM disassembler
 * Usage: rvcc_disasm [-b base_addr] input.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static const char *reg_name[32] = {
    "zero","ra","sp","gp","tp","t0","t1","t2",
    "s0","s1","a0","a1","a2","a3","a4","a5",
    "a6","a7","s2","s3","s4","s5","s6","s7",
    "s8","s9","s10","s11","t3","t4","t5","t6"
};

/* Sign-extend a value of 'bits' width */
static int32_t sign_ext(uint32_t val, int bits)
{
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((val ^ m) - m);
}

/* Extract fields */
static uint32_t opcode(uint32_t w)  { return w & 0x7f; }
static uint32_t rd(uint32_t w)      { return (w >> 7) & 0x1f; }
static uint32_t funct3(uint32_t w)  { return (w >> 12) & 0x7; }
static uint32_t rs1(uint32_t w)     { return (w >> 15) & 0x1f; }
static uint32_t rs2(uint32_t w)     { return (w >> 20) & 0x1f; }
static uint32_t funct7(uint32_t w)  { return (w >> 25) & 0x7f; }

static int32_t imm_i(uint32_t w)
{
    return sign_ext(w >> 20, 12);
}

static int32_t imm_s(uint32_t w)
{
    uint32_t v = ((w >> 7) & 0x1f) | (((w >> 25) & 0x7f) << 5);
    return sign_ext(v, 12);
}

static int32_t imm_b(uint32_t w)
{
    uint32_t v = (((w >> 8) & 0xf) << 1)
               | (((w >> 25) & 0x3f) << 5)
               | (((w >> 7) & 1) << 11)
               | (((w >> 31) & 1) << 12);
    return sign_ext(v, 13);
}

static int32_t imm_u(uint32_t w)
{
    return (int32_t)(w & 0xfffff000u);
}

static int32_t imm_j(uint32_t w)
{
    uint32_t v = (((w >> 21) & 0x3ff) << 1)
               | (((w >> 20) & 1) << 11)
               | (((w >> 12) & 0xff) << 12)
               | (((w >> 31) & 1) << 20);
    return sign_ext(v, 21);
}

static void disasm_r(uint32_t w, uint32_t addr, char *buf, size_t sz)
{
    uint32_t f3 = funct3(w), f7 = funct7(w);
    const char *mn = "???";

    if (f7 == 0x01) {
        /* M extension */
        static const char *m_ops[] = {
            "mul","mulh","mulhsu","mulhu","div","divu","rem","remu"
        };
        mn = m_ops[f3];
    } else {
        switch (f3) {
        case 0: mn = (f7 == 0x20) ? "sub" : "add"; break;
        case 1: mn = "sll"; break;
        case 2: mn = "slt"; break;
        case 3: mn = "sltu"; break;
        case 4: mn = "xor"; break;
        case 5: mn = (f7 == 0x20) ? "sra" : "srl"; break;
        case 6: mn = "or"; break;
        case 7: mn = "and"; break;
        }
    }
    snprintf(buf, sz, "%-7s %s, %s, %s", mn,
             reg_name[rd(w)], reg_name[rs1(w)], reg_name[rs2(w)]);
    (void)addr;
}

static void disasm_i_alu(uint32_t w, uint32_t addr, char *buf, size_t sz)
{
    uint32_t d = rd(w), s = rs1(w), f3 = funct3(w);
    int32_t imm = imm_i(w);

    /* Pseudo-ops */
    if (f3 == 0 && s == 0 && d == 0 && imm == 0) {
        snprintf(buf, sz, "nop"); return;
    }
    if (f3 == 0 && s == 0) {
        snprintf(buf, sz, "%-7s %s, %d", "li", reg_name[d], imm); return;
    }
    if (f3 == 0 && imm == 0) {
        snprintf(buf, sz, "%-7s %s, %s", "mv", reg_name[d], reg_name[s]); return;
    }

    const char *mn;
    switch (f3) {
    case 0: mn = "addi"; break;
    case 1: mn = "slli"; break;
    case 2: mn = "slti"; break;
    case 3: mn = "sltiu"; break;
    case 4: mn = "xori"; break;
    case 5: mn = (funct7(w) == 0x20) ? "srai" : "srli"; break;
    case 6: mn = "ori"; break;
    case 7: mn = "andi"; break;
    default: mn = "???"; break;
    }

    /* Shift immediates use only lower 5 bits */
    if (f3 == 1 || f3 == 5) imm &= 0x1f;

    snprintf(buf, sz, "%-7s %s, %s, %d", mn,
             reg_name[d], reg_name[s], imm);
    (void)addr;
}

static void disasm_load(uint32_t w, uint32_t addr, char *buf, size_t sz)
{
    static const char *ops[] = {"lb","lh","lw","???","lbu","lhu","???","???"};
    int32_t imm = imm_i(w);
    snprintf(buf, sz, "%-7s %s, %d(%s)", ops[funct3(w)],
             reg_name[rd(w)], imm, reg_name[rs1(w)]);
    (void)addr;
}

static void disasm_store(uint32_t w, uint32_t addr, char *buf, size_t sz)
{
    static const char *ops[] = {"sb","sh","sw","???","???","???","???","???"};
    int32_t imm = imm_s(w);
    snprintf(buf, sz, "%-7s %s, %d(%s)", ops[funct3(w)],
             reg_name[rs2(w)], imm, reg_name[rs1(w)]);
    (void)addr;
}

static void disasm_branch(uint32_t w, uint32_t addr, char *buf, size_t sz)
{
    static const char *ops[] = {"beq","bne","???","???","blt","bge","bltu","bgeu"};
    int32_t off = imm_b(w);
    uint32_t target = addr + (uint32_t)off;
    snprintf(buf, sz, "%-7s %s, %s, 0x%08x", ops[funct3(w)],
             reg_name[rs1(w)], reg_name[rs2(w)], target);
}

static void disasm_jal(uint32_t w, uint32_t addr, char *buf, size_t sz)
{
    uint32_t d = rd(w);
    int32_t off = imm_j(w);
    uint32_t target = addr + (uint32_t)off;

    if (d == 0) {
        snprintf(buf, sz, "%-7s 0x%08x", "j", target);
    } else if (d == 1) {
        snprintf(buf, sz, "%-7s 0x%08x", "call", target);
    } else {
        snprintf(buf, sz, "%-7s %s, 0x%08x", "jal", reg_name[d], target);
    }
}

static void disasm_jalr(uint32_t w, uint32_t addr, char *buf, size_t sz)
{
    uint32_t d = rd(w), s = rs1(w);
    int32_t imm = imm_i(w);

    if (d == 0 && s == 1 && imm == 0) {
        snprintf(buf, sz, "ret"); return;
    }
    snprintf(buf, sz, "%-7s %s, %s, %d", "jalr",
             reg_name[d], reg_name[s], imm);
    (void)addr;
}

static void disasm_lui(uint32_t w, uint32_t addr, char *buf, size_t sz)
{
    snprintf(buf, sz, "%-7s %s, 0x%x", "lui",
             reg_name[rd(w)], (uint32_t)imm_u(w) >> 12);
    (void)addr;
}

static void disasm_auipc(uint32_t w, uint32_t addr, char *buf, size_t sz)
{
    snprintf(buf, sz, "%-7s %s, 0x%x", "auipc",
             reg_name[rd(w)], (uint32_t)imm_u(w) >> 12);
    (void)addr;
}

static void disasm_system(uint32_t w, uint32_t addr, char *buf, size_t sz)
{
    uint32_t imm = w >> 20;
    if (imm == 0)
        snprintf(buf, sz, "ecall");
    else if (imm == 1)
        snprintf(buf, sz, "ebreak");
    else
        snprintf(buf, sz, "system  0x%x", imm);
    (void)addr;
}

static void disasm_one(uint32_t w, uint32_t addr, char *buf, size_t sz)
{
    switch (opcode(w)) {
    case 0x33: disasm_r(w, addr, buf, sz); break;
    case 0x13: disasm_i_alu(w, addr, buf, sz); break;
    case 0x03: disasm_load(w, addr, buf, sz); break;
    case 0x23: disasm_store(w, addr, buf, sz); break;
    case 0x63: disasm_branch(w, addr, buf, sz); break;
    case 0x6F: disasm_jal(w, addr, buf, sz); break;
    case 0x67: disasm_jalr(w, addr, buf, sz); break;
    case 0x37: disasm_lui(w, addr, buf, sz); break;
    case 0x17: disasm_auipc(w, addr, buf, sz); break;
    case 0x73: disasm_system(w, addr, buf, sz); break;
    default:
        snprintf(buf, sz, ".word   0x%08x", w);
        break;
    }
}

/* Heuristic: is this likely a valid instruction? */
static int is_instruction(uint32_t w)
{
    uint32_t op = opcode(w);
    return op == 0x33 || op == 0x13 || op == 0x03 || op == 0x23 ||
           op == 0x63 || op == 0x6F || op == 0x67 || op == 0x37 ||
           op == 0x17 || op == 0x73;
}

/* Is this a terminal instruction (ret, ecall, unconditional jump to x0)? */
static int is_terminal(uint32_t w)
{
    /* ret = jalr x0, ra, 0 */
    if (w == 0x00008067) return 1;
    /* ecall */
    if (w == 0x00000073) return 1;
    /* j (jal x0, ...) — unconditional, no link */
    if (opcode(w) == 0x6F && rd(w) == 0) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t base = 0x80000000u;
    const char *infile = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            base = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (argv[i][0] != '-') {
            infile = argv[i];
        } else {
            fprintf(stderr, "Usage: rvcc_disasm [-b base_addr] input.bin\n");
            return 1;
        }
    }

    if (!infile) {
        fprintf(stderr, "Usage: rvcc_disasm [-b base_addr] input.bin\n");
        return 1;
    }

    FILE *f = fopen(infile, "rb");
    if (!f) {
        perror(infile);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 16 * 1024 * 1024) {
        fprintf(stderr, "Invalid file size: %ld\n", fsize);
        fclose(f);
        return 1;
    }

    uint8_t *data = malloc((size_t)fsize);
    if (!data) {
        fprintf(stderr, "Out of memory\n");
        fclose(f);
        return 1;
    }
    if (fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
        fprintf(stderr, "Read error\n");
        free(data);
        fclose(f);
        return 1;
    }
    fclose(f);

    /* Find where code ends and data begins.
     * After a terminal instruction (ret/ecall/j), the first non-instruction
     * marks the start of the data section. */
    size_t code_end = (size_t)fsize;
    int saw_terminal = 0;
    for (size_t off = 0; off + 3 < (size_t)fsize; off += 4) {
        uint32_t w = (uint32_t)data[off]
                   | ((uint32_t)data[off+1] << 8)
                   | ((uint32_t)data[off+2] << 16)
                   | ((uint32_t)data[off+3] << 24);
        if (saw_terminal && !is_instruction(w)) {
            code_end = off;
            break;
        }
        saw_terminal = is_terminal(w);
    }

    /* Disassemble code section */
    char buf[128];
    for (size_t off = 0; off + 3 < code_end; off += 4) {
        uint32_t w = (uint32_t)data[off]
                   | ((uint32_t)data[off+1] << 8)
                   | ((uint32_t)data[off+2] << 16)
                   | ((uint32_t)data[off+3] << 24);
        uint32_t addr = base + (uint32_t)off;
        disasm_one(w, addr, buf, sizeof(buf));
        printf("%08x:  %08x  %s\n", addr, w, buf);
    }

    /* Data section */
    if (code_end < (size_t)fsize) {
        printf("\n; --- data section ---\n");
        size_t off = code_end;
        /* Word-aligned dump */
        for (; off + 3 < (size_t)fsize; off += 4) {
            uint32_t w = (uint32_t)data[off]
                       | ((uint32_t)data[off+1] << 8)
                       | ((uint32_t)data[off+2] << 16)
                       | ((uint32_t)data[off+3] << 24);
            uint32_t addr = base + (uint32_t)off;
            /* Try to show printable ASCII */
            char ascii[5];
            for (int i = 0; i < 4; i++) {
                uint8_t c = data[off + i];
                ascii[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
            }
            ascii[4] = '\0';
            printf("%08x:  %08x  .word   0x%08x  ; \"%s\"\n",
                   addr, w, w, ascii);
        }
        /* Remaining bytes */
        for (; off < (size_t)fsize; off++) {
            uint32_t addr = base + (uint32_t)off;
            uint8_t b = data[off];
            char c = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
            printf("%08x:  %02x        .byte   0x%02x      ; '%c'\n",
                   addr, b, b, c);
        }
    }

    free(data);
    return 0;
}
