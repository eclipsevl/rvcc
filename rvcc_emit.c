/*
 * rvcc_emit.c — emit buffer and RV32IM instruction encoding
 */
#include "rvcc_internal.h"

/* ── Emit buffer ───────────────────────────────────────────────── */

void emit_init(compiler_t *c)
{
    c->code_cap = EMIT_INIT_CAP;
    c->code_len = 0;
    c->code = (uint8_t *)malloc(c->code_cap);
    if (!c->code) {
        compile_error(c, "out of memory");
    }
}

static void emit_grow(compiler_t *c, uint32_t need)
{
    while (c->code_len + need > c->code_cap) {
        c->code_cap *= 2;
        uint8_t *p = (uint8_t *)realloc(c->code, c->code_cap);
        if (!p) {
            compile_error(c, "out of memory");
            return;
        }
        c->code = p;
    }
}

void emit8(compiler_t *c, uint8_t val)
{
    emit_grow(c, 1);
    c->code[c->code_len++] = val;
}

void emit32(compiler_t *c, uint32_t val)
{
    emit_grow(c, 4);
    c->code[c->code_len++] = (uint8_t)(val);
    c->code[c->code_len++] = (uint8_t)(val >> 8);
    c->code[c->code_len++] = (uint8_t)(val >> 16);
    c->code[c->code_len++] = (uint8_t)(val >> 24);
}

void patch32(compiler_t *c, uint32_t offset, uint32_t val)
{
    if (offset + 4 > c->code_len) {
        compile_error(c, "patch32: offset out of range");
        return;
    }
    c->code[offset + 0] = (uint8_t)(val);
    c->code[offset + 1] = (uint8_t)(val >> 8);
    c->code[offset + 2] = (uint8_t)(val >> 16);
    c->code[offset + 3] = (uint8_t)(val >> 24);
}

uint32_t emit_cur(compiler_t *c)
{
    return c->code_len;
}

/* ── RV32IM instruction format encoders ────────────────────────── */

/*
 * R-type: funct7[31:25] | rs2[24:20] | rs1[19:15] | funct3[14:12] | rd[11:7] | opcode[6:0]
 */
uint32_t rv_r(int op, int rd, int f3, int rs1, int rs2, int f7)
{
    return (uint32_t)(
        ((f7  & 0x7F) << 25) |
        ((rs2 & 0x1F) << 20) |
        ((rs1 & 0x1F) << 15) |
        ((f3  & 0x07) << 12) |
        ((rd  & 0x1F) <<  7) |
        (op   & 0x7F)
    );
}

/*
 * I-type: imm[11:0] at [31:20] | rs1[19:15] | funct3[14:12] | rd[11:7] | opcode[6:0]
 */
uint32_t rv_i(int op, int rd, int f3, int rs1, int imm)
{
    return (uint32_t)(
        ((imm & 0xFFF) << 20) |
        ((rs1 & 0x1F)  << 15) |
        ((f3  & 0x07)  << 12) |
        ((rd  & 0x1F)  <<  7) |
        (op   & 0x7F)
    );
}

/*
 * S-type: imm[11:5] at [31:25] | rs2[24:20] | rs1[19:15] | funct3[14:12] | imm[4:0] at [11:7] | opcode[6:0]
 */
uint32_t rv_s(int op, int f3, int rs1, int rs2, int imm)
{
    return (uint32_t)(
        (((imm >> 5) & 0x7F) << 25) |
        ((rs2 & 0x1F)        << 20) |
        ((rs1 & 0x1F)        << 15) |
        ((f3  & 0x07)        << 12) |
        ((imm & 0x1F)        <<  7) |
        (op   & 0x7F)
    );
}

/*
 * B-type: imm[12|10:5] at [31:25] | rs2[24:20] | rs1[19:15] | funct3[14:12] | imm[4:1|11] at [11:7] | opcode[6:0]
 *
 * The immediate encodes a signed offset in multiples of 2.
 * Bit layout of the immediate in the instruction:
 *   bit 31    = imm[12]
 *   bits 30:25 = imm[10:5]
 *   bits 11:8  = imm[4:1]
 *   bit 7      = imm[11]
 */
uint32_t rv_b(int op, int f3, int rs1, int rs2, int imm)
{
    uint32_t v = (uint32_t)imm;
    return (uint32_t)(
        (((v >> 12) & 0x1)  << 31) |
        (((v >>  5) & 0x3F) << 25) |
        ((rs2 & 0x1F)       << 20) |
        ((rs1 & 0x1F)       << 15) |
        ((f3  & 0x07)       << 12) |
        (((v >>  1) & 0x0F) <<  8) |
        (((v >> 11) & 0x1)  <<  7) |
        (op   & 0x7F)
    );
}

/*
 * U-type: imm[31:12] | rd[11:7] | opcode[6:0]
 *
 * The caller passes the full upper 20-bit value (already shifted or not);
 * we place imm[31:12] into the instruction's upper 20 bits.
 */
uint32_t rv_u(int op, int rd, int imm)
{
    return (((uint32_t)imm & 0xFFFFF000U) |
            (((uint32_t)rd & 0x1F) << 7) |
            ((uint32_t)op & 0x7F));
}

/*
 * J-type: imm[20|10:1|11|19:12] at [31:12] | rd[11:7] | opcode[6:0]
 *
 * The immediate encodes a signed offset in multiples of 2.
 * Bit layout in instruction:
 *   bit 31     = imm[20]
 *   bits 30:21 = imm[10:1]
 *   bit 20     = imm[11]
 *   bits 19:12 = imm[19:12]
 */
uint32_t rv_j(int op, int rd, int imm)
{
    uint32_t v = (uint32_t)imm;
    return (uint32_t)(
        (((v >> 20) & 0x1)   << 31) |
        (((v >>  1) & 0x3FF) << 21) |
        (((v >> 11) & 0x1)   << 20) |
        (((v >> 12) & 0xFF)  << 12) |
        ((rd & 0x1F)         <<  7) |
        (op  & 0x7F)
    );
}

/* ── High-level emit helpers ───────────────────────────────────── */

void emit_nop(compiler_t *c)
{
    /* addi x0, x0, 0 — opcode=0x13, funct3=0 */
    emit32(c, rv_i(0x13, REG_ZERO, 0, REG_ZERO, 0));
}

/*
 * Load a 32-bit immediate into register rd.
 *
 * If the value fits in 12-bit signed (-2048..2047), use a single addi.
 * Otherwise, split into lui (upper 20 bits) + addi (lower 12 bits).
 * Because addi sign-extends, if bit 11 of the lower half is set,
 * we must add 1 to the upper portion to compensate.
 */
void emit_li(compiler_t *c, int rd, int32_t imm)
{
    if (imm >= -2048 && imm <= 2047) {
        /* addi rd, x0, imm */
        emit32(c, rv_i(0x13, rd, 0, REG_ZERO, imm));
    } else {
        int32_t upper = imm >> 12;
        int32_t lower = imm & 0xFFF;

        /* sign-extend compensation: if lower 12 bits will be treated
         * as negative by addi, bump upper by 1 */
        if (lower & 0x800)
            upper += 1;

        emit32(c, rv_u(0x37, rd, upper << 12));     /* lui rd, upper */
        emit32(c, rv_i(0x13, rd, 0, rd, lower));    /* addi rd, rd, lower */
    }
}

void emit_mv(compiler_t *c, int rd, int rs)
{
    /* addi rd, rs, 0 */
    emit32(c, rv_i(0x13, rd, 0, rs, 0));
}

void emit_push(compiler_t *c, int reg)
{
    /* addi sp, sp, -4 */
    emit32(c, rv_i(0x13, REG_SP, 0, REG_SP, -4));
    /* sw reg, 0(sp) — S-type: opcode=0x23, funct3=2 */
    emit32(c, rv_s(0x23, 2, REG_SP, reg, 0));
}

void emit_pop(compiler_t *c, int reg)
{
    /* lw reg, 0(sp) — I-type load: opcode=0x03, funct3=2 */
    emit32(c, rv_i(0x03, reg, 2, REG_SP, 0));
    /* addi sp, sp, 4 */
    emit32(c, rv_i(0x13, REG_SP, 0, REG_SP, 4));
}

void emit_ret(compiler_t *c)
{
    /* jalr x0, ra, 0 — opcode=0x67, funct3=0 */
    emit32(c, rv_i(0x67, REG_ZERO, 0, REG_RA, 0));
}

/*
 * Emit a load instruction.
 *   rd   = destination register
 *   rs   = base register
 *   off  = offset from base
 *   size = 1, 2, or 4
 *   is_unsigned = nonzero for lbu/lhu
 *
 * Load opcode = 0x03
 *   funct3: lb=0, lh=1, lw=2, lbu=4, lhu=5
 */
void emit_load(compiler_t *c, int rd, int rs, int off, int size, int is_unsigned)
{
    int f3;
    switch (size) {
    case 1:  f3 = is_unsigned ? 4 : 0; break;
    case 2:  f3 = is_unsigned ? 5 : 1; break;
    default: f3 = 2; break;  /* 4-byte: lw */
    }
    emit32(c, rv_i(0x03, rd, f3, rs, off));
}

/*
 * Emit a store instruction.
 *   rs2  = source register (value to store)
 *   rs1  = base register
 *   off  = offset from base
 *   size = 1, 2, or 4
 *
 * Store opcode = 0x23
 *   funct3: sb=0, sh=1, sw=2
 */
void emit_store(compiler_t *c, int rs2, int rs1, int off, int size)
{
    int f3;
    switch (size) {
    case 1:  f3 = 0; break;
    case 2:  f3 = 1; break;
    default: f3 = 2; break;  /* 4-byte: sw */
    }
    emit32(c, rv_s(0x23, f3, rs1, rs2, off));
}

void emit_add(compiler_t *c, int rd, int rs1, int rs2)
{
    /* add: opcode=0x33, funct3=0, funct7=0 */
    emit32(c, rv_r(0x33, rd, 0, rs1, rs2, 0));
}

void emit_sub(compiler_t *c, int rd, int rs1, int rs2)
{
    /* sub: opcode=0x33, funct3=0, funct7=0x20 */
    emit32(c, rv_r(0x33, rd, 0, rs1, rs2, 0x20));
}

void emit_jal(compiler_t *c, int rd, int imm)
{
    /* jal: opcode=0x6F */
    emit32(c, rv_j(0x6F, rd, imm));
}

void emit_jalr(compiler_t *c, int rd, int rs, int imm)
{
    /* jalr: opcode=0x67, funct3=0 */
    emit32(c, rv_i(0x67, rd, 0, rs, imm));
}

void emit_lui(compiler_t *c, int rd, int imm)
{
    /* lui: opcode=0x37 */
    emit32(c, rv_u(0x37, rd, imm));
}

void emit_addi(compiler_t *c, int rd, int rs, int imm)
{
    /* addi: opcode=0x13, funct3=0 */
    emit32(c, rv_i(0x13, rd, 0, rs, imm));
}

void emit_branch(compiler_t *c, int f3, int rs1, int rs2, int imm)
{
    /* branch: opcode=0x63 */
    emit32(c, rv_b(0x63, f3, rs1, rs2, imm));
}

/* ── Data section ──────────────────────────────────────────────── */

void data_init(compiler_t *c)
{
    c->data_cap = EMIT_INIT_CAP;
    c->data_len = 0;
    c->data = (uint8_t *)malloc(c->data_cap);
    if (!c->data) {
        compile_error(c, "out of memory");
    }
}

static void data_grow(compiler_t *c, uint32_t need)
{
    while (c->data_len + need > c->data_cap) {
        c->data_cap *= 2;
        uint8_t *p = (uint8_t *)realloc(c->data, c->data_cap);
        if (!p) {
            compile_error(c, "out of memory");
            return;
        }
        c->data = p;
    }
}

void data_emit8(compiler_t *c, uint8_t val)
{
    data_grow(c, 1);
    c->data[c->data_len++] = val;
}

void data_align4(compiler_t *c)
{
    while (c->data_len & 3)
        data_emit8(c, 0);
}
