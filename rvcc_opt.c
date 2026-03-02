/*
 * rvcc_opt.c — peephole optimizer
 *
 * Runs after parsing, before finalize(). Scans the code buffer for
 * redundant instruction patterns (push-pop pairs, trivial jumps,
 * compare-branch sequences) and removes them. Then compacts the code
 * and remaps all branch offsets and patch records.
 */
#include "rvcc_internal.h"

/* ── Instruction decode helpers ─────────────────────────────────── */

typedef struct {
    uint32_t word;      /* raw instruction */
    uint32_t offset;    /* original byte offset in code[] */
    int      removed;   /* 1 = mark for removal */
} opt_insn_t;

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int opcode(uint32_t w)  { return w & 0x7F; }
static int rd(uint32_t w)      { return (w >> 7) & 0x1F; }
static int funct3(uint32_t w)  { return (w >> 12) & 0x07; }
static int rs1(uint32_t w)     { return (w >> 15) & 0x1F; }
static int rs2(uint32_t w)     { return (w >> 20) & 0x1F; }
static int funct7(uint32_t w)  { return (w >> 25) & 0x7F; }

/* Sign-extend a value from bit 'bits-1' */
static int32_t sign_extend(uint32_t val, int bits)
{
    uint32_t mask = 1U << (bits - 1);
    return (int32_t)((val ^ mask) - mask);
}

/* Extract I-type immediate (bits [31:20], sign-extended) */
static int32_t imm_i(uint32_t w)
{
    return sign_extend(w >> 20, 12);
}

/* Extract S-type immediate */
static int32_t imm_s(uint32_t w)
{
    uint32_t lo = (w >> 7) & 0x1F;
    uint32_t hi = (w >> 25) & 0x7F;
    return sign_extend((hi << 5) | lo, 12);
}

/* Extract B-type immediate */
static int32_t imm_b(uint32_t w)
{
    uint32_t b11  = (w >> 7)  & 0x1;
    uint32_t b4_1 = (w >> 8)  & 0xF;
    uint32_t b10_5= (w >> 25) & 0x3F;
    uint32_t b12  = (w >> 31) & 0x1;
    uint32_t imm = (b12 << 12) | (b11 << 11) | (b10_5 << 5) | (b4_1 << 1);
    return sign_extend(imm, 13);
}

/* Extract J-type immediate */
static int32_t imm_j(uint32_t w)
{
    uint32_t b19_12 = (w >> 12) & 0xFF;
    uint32_t b11    = (w >> 20) & 0x1;
    uint32_t b10_1  = (w >> 21) & 0x3FF;
    uint32_t b20    = (w >> 31) & 0x1;
    uint32_t imm = (b20 << 20) | (b19_12 << 12) | (b11 << 11) | (b10_1 << 1);
    return sign_extend(imm, 21);
}

/* ── Pattern matchers ───────────────────────────────────────────── */

/* Check: addi sp, sp, imm */
static int is_sp_addi(uint32_t w, int expected_imm)
{
    return opcode(w) == 0x13 && rd(w) == REG_SP && funct3(w) == 0 &&
           rs1(w) == REG_SP && imm_i(w) == expected_imm;
}

/* Check: sw rX, 0(sp) — returns rX or -1 */
static int is_sw_sp0(uint32_t w)
{
    if (opcode(w) == 0x23 && funct3(w) == 2 && rs1(w) == REG_SP && imm_s(w) == 0)
        return rs2(w);
    return -1;
}

/* Check: lw rX, 0(sp) — returns rX or -1 */
static int is_lw_sp0(uint32_t w)
{
    if (opcode(w) == 0x03 && funct3(w) == 2 && rs1(w) == REG_SP && imm_i(w) == 0)
        return rd(w);
    return -1;
}

/* Check if instruction writes to a given register */
static int writes_reg(uint32_t w, int reg)
{
    int op = opcode(w);
    /* S-type, B-type: no rd */
    if (op == 0x23 || op == 0x63) return 0;
    if (op == 0x73) return 0; /* ecall */
    return rd(w) == reg;
}

/* Check if instruction reads or writes a given register */
static int uses_reg(uint32_t w, int reg)
{
    int op = opcode(w);
    if (writes_reg(w, reg)) return 1;
    /* Check rs1 — present in R, I, S, B types (not U, J) */
    if (op != 0x37 && op != 0x17 && op != 0x6F) {
        if (rs1(w) == reg) return 1;
    }
    /* Check rs2 — present in R, S, B types */
    if (op == 0x33 || op == 0x23 || op == 0x63) {
        if (rs2(w) == reg) return 1;
    }
    return 0;
}

/* Check if instruction touches sp (reads or writes) */
static int touches_sp(uint32_t w)
{
    return uses_reg(w, REG_SP);
}

/* Check if instruction is a branch or jump (changes control flow) */
static int is_branch_or_jump(uint32_t w)
{
    int op = opcode(w);
    return op == 0x63 || op == 0x6F || op == 0x67 || op == 0x73;
}

/* ── Helpers for scanning past removed instructions ─────────────── */

/* Find the next non-removed instruction at or after index start.
 * Returns -1 if none found before limit. */
static int next_live(opt_insn_t *insns, int n, int start)
{
    while (start < n && insns[start].removed)
        start++;
    return (start < n) ? start : -1;
}

/* ── Peephole pass: mark removable instructions ─────────────────── */

static int mark_patterns(opt_insn_t *insns, int n)
{
    int changed = 0;

    for (int i = 0; i < n; i++) {
        if (insns[i].removed) continue;

        /* ── Push-Pop patterns ─────────────────────────────────────
         * Look for: addi sp,-4 / sw rX,0(sp) then scan forward
         * (skipping removed insns) for the matching lw rY,0(sp) / addi sp,+4.
         * All middle instructions must not touch sp or branch. */
        if (is_sp_addi(insns[i].word, -4)) {
            int j = next_live(insns, n, i + 1);
            if (j < 0) continue;
            int rX = is_sw_sp0(insns[j].word);
            if (rX < 0) continue;
            int sw_idx = j;

            /* Scan middle instructions */
            int mid_start = next_live(insns, n, sw_idx + 1);
            if (mid_start < 0) continue;

            /* Find matching pop: lw rY, 0(sp) followed by addi sp, +4 */
            int k = mid_start;
            int n_mid = 0;
            int mid_ok = 1;
            int any_writes_rY = 0; /* we'll check once we know rY */
            int pop_lw = -1, pop_sp = -1;

            /* Scan up to 16 middle instructions */
            while (k >= 0 && n_mid < 16) {
                uint32_t w = insns[k].word;

                /* Check if this is the pop lw */
                int rY_cand = is_lw_sp0(w);
                if (rY_cand >= 0) {
                    int sp_next = next_live(insns, n, k + 1);
                    if (sp_next >= 0 && is_sp_addi(insns[sp_next].word, 4)) {
                        pop_lw = k;
                        pop_sp = sp_next;
                        break;
                    }
                }

                /* Middle instruction constraints */
                if (touches_sp(w) || is_branch_or_jump(w)) {
                    mid_ok = 0;
                    break;
                }
                n_mid++;
                k = next_live(insns, n, k + 1);
            }

            if (!mid_ok || pop_lw < 0) continue;

            int rY = is_lw_sp0(insns[pop_lw].word);

            /* Check if any middle instruction writes rY */
            any_writes_rY = 0;
            for (int m = mid_start; m < pop_lw; m++) {
                if (insns[m].removed) continue;
                if (writes_reg(insns[m].word, rY)) {
                    any_writes_rY = 1;
                    break;
                }
            }

            if (n_mid == 0) {
                /* Adjacent push-pop (Pattern 1) */
                if (rX == rY) {
                    /* Same register: remove all 4 */
                    insns[i].removed = insns[sw_idx].removed = 1;
                    insns[pop_lw].removed = insns[pop_sp].removed = 1;
                } else {
                    /* Different: replace push with mv rY, rX */
                    insns[i].word = rv_i(0x13, rY, 0, rX, 0);
                    insns[sw_idx].removed = 1;
                    insns[pop_lw].removed = insns[pop_sp].removed = 1;
                }
                changed = 1;
                i = pop_sp;
                continue;
            }

            if (!any_writes_rY) {
                /* Middle code doesn't clobber rY — replace push with mv rY, rX,
                 * remove sw and pop frame. (Generalized Pattern 2) */
                insns[i].word = rv_i(0x13, rY, 0, rX, 0); /* mv rY, rX */
                insns[sw_idx].removed = 1;
                insns[pop_lw].removed = insns[pop_sp].removed = 1;
                changed = 1;
                i = pop_sp;
                continue;
            }

            /* Middle code DOES clobber rY. Try register rename through
             * an available t-register (t0-t6). */
            {
                static const int t_regs[] = {REG_T0, REG_T1, REG_T2, REG_T3, REG_T4, REG_T5, REG_T6};
                int found = 0;
                for (int ti = 0; ti < 7; ti++) {
                    int tr = t_regs[ti];
                    /* Check if any middle instruction uses this t-register */
                    int tr_used = 0;
                    for (int m = mid_start; m < pop_lw; m++) {
                        if (insns[m].removed) continue;
                        if (uses_reg(insns[m].word, tr)) { tr_used = 1; break; }
                    }
                    if (!tr_used) {
                        insns[i].word = rv_i(0x13, tr, 0, rX, 0); /* mv tN, rX */
                        insns[sw_idx].removed = 1;
                        insns[pop_lw].word = rv_i(0x13, rY, 0, tr, 0); /* mv rY, tN */
                        insns[pop_sp].removed = 1;
                        changed = 1; found = 1; break;
                    }
                }
                if (found) { i = pop_sp; continue; }
            }

            /* Can't optimize this push-pop */
            continue;
        }

        /* Pattern 3: Jump to next instruction (jal x0, +4) */
        if (opcode(insns[i].word) == 0x6F && rd(insns[i].word) == REG_ZERO) {
            int32_t off = imm_j(insns[i].word);
            if (off == 4) {
                insns[i].removed = 1;
                changed = 1;
                continue;
            }
        }

        /* Pattern 3b: NOP removal (addi x0, x0, 0) */
        if (insns[i].word == rv_i(0x13, REG_ZERO, 0, REG_ZERO, 0)) {
            insns[i].removed = 1;
            changed = 1;
            continue;
        }

        /* Pattern 3c: Dead move elimination.
         * mv rX, rY (addi rX, rY, 0) where the next live instruction
         * unconditionally writes to rX → the mv is dead, remove it. */
        if (opcode(insns[i].word) == 0x13 && funct3(insns[i].word) == 0 &&
            imm_i(insns[i].word) == 0 && rd(insns[i].word) != REG_ZERO) {
            int rX = rd(insns[i].word);
            int j = next_live(insns, n, i + 1);
            if (j >= 0 && !is_branch_or_jump(insns[j].word) &&
                writes_reg(insns[j].word, rX) &&
                !( /* ensure next insn doesn't READ rX via rs1/rs2 */
                  (opcode(insns[j].word) != 0x37 && opcode(insns[j].word) != 0x17 &&
                   opcode(insns[j].word) != 0x6F && rs1(insns[j].word) == rX) ||
                  ((opcode(insns[j].word) == 0x33 || opcode(insns[j].word) == 0x23 ||
                    opcode(insns[j].word) == 0x63) && rs2(insns[j].word) == rX)
                )) {
                insns[i].removed = 1;
                changed = 1;
                continue;
            }
        }

        /* Pattern 3d: Store-load elimination (extended).
         * sw rX, off(base) ... lw rY, off(base) with same offset and base,
         * no intervening store to same location, no branches, and rX not
         * clobbered → remove lw (if rX==rY) or replace with mv rY, rX.
         * Scans up to 8 live instructions forward. */
        if (opcode(insns[i].word) == 0x23 && funct3(insns[i].word) == 2) {
            int sw_rs2 = rs2(insns[i].word);
            int sw_base = rs1(insns[i].word);
            int32_t sw_off = imm_s(insns[i].word);

            int k = i;
            int n_scanned = 0;
            int ok = 1;
            while (ok && n_scanned < 8) {
                k = next_live(insns, n, k + 1);
                if (k < 0) break;
                uint32_t w = insns[k].word;

                /* Found matching load? */
                if (opcode(w) == 0x03 && funct3(w) == 2 &&
                    rs1(w) == sw_base && imm_i(w) == sw_off) {
                    int lw_rd = rd(w);
                    if (lw_rd == sw_rs2) {
                        insns[k].removed = 1;
                    } else {
                        insns[k].word = rv_i(0x13, lw_rd, 0, sw_rs2, 0);
                    }
                    changed = 1;
                    break;
                }

                /* Intervening store to same (base, offset)? */
                if (opcode(w) == 0x23 && rs1(w) == sw_base && imm_s(w) == sw_off) {
                    ok = 0; break;
                }
                /* Branch or jump? */
                if (is_branch_or_jump(w)) { ok = 0; break; }
                /* Stack operations? Push-pop opt may rewrite intervening insns */
                if (touches_sp(w)) { ok = 0; break; }
                /* Source register clobbered? */
                if (writes_reg(w, sw_rs2)) { ok = 0; break; }
                /* Base register clobbered? */
                if (writes_reg(w, sw_base)) { ok = 0; break; }
                n_scanned++;
            }
            if (!ok || k < 0) { /* no match found, fall through */ }
            else continue;
        }

        /* Pattern 3e: li-mv fusion.
         * li rD, imm (addi rD, zero, imm) followed by mv rY, rD (addi rY, rD, 0)
         * where rD is not used after → li rY, imm directly. */
        if (opcode(insns[i].word) == 0x13 && funct3(insns[i].word) == 0 &&
            rs1(insns[i].word) == REG_ZERO && rd(insns[i].word) != REG_ZERO) {
            int rD = rd(insns[i].word);
            int32_t imm = imm_i(insns[i].word);
            int j = next_live(insns, n, i + 1);
            if (j >= 0 && opcode(insns[j].word) == 0x13 && funct3(insns[j].word) == 0 &&
                rs1(insns[j].word) == rD && imm_i(insns[j].word) == 0 &&
                rd(insns[j].word) != REG_ZERO) {
                int rY = rd(insns[j].word);
                /* Check rD is dead after the mv */
                int rd_dead = 1;
                int k = next_live(insns, n, j + 1);
                if (k >= 0 && uses_reg(insns[k].word, rD) && !writes_reg(insns[k].word, rD))
                    rd_dead = 0;
                if (rD == rY) rd_dead = 1; /* trivial: same reg */
                if (rd_dead) {
                    insns[i].word = rv_i(0x13, rY, 0, REG_ZERO, imm);
                    insns[j].removed = 1;
                    changed = 1; continue;
                }
            }
        }

        /* Pattern 3f: lui-mv fusion.
         * lui rD, imm followed by mv rY, rD where rD is dead after → lui rY, imm. */
        if (opcode(insns[i].word) == 0x37 && rd(insns[i].word) != REG_ZERO) {
            int rD = rd(insns[i].word);
            uint32_t lui_imm = insns[i].word & 0xFFFFF000U;
            int j = next_live(insns, n, i + 1);
            if (j >= 0 && opcode(insns[j].word) == 0x13 && funct3(insns[j].word) == 0 &&
                rs1(insns[j].word) == rD && imm_i(insns[j].word) == 0 &&
                rd(insns[j].word) != REG_ZERO) {
                int rY = rd(insns[j].word);
                int rd_dead = 1;
                int k = next_live(insns, n, j + 1);
                if (k >= 0 && uses_reg(insns[k].word, rD) && !writes_reg(insns[k].word, rD))
                    rd_dead = 0;
                if (rD == rY) rd_dead = 1;
                if (rd_dead) {
                    insns[i].word = lui_imm | ((rY & 0x1F) << 7);
                    insns[j].removed = 1;
                    changed = 1; continue;
                }
            }
        }

        /* Pattern 3g: Redundant li elimination.
         * li rD, IMM (addi rD, zero, IMM) — scan backward up to 8 live insns.
         * If a prior li rD, same_IMM exists with no writes to rD and no branches
         * between, remove the duplicate. */
        if (opcode(insns[i].word) == 0x13 && funct3(insns[i].word) == 0 &&
            rs1(insns[i].word) == REG_ZERO && rd(insns[i].word) != REG_ZERO) {
            int rD = rd(insns[i].word);
            int32_t imm_val = imm_i(insns[i].word);
            int k = i - 1;
            int n_back = 0;
            int found = 0;
            while (k >= 0 && n_back < 8) {
                if (insns[k].removed) { k--; continue; }
                uint32_t w = insns[k].word;
                /* Same li? */
                if (opcode(w) == 0x13 && funct3(w) == 0 &&
                    rs1(w) == REG_ZERO && rd(w) == rD && imm_i(w) == imm_val) {
                    insns[i].removed = 1;
                    changed = 1;
                    found = 1;
                    break;
                }
                /* rD written by something else? */
                if (writes_reg(w, rD)) break;
                /* Branch/jump? */
                if (is_branch_or_jump(w)) break;
                n_back++;
                k--;
            }
            if (found) continue;
        }

        /* Pattern 4: Compare-Branch Fusion (equality)
         * sub a0,rX,rY + sltiu a0,a0,1 + beq a0,zero,T → bne rX,rY,T
         * sub a0,rX,rY + sltu a0,zero,a0 + beq a0,zero,T → beq rX,rY,T
         * Handles any sub a0,rX,rY (including t-register operands). */
        if (i + 2 < n &&
            opcode(insns[i].word) == 0x33 && rd(insns[i].word) == REG_A0 &&
            funct3(insns[i].word) == 0 && funct7(insns[i].word) == 0x20 &&
            !insns[i+1].removed && !insns[i+2].removed &&
            opcode(insns[i+2].word) == 0x63 &&
            funct3(insns[i+2].word) == 0 &&
            rs1(insns[i+2].word) == REG_A0 && rs2(insns[i+2].word) == REG_ZERO)
        {
            int sub_rs1 = rs1(insns[i].word);
            int sub_rs2 = rs2(insns[i].word);
            uint32_t w1 = insns[i+1].word;

            /* == test: sltiu a0, a0, 1 */
            if (opcode(w1) == 0x13 && rd(w1) == REG_A0 && funct3(w1) == 3 &&
                rs1(w1) == REG_A0 && imm_i(w1) == 1)
            {
                int32_t branch_off = imm_b(insns[i+2].word);
                int32_t new_off = branch_off + (int32_t)(insns[i+2].offset - insns[i].offset);
                insns[i].word = rv_b(0x63, 1, sub_rs1, sub_rs2, new_off);
                insns[i+1].removed = insns[i+2].removed = 1;
                changed = 1;
                i += 2;
                continue;
            }

            /* != test: sltu a0, zero, a0 */
            if (opcode(w1) == 0x33 && rd(w1) == REG_A0 && funct3(w1) == 3 &&
                rs1(w1) == REG_ZERO && rs2(w1) == REG_A0 && funct7(w1) == 0)
            {
                int32_t branch_off = imm_b(insns[i+2].word);
                int32_t new_off = branch_off + (int32_t)(insns[i+2].offset - insns[i].offset);
                insns[i].word = rv_b(0x63, 0, sub_rs1, sub_rs2, new_off);
                insns[i+1].removed = insns[i+2].removed = 1;
                changed = 1;
                i += 2;
                continue;
            }
        }

        /* Pattern 5a: slt/sltu a0,rX,rY + beq a0,zero,T → bge/bgeu rX,rY,T */
        if (i + 1 < n &&
            opcode(insns[i].word) == 0x33 && rd(insns[i].word) == REG_A0 &&
            (funct3(insns[i].word) == 2 || funct3(insns[i].word) == 3) &&
            funct7(insns[i].word) == 0 &&
            !insns[i+1].removed &&
            opcode(insns[i+1].word) == 0x63 &&
            funct3(insns[i+1].word) == 0 &&
            rs1(insns[i+1].word) == REG_A0 && rs2(insns[i+1].word) == REG_ZERO)
        {
            int f3_slt = funct3(insns[i].word);
            int rX = rs1(insns[i].word);
            int rY = rs2(insns[i].word);
            int32_t branch_off = imm_b(insns[i+1].word);
            int32_t new_off = branch_off + (int32_t)(insns[i+1].offset - insns[i].offset);

            int new_f3 = (f3_slt == 2) ? 5 : 7;
            insns[i].word = rv_b(0x63, new_f3, rX, rY, new_off);
            insns[i+1].removed = 1;
            changed = 1;
            i += 1;
            continue;
        }

        /* Pattern 5b: slt/sltu + xori a0,a0,1 + beq a0,zero,T → blt/bltu */
        if (i + 2 < n &&
            opcode(insns[i].word) == 0x33 && rd(insns[i].word) == REG_A0 &&
            (funct3(insns[i].word) == 2 || funct3(insns[i].word) == 3) &&
            funct7(insns[i].word) == 0 &&
            !insns[i+1].removed && !insns[i+2].removed)
        {
            uint32_t w1 = insns[i+1].word;
            if (opcode(w1) == 0x13 && rd(w1) == REG_A0 && funct3(w1) == 4 &&
                rs1(w1) == REG_A0 && imm_i(w1) == 1 &&
                opcode(insns[i+2].word) == 0x63 &&
                funct3(insns[i+2].word) == 0 &&
                rs1(insns[i+2].word) == REG_A0 && rs2(insns[i+2].word) == REG_ZERO)
            {
                int f3_slt = funct3(insns[i].word);
                int rX = rs1(insns[i].word);
                int rY = rs2(insns[i].word);
                int32_t branch_off = imm_b(insns[i+2].word);
                int32_t new_off = branch_off + (int32_t)(insns[i+2].offset - insns[i].offset);

                int new_f3 = (f3_slt == 2) ? 4 : 6;
                insns[i].word = rv_b(0x63, new_f3, rX, rY, new_off);
                insns[i+1].removed = insns[i+2].removed = 1;
                changed = 1;
                i += 2;
                continue;
            }
        }
    }

    return changed;
}

/* ── Dead function elimination ──────────────────────────────────── */

/* Mark all instructions belonging to unreferenced functions as removed. */
static void mark_dead_functions(compiler_t *c, opt_insn_t *insns, int n_insns)
{
    /* Build sorted list of defined function ranges */
    typedef struct { uint32_t start; uint32_t end; int dead; } frange_t;
    int nf = 0;
    frange_t ranges[MAX_FUNCS];

    for (int i = 0; i < c->num_funcs; i++) {
        if (!c->funcs[i].defined) continue;
        ranges[nf].start = c->funcs[i].addr;
        ranges[nf].end   = 0;
        ranges[nf].dead  = 0;
        /* Is this main? main is always referenced */
        if (strcmp(c->funcs[i].name, "main") == 0)
            c->funcs[i].referenced = 1;
        if (!c->funcs[i].referenced)
            ranges[nf].dead = 1;
        nf++;
    }
    if (nf == 0) return;

    /* Sort by start address */
    for (int i = 0; i < nf - 1; i++)
        for (int j = i + 1; j < nf; j++)
            if (ranges[j].start < ranges[i].start) {
                frange_t tmp = ranges[i];
                ranges[i] = ranges[j];
                ranges[j] = tmp;
            }

    /* Compute end of each function = start of next (or end of code) */
    for (int i = 0; i < nf - 1; i++)
        ranges[i].end = ranges[i+1].start;
    ranges[nf-1].end = c->code_len;

    /* Mark instructions in dead function ranges */
    for (int f = 0; f < nf; f++) {
        if (!ranges[f].dead) continue;
        for (int i = 0; i < n_insns; i++) {
            uint32_t off = insns[i].offset;
            if (off >= ranges[f].start && off < ranges[f].end)
                insns[i].removed = 1;
        }
    }

    /* Remove patches that reference dead functions or fall within dead ranges */
    for (int i = 0; i < c->num_call_patches; ) {
        uint32_t co = c->call_patches[i].code_offset;
        int in_dead = 0;
        for (int f = 0; f < nf; f++) {
            if (ranges[f].dead && co >= ranges[f].start && co < ranges[f].end) {
                in_dead = 1; break;
            }
        }
        if (in_dead) {
            c->call_patches[i] = c->call_patches[--c->num_call_patches];
        } else {
            i++;
        }
    }
    for (int i = 0; i < c->num_string_patches; ) {
        uint32_t co = c->string_patches[i].lui_offset;
        int in_dead = 0;
        for (int f = 0; f < nf; f++) {
            if (ranges[f].dead && co >= ranges[f].start && co < ranges[f].end) {
                in_dead = 1; break;
            }
        }
        if (in_dead) {
            c->string_patches[i] = c->string_patches[--c->num_string_patches];
        } else {
            i++;
        }
    }
    for (int i = 0; i < c->num_global_patches; ) {
        uint32_t co = c->global_patches[i].lui_offset;
        int in_dead = 0;
        for (int f = 0; f < nf; f++) {
            if (ranges[f].dead && co >= ranges[f].start && co < ranges[f].end) {
                in_dead = 1; break;
            }
        }
        if (in_dead) {
            c->global_patches[i] = c->global_patches[--c->num_global_patches];
        } else {
            i++;
        }
    }
}

/* ── Offset remapping and compaction ────────────────────────────── */

void optimize(compiler_t *c)
{
    if (c->had_error) return;
    if (c->code_len <= STARTUP_SIZE) return;

    /* 1. Decode instructions from STARTUP_SIZE..code_len */
    int n_insns = (c->code_len - STARTUP_SIZE) / 4;
    if (n_insns <= 0) return;

    opt_insn_t *insns = (opt_insn_t *)malloc(n_insns * sizeof(opt_insn_t));
    if (!insns) { compile_error(c, "optimizer: out of memory"); return; }

    for (int i = 0; i < n_insns; i++) {
        uint32_t off = STARTUP_SIZE + i * 4;
        insns[i].word = read32(c->code + off);
        insns[i].offset = off;
        insns[i].removed = 0;
    }

    /* 1b. Dead function elimination */
    mark_dead_functions(c, insns, n_insns);

    /* 2. Mark patterns for removal — run multiple passes until stable */
    for (int pass = 0; pass < 8; pass++) {
        if (!mark_patterns(insns, n_insns))
            break;
    }

    /* 3. Build offset map: old_offset → new_offset */
    int map_size = n_insns + 1;
    uint32_t *offset_map = (uint32_t *)malloc(map_size * sizeof(uint32_t));
    if (!offset_map) {
        free(insns);
        compile_error(c, "optimizer: out of memory");
        return;
    }

    uint32_t new_off = STARTUP_SIZE;
    for (int i = 0; i < n_insns; i++) {
        offset_map[i] = new_off;
        if (!insns[i].removed)
            new_off += 4;
    }
    offset_map[n_insns] = new_off;
    uint32_t new_code_len = new_off;

    #define MAP_OFFSET(old_off) \
        ((old_off) < STARTUP_SIZE ? (old_off) : \
         offset_map[((old_off) - STARTUP_SIZE) / 4])

    /* 4. Fix branch and jump offsets */
    for (int i = 0; i < n_insns; i++) {
        if (insns[i].removed) continue;
        uint32_t w = insns[i].word;
        int op = opcode(w);

        if (op == 0x63) {
            int32_t off = imm_b(w);
            uint32_t old_src = insns[i].offset;
            uint32_t old_tgt = (uint32_t)((int32_t)old_src + off);
            uint32_t new_src = MAP_OFFSET(old_src);
            uint32_t new_tgt = MAP_OFFSET(old_tgt);
            int32_t new_rel = (int32_t)(new_tgt - new_src);
            insns[i].word = rv_b(0x63, funct3(w), rs1(w), rs2(w), new_rel);
        }
        else if (op == 0x6F) {
            int32_t off = imm_j(w);
            uint32_t old_src = insns[i].offset;
            uint32_t old_tgt = (uint32_t)((int32_t)old_src + off);
            uint32_t new_src = MAP_OFFSET(old_src);
            uint32_t new_tgt = (old_tgt < STARTUP_SIZE) ? old_tgt : MAP_OFFSET(old_tgt);
            int32_t new_rel = (int32_t)(new_tgt - new_src);
            insns[i].word = rv_j(0x6F, rd(w), new_rel);
        }
    }

    /* 5. Compact: write non-removed instructions back to code buffer */
    uint32_t wp = STARTUP_SIZE;
    for (int i = 0; i < n_insns; i++) {
        if (insns[i].removed) continue;
        uint32_t w = insns[i].word;
        c->code[wp++] = (uint8_t)(w);
        c->code[wp++] = (uint8_t)(w >> 8);
        c->code[wp++] = (uint8_t)(w >> 16);
        c->code[wp++] = (uint8_t)(w >> 24);
    }
    c->code_len = new_code_len;

    /* 6. Remap patch records */
    for (int i = 0; i < c->num_call_patches; i++)
        c->call_patches[i].code_offset = MAP_OFFSET(c->call_patches[i].code_offset);
    for (int i = 0; i < c->num_string_patches; i++) {
        c->string_patches[i].lui_offset  = MAP_OFFSET(c->string_patches[i].lui_offset);
        c->string_patches[i].addi_offset = MAP_OFFSET(c->string_patches[i].addi_offset);
    }
    for (int i = 0; i < c->num_global_patches; i++) {
        c->global_patches[i].lui_offset  = MAP_OFFSET(c->global_patches[i].lui_offset);
        c->global_patches[i].addi_offset = MAP_OFFSET(c->global_patches[i].addi_offset);
    }
    for (int i = 0; i < c->num_funcs; i++) {
        if (c->funcs[i].defined)
            c->funcs[i].addr = MAP_OFFSET(c->funcs[i].addr);
    }

    #undef MAP_OFFSET

    free(offset_map);
    free(insns);
}
