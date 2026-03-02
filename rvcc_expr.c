/*
 * rvcc_expr.c — expression parser and codegen
 *
 * All expressions leave their result in a0.
 * Binary ops: push left → eval right into a0 → pop left into a1 → operate.
 */
#include "rvcc_internal.h"
#include <string.h>

/* ── T-register mapping for binary op saves ────────────────────── */
static const int t_save_regs[] = { REG_T3, REG_T4, REG_T5, REG_T6 };
#define MAX_SAVE_DEPTH 4

/* ── lvalue helpers ────────────────────────────────────────────── */

/* Load value from an lvalue into a0 */
static void emit_lvalue_load(compiler_t *c, lvalue_t *lv)
{
    if (lv->kind == 1) {
        /* local variable */
        local_t *l = &c->locals[lv->local_id];
        int sz = type_sizeof(&l->type);
        if (l->type.kind == TY_ARRAY) {
            /* array decays to pointer: address = s0 + offset */
            emit_addi(c, REG_A0, REG_S0, l->offset);
        } else {
            emit_load(c, REG_A0, REG_S0, l->offset, sz, l->type.is_unsigned);
        }
    } else if (lv->kind == 2) {
        /* a0 already has address, dereference */
        int sz = type_sizeof(&lv->type);
        if (lv->type.kind != TY_ARRAY && lv->type.kind != TY_STRUCT)
            emit_load(c, REG_A0, REG_A0, 0, sz, lv->type.is_unsigned);
    } else if (lv->kind == 3) {
        /* global — address already set up via lui+addi, a0 has address */
        int sz = type_sizeof(&lv->type);
        if (lv->type.kind == TY_ARRAY) {
            /* already a pointer */
        } else {
            emit_load(c, REG_A0, REG_A0, 0, sz, lv->type.is_unsigned);
        }
    }
}

/* Store a0 into lvalue. Assumes address/info is available. */
void emit_lvalue_store(compiler_t *c, lvalue_t *lv)
{
    if (lv->kind == 1) {
        local_t *l = &c->locals[lv->local_id];
        int sz = type_sizeof(&l->type);
        emit_store(c, REG_A0, REG_S0, l->offset, sz);
    } else if (lv->kind == 2) {
        /* address is on stack (was pushed before rhs eval) */
        emit_pop(c, REG_A1);   /* a1 = address */
        int sz = type_sizeof(&lv->type);
        emit_store(c, REG_A0, REG_A1, 0, sz);
    } else if (lv->kind == 3) {
        /* need to re-load global address; use t0 */
        /* Actually for global store, we need the address. Let's push/pop. */
        /* The global address was loaded into a0 earlier but we overwrote it with rhs.
         * For globals, the assignment path pushes address before rhs. Pop into a1. */
        emit_pop(c, REG_A1);
        int sz = type_sizeof(&lv->type);
        emit_store(c, REG_A0, REG_A1, 0, sz);
    }
}

/* Helper: emit a global variable address into a0 (with patch record) */
static void emit_global_addr(compiler_t *c, int gid)
{
    if (c->num_global_patches >= MAX_PATCHES) {
        compile_error(c, "too many global patches"); return;
    }
    global_patch_t *gp = &c->global_patches[c->num_global_patches++];
    gp->lui_offset  = emit_cur(c);
    emit_lui(c, REG_A0, 0);    /* placeholder */
    gp->addi_offset = emit_cur(c);
    emit_addi(c, REG_A0, REG_A0, 0); /* placeholder */
    gp->global_id   = gid;
}

/* ── Expression parsing ────────────────────────────────────────── */

/* Parse an lvalue expression: returns the type and fills lv.
 * Does NOT load the value — caller decides whether to load or store. */
type_t expr_lvalue(compiler_t *c, lvalue_t *lv)
{
    lv->kind = 0;
    lv->local_id = -1;
    lv->global_id = -1;

    if (c->tok == TOK_IDENT) {
        char name[MAX_IDENT];
        strncpy(name, c->tok_ident, MAX_IDENT);
        name[MAX_IDENT-1] = '\0';

        int lid = find_local(c, name);
        if (lid >= 0) {
            next_token(c);
            lv->kind = 1;
            lv->local_id = lid;
            lv->type = c->locals[lid].type;
            return c->locals[lid].type;
        }

        int gid = find_global(c, name);
        if (gid >= 0) {
            next_token(c);
            lv->kind = 3;
            lv->global_id = gid;
            lv->type = c->globals[gid].type;
            emit_global_addr(c, gid);
            return c->globals[gid].type;
        }
    }

    if (c->tok == TOK_STAR) {
        /* *expr — dereference lvalue */
        next_token(c);
        type_t t = expr_unary(c);
        /* a0 has address */
        if (type_is_ptr(&t) && t.base) {
            lv->kind = 2;
            lv->type = *t.base;
            return *t.base;
        } else {
            lv->kind = 2;
            lv->type = type_char();
            return type_char();
        }
    }

    /* Not a simple lvalue — parse as expression */
    type_t t = expr_unary(c);
    lv->kind = 0;
    lv->type = t;
    return t;
}

type_t expr_primary(compiler_t *c)
{
    if (c->had_error) return type_int();

    /* Number literal */
    if (c->tok == TOK_NUM) {
        emit_li(c, REG_A0, c->tok_num);
        next_token(c);
        return type_int();
    }

    /* String literal */
    if (c->tok == TOK_STR) {
        /* Register string for data section */
        if (c->num_strings >= MAX_STRINGS) {
            compile_error(c, "too many strings"); return type_ptr(&(type_t){TY_CHAR,1,0,NULL,0,-1});
        }
        int sid = c->num_strings++;
        string_entry_t *se = &c->strings[sid];
        se->len = c->tok_str_len + 1; /* include NUL */
        se->data = (char *)malloc(se->len);
        memcpy(se->data, c->tok_str, c->tok_str_len);
        se->data[c->tok_str_len] = '\0';

        /* Emit placeholder lui+addi (will be patched later) */
        if (c->num_string_patches >= MAX_PATCHES) {
            compile_error(c, "too many string patches"); return type_int();
        }
        string_patch_t *sp = &c->string_patches[c->num_string_patches++];
        sp->lui_offset  = emit_cur(c);
        emit_lui(c, REG_A0, 0);         /* placeholder */
        sp->addi_offset = emit_cur(c);
        emit_addi(c, REG_A0, REG_A0, 0); /* placeholder */
        sp->string_id   = sid;

        next_token(c);
        type_t base = type_char();
        return type_ptr(&base);
    }

    /* Parenthesized expression or cast */
    if (c->tok == TOK_LPAREN) {
        next_token(c);

        /* Check if it's a cast: (type)expr */
        if (is_type_token(c)) {
            type_t cast_type = parse_base_type(c);
            while (c->tok == TOK_STAR) {
                next_token(c);
                cast_type = type_ptr(&cast_type);
            }
            expect(c, TOK_RPAREN);
            expr_cast(c);
            /* Result is in a0, just reinterpret the type */
            return cast_type;
        }

        type_t t = expr(c);
        expect(c, TOK_RPAREN);
        return t;
    }

    /* sizeof */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "sizeof") == 0) {
        next_token(c);
        int paren = 0;
        if (c->tok == TOK_LPAREN) { paren = 1; next_token(c); }

        int sz;
        if (is_type_token(c)) {
            type_t t = parse_base_type(c);
            while (c->tok == TOK_STAR) { next_token(c); t = type_ptr(&t); }
            /* Check for array suffix */
            if (c->tok == TOK_LBRACKET) {
                /* e.g. sizeof(int[10]) — rare but possible */
                next_token(c);
                int alen = 0;
                if (c->tok == TOK_NUM) { alen = c->tok_num; next_token(c); }
                expect(c, TOK_RBRACKET);
                sz = type_sizeof(&t) * alen;
            } else {
                sz = type_sizeof(&t);
            }
        } else {
            /* sizeof(expr) — evaluate for type but discard code?
             * Simple approach: just parse identifier and look up type */
            if (c->tok == TOK_IDENT) {
                int lid = find_local(c, c->tok_ident);
                if (lid >= 0) {
                    sz = type_sizeof(&c->locals[lid].type);
                    next_token(c);
                } else {
                    int gid = find_global(c, c->tok_ident);
                    if (gid >= 0) {
                        sz = type_sizeof(&c->globals[gid].type);
                        next_token(c);
                    } else {
                        sz = 4; /* default */
                        next_token(c);
                    }
                }
            } else {
                sz = 4;
            }
        }

        if (paren) expect(c, TOK_RPAREN);
        emit_li(c, REG_A0, sz);
        return type_int();
    }

    /* __syscall built-in — direct register allocation */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "__syscall") == 0) {
        next_token(c);
        expect(c, TOK_LPAREN);

        /* arg1: syscall id → a0, then move to a7 */
        expr(c);
        emit_mv(c, REG_A7, REG_A0);
        expect(c, TOK_COMMA);

        /* arg2: p1 → a0 (already in final target) */
        expr(c);
        expect(c, TOK_COMMA);

        /* arg3: p2 → a1. Try to detect constant (including casts) */
        {
            int got_const = 0;
            int32_t const_val = 0;

            if (c->tok == TOK_NUM) {
                /* Simple constant */
                const_val = c->tok_num;
                next_token(c);
                got_const = 1;
            } else if (c->tok == TOK_LPAREN && !got_const) {
                /* Peek for (type)(constant) pattern — e.g. (uint32_t)(0) */
                const char *save_pos = c->pos;
                const char *save_expand = c->expand_pos;
                int save_line = c->line;
                int save_tok = c->tok;

                next_token(c);
                if (is_type_token(c)) {
                    /* Skip type and pointer stars */
                    parse_base_type(c);
                    while (c->tok == TOK_STAR) next_token(c);
                    if (c->tok == TOK_RPAREN) {
                        next_token(c); /* consume ) */
                        /* Now expect (NUM) or (- NUM) or just NUM */
                        int paren2 = 0;
                        if (c->tok == TOK_LPAREN) { paren2 = 1; next_token(c); }
                        int negate = 0;
                        if (c->tok == TOK_MINUS) { negate = 1; next_token(c); }
                        if (c->tok == TOK_NUM) {
                            const_val = negate ? -c->tok_num : c->tok_num;
                            next_token(c);
                            if (paren2) expect(c, TOK_RPAREN);
                            got_const = 1;
                        }
                    }
                }

                if (!got_const) {
                    /* Not a cast-constant — restore lexer */
                    c->pos = save_pos;
                    c->expand_pos = save_expand;
                    c->line = save_line;
                    c->tok = save_tok;
                }
            }

            if (got_const) {
                emit_li(c, REG_A1, const_val);
            } else {
                /* General expression for p2 */
                emit_mv(c, REG_T0, REG_A0);  /* save p1 */
                expr(c);
                emit_mv(c, REG_A1, REG_A0);
                emit_mv(c, REG_A0, REG_T0);  /* restore p1 */
            }
        }

        expect(c, TOK_RPAREN);

        /* ecall */
        emit32(c, 0x00000073);

        /* result is in a2, move to a0 */
        emit_mv(c, REG_A0, REG_A2);

        return type_int();
    }

    /* Identifier: variable or function call */
    if (c->tok == TOK_IDENT) {
        char name[MAX_IDENT];
        strncpy(name, c->tok_ident, MAX_IDENT);
        name[MAX_IDENT-1] = '\0';
        next_token(c);

        /* Function call */
        if (c->tok == TOK_LPAREN) {
            next_token(c); /* skip ( */

            /* Find or create function reference */
            int fid = find_func(c, name);
            if (fid < 0) fid = add_func(c, name);

            /* Mark function as referenced and caller as non-leaf */
            if (fid >= 0) c->funcs[fid].referenced = 1;
            c->has_calls = 1;

            /* Spill active t-regs before call (calls clobber all t-regs) */
            int spill_depth = c->save_depth;
            for (int d = 0; d < spill_depth; d++)
                emit_push(c, t_save_regs[d]);

            /* Evaluate args, push all */
            int nargs = 0;
            while (c->tok != TOK_RPAREN && c->tok != TOK_EOF) {
                expr(c);
                emit_push(c, REG_A0);
                nargs++;
                if (c->tok == TOK_COMMA) next_token(c);
            }
            expect(c, TOK_RPAREN);

            /* Pop args into registers a0-a7 (in reverse order) */
            for (int i = nargs - 1; i >= 0; i--) {
                emit_pop(c, REG_A0 + i);
            }

            /* Emit call (may be a forward reference) */
            if (fid >= 0 && c->funcs[fid].defined) {
                int offset = (int)c->funcs[fid].addr - (int)emit_cur(c);
                emit_jal(c, REG_RA, offset);
            } else {
                /* Forward reference — emit placeholder, record patch */
                if (c->num_call_patches >= MAX_PATCHES) {
                    compile_error(c, "too many call patches");
                    return type_int();
                }
                call_patch_t *cp = &c->call_patches[c->num_call_patches++];
                cp->code_offset = emit_cur(c);
                cp->func_id = fid;
                emit_jal(c, REG_RA, 0); /* placeholder */
            }

            /* Restore spilled t-regs after call */
            for (int d = spill_depth - 1; d >= 0; d--)
                emit_pop(c, t_save_regs[d]);

            return (fid >= 0) ? c->funcs[fid].ret_type : type_int();
        }

        /* Variable reference */
        int lid = find_local(c, name);
        if (lid >= 0) {
            local_t *l = &c->locals[lid];
            if (l->type.kind == TY_ARRAY) {
                /* array decays to pointer */
                emit_addi(c, REG_A0, REG_S0, l->offset);
                type_t base = *l->type.base;
                return type_ptr(&base);
            }
            emit_load(c, REG_A0, REG_S0, l->offset,
                       type_sizeof(&l->type), l->type.is_unsigned);
            return l->type;
        }

        int gid = find_global(c, name);
        if (gid >= 0) {
            emit_global_addr(c, gid);
            global_t *g = &c->globals[gid];
            if (g->type.kind == TY_ARRAY) {
                return type_ptr(g->type.base);
            }
            emit_load(c, REG_A0, REG_A0, 0,
                       type_sizeof(&g->type), g->type.is_unsigned);
            return g->type;
        }

        compile_error(c, "undefined identifier '%s'", name);
        return type_int();
    }

    compile_error(c, "expected expression");
    return type_int();
}

type_t expr_postfix(compiler_t *c)
{
    type_t t = expr_primary(c);

    for (;;) {
        if (c->had_error) return t;

        /* Array indexing: expr[index] */
        if (c->tok == TOK_LBRACKET) {
            next_token(c);
            emit_push(c, REG_A0); /* save base addr/value */
            type_t idx_t = expr(c);
            (void)idx_t;
            expect(c, TOK_RBRACKET);

            /* a0 = index, stack top = base pointer */
            int elem_size = type_deref_size(&t);
            if (elem_size != 1) {
                emit_li(c, REG_A1, elem_size);
                emit32(c, rv_r(0x33, REG_A0, 0, REG_A0, REG_A1, 0x01)); /* mul a0,a0,a1 */
            }
            emit_pop(c, REG_A1);  /* a1 = base */
            emit_add(c, REG_A0, REG_A1, REG_A0); /* a0 = base + index*size */

            if (type_is_ptr(&t) && t.base) {
                t = *t.base;
                if (t.kind != TY_ARRAY && t.kind != TY_STRUCT)
                    emit_load(c, REG_A0, REG_A0, 0, type_sizeof(&t), t.is_unsigned);
            }
            continue;
        }

        /* Struct member: expr.member */
        if (c->tok == TOK_DOT || c->tok == TOK_ARROW) {
            int is_arrow = (c->tok == TOK_ARROW);
            next_token(c);

            if (c->tok != TOK_IDENT) {
                compile_error(c, "expected member name");
                return t;
            }
            char mname[MAX_IDENT];
            strncpy(mname, c->tok_ident, MAX_IDENT);
            mname[MAX_IDENT-1] = '\0';
            next_token(c);

            int sid = -1;
            if (is_arrow) {
                /* a0 is pointer to struct */
                if (type_is_ptr(&t) && t.base && t.base->kind == TY_STRUCT)
                    sid = t.base->struct_id;
            } else {
                /* a0 is the struct value/address — for our model, should be address */
                if (t.kind == TY_STRUCT)
                    sid = t.struct_id;
            }

            if (sid < 0) {
                compile_error(c, "member access on non-struct");
                return t;
            }

            member_t *mem = find_member(c, sid, mname);
            if (!mem) {
                compile_error(c, "no member '%s'", mname);
                return t;
            }

            if (mem->offset != 0)
                emit_addi(c, REG_A0, REG_A0, mem->offset);

            t = mem->type;
            if (t.kind != TY_ARRAY && t.kind != TY_STRUCT)
                emit_load(c, REG_A0, REG_A0, 0, type_sizeof(&t), t.is_unsigned);
            continue;
        }

        /* Post-increment/decrement */
        if (c->tok == TOK_INC || c->tok == TOK_DEC) {
            int is_inc = (c->tok == TOK_INC);
            next_token(c);
            /* a0 has current value. We need the lvalue to store back.
             * This is tricky in our model. For simplicity, only support
             * post-inc/dec on the most recent primary (variable). */
            /* Just emit: push old value, add/sub 1, store back, pop old value */
            /* We need to know what we're incrementing. Punt: just modify a0 and
             * assume the caller pattern handles it. For now, treat as value +-1. */
            emit_push(c, REG_A0); /* save old value */
            emit_addi(c, REG_A0, REG_A0, is_inc ? 1 : -1);
            /* We can't easily store back without lvalue info here.
             * This is a limitation — full support would need lvalue tracking through postfix.
             * For now, return old value. */
            emit_pop(c, REG_A0);
            continue;
        }

        break;
    }
    return t;
}

type_t expr_unary(compiler_t *c)
{
    if (c->had_error) return type_int();

    /* Prefix - (negate) */
    if (c->tok == TOK_MINUS) {
        next_token(c);
        type_t t = expr_cast(c);
        /* a0 = -a0: sub a0, zero, a0 */
        emit_sub(c, REG_A0, REG_ZERO, REG_A0);
        return t;
    }

    /* Prefix + (no-op) */
    if (c->tok == TOK_PLUS) {
        next_token(c);
        return expr_cast(c);
    }

    /* Logical not */
    if (c->tok == TOK_BANG) {
        next_token(c);
        expr_cast(c);
        /* a0 = (a0 == 0) ? 1 : 0  →  sltiu a0, a0, 1 */
        emit32(c, rv_i(0x13, REG_A0, 3, REG_A0, 1)); /* sltiu a0,a0,1 */
        return type_int();
    }

    /* Bitwise not */
    if (c->tok == TOK_TILDE) {
        next_token(c);
        type_t t = expr_cast(c);
        /* xori a0, a0, -1 */
        emit32(c, rv_i(0x13, REG_A0, 4, REG_A0, -1)); /* xori a0,a0,-1 */
        return t;
    }

    /* Address-of */
    if (c->tok == TOK_AMP) {
        next_token(c);
        if (c->tok == TOK_IDENT) {
            char name[MAX_IDENT];
            strncpy(name, c->tok_ident, MAX_IDENT);
            name[MAX_IDENT-1] = '\0';
            next_token(c);

            int lid = find_local(c, name);
            if (lid >= 0) {
                emit_addi(c, REG_A0, REG_S0, c->locals[lid].offset);
                type_t base = c->locals[lid].type;
                return type_ptr(&base);
            }
            int gid = find_global(c, name);
            if (gid >= 0) {
                emit_global_addr(c, gid);
                type_t base = c->globals[gid].type;
                return type_ptr(&base);
            }
            compile_error(c, "cannot take address of '%s'", name);
            return type_int();
        }
        compile_error(c, "expected identifier after '&'");
        return type_int();
    }

    /* Dereference */
    if (c->tok == TOK_STAR) {
        next_token(c);
        type_t t = expr_cast(c);
        /* a0 = address, load value */
        if (type_is_ptr(&t) && t.base) {
            type_t deref = *t.base;
            if (deref.kind != TY_STRUCT && deref.kind != TY_ARRAY)
                emit_load(c, REG_A0, REG_A0, 0, type_sizeof(&deref), deref.is_unsigned);
            return deref;
        }
        emit_load(c, REG_A0, REG_A0, 0, 1, 1);
        return type_char();
    }

    /* Pre-increment/decrement */
    if (c->tok == TOK_INC || c->tok == TOK_DEC) {
        int is_inc = (c->tok == TOK_INC);
        next_token(c);
        lvalue_t lv;
        type_t t = expr_lvalue(c, &lv);
        emit_lvalue_load(c, &lv);
        emit_addi(c, REG_A0, REG_A0, is_inc ? 1 : -1);
        if (lv.kind == 1) {
            local_t *l = &c->locals[lv.local_id];
            emit_store(c, REG_A0, REG_S0, l->offset, type_sizeof(&l->type));
        }
        return t;
    }

    return expr_postfix(c);
}

type_t expr_cast(compiler_t *c)
{
    /* Casts handled in expr_primary via (type)expr pattern */
    return expr_unary(c);
}

/* ── Binary operators (precedence climbing) ────────────────────── */

type_t expr_multiplicative(compiler_t *c)
{
    type_t t = expr_cast(c);
    while (c->tok == TOK_STAR || c->tok == TOK_SLASH || c->tok == TOK_PERCENT) {
        int op = c->tok;
        next_token(c);

        /* Constant-RHS shortcut: load constant into a1, left stays in a0 */
        int const_rhs = 0;
        if (c->tok == TOK_NUM) {
            emit_li(c, REG_A1, c->tok_num);
            next_token(c);
            const_rhs = 1;
        }

        int left_reg = REG_A1; /* register holding left operand */

        if (!const_rhs) {
            if (c->save_depth < MAX_SAVE_DEPTH) {
                /* T-reg save: mv tN, a0; eval_rhs; use tN directly */
                left_reg = t_save_regs[c->save_depth];
                emit_mv(c, left_reg, REG_A0);
                c->save_depth++;
                expr_cast(c);
                c->save_depth--;
            } else {
                /* Stack fallback */
                emit_push(c, REG_A0);
                expr_cast(c);
                emit_pop(c, REG_A1);
            }
        }

        if (const_rhs) {
            /* a0=left, a1=right */
            if (op == TOK_STAR) {
                emit32(c, rv_r(0x33, REG_A0, 0, REG_A0, REG_A1, 0x01));
            } else if (op == TOK_SLASH) {
                if (t.is_unsigned)
                    emit32(c, rv_r(0x33, REG_A0, 5, REG_A0, REG_A1, 0x01));
                else
                    emit32(c, rv_r(0x33, REG_A0, 4, REG_A0, REG_A1, 0x01));
            } else {
                if (t.is_unsigned)
                    emit32(c, rv_r(0x33, REG_A0, 7, REG_A0, REG_A1, 0x01));
                else
                    emit32(c, rv_r(0x33, REG_A0, 6, REG_A0, REG_A1, 0x01));
            }
        } else {
            /* left_reg = left, a0 = right */
            if (op == TOK_STAR) {
                emit32(c, rv_r(0x33, REG_A0, 0, left_reg, REG_A0, 0x01));
            } else if (op == TOK_SLASH) {
                if (t.is_unsigned)
                    emit32(c, rv_r(0x33, REG_A0, 5, left_reg, REG_A0, 0x01));
                else
                    emit32(c, rv_r(0x33, REG_A0, 4, left_reg, REG_A0, 0x01));
            } else {
                if (t.is_unsigned)
                    emit32(c, rv_r(0x33, REG_A0, 7, left_reg, REG_A0, 0x01));
                else
                    emit32(c, rv_r(0x33, REG_A0, 6, left_reg, REG_A0, 0x01));
            }
        }
        t = type_int();
    }
    return t;
}

type_t expr_additive(compiler_t *c)
{
    type_t t = expr_multiplicative(c);
    while (c->tok == TOK_PLUS || c->tok == TOK_MINUS) {
        int op = c->tok;
        next_token(c);

        /* Constant-RHS shortcut (non-pointer only): load const into a1 */
        int const_rhs = 0;
        if (!type_is_ptr(&t) && c->tok == TOK_NUM) {
            emit_li(c, REG_A1, c->tok_num);
            next_token(c);
            const_rhs = 1;
        }

        type_t t2;
        int left_reg = REG_A1;

        if (!const_rhs) {
            if (c->save_depth < MAX_SAVE_DEPTH) {
                left_reg = t_save_regs[c->save_depth];
                emit_mv(c, left_reg, REG_A0);
                c->save_depth++;
                t2 = expr_multiplicative(c);
                c->save_depth--;
            } else {
                emit_push(c, REG_A0);
                t2 = expr_multiplicative(c);
                emit_pop(c, REG_A1);
            }
        } else {
            t2 = type_int();
        }

        /* Pointer arithmetic: scale index by element size */
        int scale = 0;
        if (type_is_ptr(&t) && !type_is_ptr(&t2)) {
            scale = type_deref_size(&t);
        }

        if (const_rhs) {
            /* a0=left, a1=right — no pointer scaling needed (guarded above) */
            if (op == TOK_PLUS) {
                emit_add(c, REG_A0, REG_A0, REG_A1);
            } else {
                emit_sub(c, REG_A0, REG_A0, REG_A1);
            }
        } else {
            /* left_reg = left, a0 = right */
            if (scale > 1) {
                emit_li(c, REG_T0, scale);
                emit32(c, rv_r(0x33, REG_A0, 0, REG_A0, REG_T0, 0x01)); /* mul a0,a0,t0 */
            }

            if (op == TOK_PLUS) {
                emit_add(c, REG_A0, left_reg, REG_A0);
                if (type_is_ptr(&t)) return t;
            } else {
                emit_sub(c, REG_A0, left_reg, REG_A0);
                if (type_is_ptr(&t) && type_is_ptr(&t2)) return type_int();
                if (type_is_ptr(&t)) return t;
            }
        }
        t = type_int();
    }
    return t;
}

type_t expr_shift(compiler_t *c)
{
    type_t t = expr_additive(c);
    while (c->tok == TOK_SHL || c->tok == TOK_SHR) {
        int op = c->tok;
        next_token(c);

        /* Constant-RHS shortcut */
        int const_rhs = 0;
        if (c->tok == TOK_NUM) {
            emit_li(c, REG_A1, c->tok_num);
            next_token(c);
            const_rhs = 1;
        }

        int left_reg = REG_A1;

        if (!const_rhs) {
            if (c->save_depth < MAX_SAVE_DEPTH) {
                left_reg = t_save_regs[c->save_depth];
                emit_mv(c, left_reg, REG_A0);
                c->save_depth++;
                expr_additive(c);
                c->save_depth--;
            } else {
                emit_push(c, REG_A0);
                expr_additive(c);
                emit_pop(c, REG_A1);
            }
        }

        if (const_rhs) {
            /* a0=left, a1=shift amount */
            if (op == TOK_SHL)
                emit32(c, rv_r(0x33, REG_A0, 1, REG_A0, REG_A1, 0x00));
            else if (t.is_unsigned)
                emit32(c, rv_r(0x33, REG_A0, 5, REG_A0, REG_A1, 0x00));
            else
                emit32(c, rv_r(0x33, REG_A0, 5, REG_A0, REG_A1, 0x20));
        } else {
            /* left_reg=left, a0=shift amount */
            if (op == TOK_SHL)
                emit32(c, rv_r(0x33, REG_A0, 1, left_reg, REG_A0, 0x00));
            else if (t.is_unsigned)
                emit32(c, rv_r(0x33, REG_A0, 5, left_reg, REG_A0, 0x00));
            else
                emit32(c, rv_r(0x33, REG_A0, 5, left_reg, REG_A0, 0x20));
        }
    }
    return t;
}

type_t expr_relational(compiler_t *c)
{
    type_t t = expr_shift(c);
    while (c->tok == TOK_LT || c->tok == TOK_GT ||
           c->tok == TOK_LE || c->tok == TOK_GE) {
        int op = c->tok;
        next_token(c);

        /* Constant-RHS shortcut: load constant into a1, left stays in a0 */
        int const_rhs = 0;
        if (c->tok == TOK_NUM) {
            emit_li(c, REG_A1, c->tok_num);
            next_token(c);
            const_rhs = 1;
        }

        int left_reg = REG_A1;

        if (!const_rhs) {
            if (c->save_depth < MAX_SAVE_DEPTH) {
                left_reg = t_save_regs[c->save_depth];
                emit_mv(c, left_reg, REG_A0);
                c->save_depth++;
                expr_shift(c);
                c->save_depth--;
            } else {
                emit_push(c, REG_A0);
                expr_shift(c);
                emit_pop(c, REG_A1);
            }
        }

        int is_unsigned = t.is_unsigned;

        if (const_rhs) {
            /* a0=left, a1=right */
            switch (op) {
            case TOK_LT:
                emit32(c, rv_r(0x33, REG_A0, is_unsigned?3:2, REG_A0, REG_A1, 0));
                break;
            case TOK_GT:
                emit32(c, rv_r(0x33, REG_A0, is_unsigned?3:2, REG_A1, REG_A0, 0));
                break;
            case TOK_LE:
                emit32(c, rv_r(0x33, REG_A0, is_unsigned?3:2, REG_A1, REG_A0, 0));
                emit32(c, rv_i(0x13, REG_A0, 4, REG_A0, 1));
                break;
            case TOK_GE:
                emit32(c, rv_r(0x33, REG_A0, is_unsigned?3:2, REG_A0, REG_A1, 0));
                emit32(c, rv_i(0x13, REG_A0, 4, REG_A0, 1));
                break;
            }
        } else {
            /* left_reg=left, a0=right */
            switch (op) {
            case TOK_LT:
                emit32(c, rv_r(0x33, REG_A0, is_unsigned?3:2, left_reg, REG_A0, 0));
                break;
            case TOK_GT:
                emit32(c, rv_r(0x33, REG_A0, is_unsigned?3:2, REG_A0, left_reg, 0));
                break;
            case TOK_LE:
                emit32(c, rv_r(0x33, REG_A0, is_unsigned?3:2, REG_A0, left_reg, 0));
                emit32(c, rv_i(0x13, REG_A0, 4, REG_A0, 1));
                break;
            case TOK_GE:
                emit32(c, rv_r(0x33, REG_A0, is_unsigned?3:2, left_reg, REG_A0, 0));
                emit32(c, rv_i(0x13, REG_A0, 4, REG_A0, 1));
                break;
            }
        }
        t = type_int();
    }
    return t;
}

type_t expr_equality(compiler_t *c)
{
    type_t t = expr_relational(c);
    while (c->tok == TOK_EQ || c->tok == TOK_NE) {
        int op = c->tok;
        next_token(c);

        /* Constant-RHS shortcut: load constant into a1, left stays in a0 */
        int const_rhs = 0;
        if (c->tok == TOK_NUM) {
            emit_li(c, REG_A1, c->tok_num);
            next_token(c);
            const_rhs = 1;
        }

        int left_reg = REG_A1;

        if (!const_rhs) {
            if (c->save_depth < MAX_SAVE_DEPTH) {
                left_reg = t_save_regs[c->save_depth];
                emit_mv(c, left_reg, REG_A0);
                c->save_depth++;
                expr_relational(c);
                c->save_depth--;
            } else {
                emit_push(c, REG_A0);
                expr_relational(c);
                emit_pop(c, REG_A1);
            }
        }

        if (const_rhs) {
            /* a0=left, a1=right → sub a0, a0, a1 */
            emit_sub(c, REG_A0, REG_A0, REG_A1);
        } else {
            /* left_reg=left, a0=right → sub a0, left_reg, a0 */
            emit_sub(c, REG_A0, left_reg, REG_A0);
        }
        if (op == TOK_EQ) {
            emit32(c, rv_i(0x13, REG_A0, 3, REG_A0, 1));
        } else {
            emit32(c, rv_r(0x33, REG_A0, 3, REG_ZERO, REG_A0, 0));
        }
        t = type_int();
    }
    return t;
}

type_t expr_bitand(compiler_t *c)
{
    type_t t = expr_equality(c);
    while (c->tok == TOK_AMP) {
        next_token(c);

        int const_rhs = 0;
        if (c->tok == TOK_NUM) {
            emit_li(c, REG_A1, c->tok_num);
            next_token(c);
            const_rhs = 1;
        }

        int left_reg = REG_A1;

        if (!const_rhs) {
            if (c->save_depth < MAX_SAVE_DEPTH) {
                left_reg = t_save_regs[c->save_depth];
                emit_mv(c, left_reg, REG_A0);
                c->save_depth++;
                expr_equality(c);
                c->save_depth--;
            } else {
                emit_push(c, REG_A0);
                expr_equality(c);
                emit_pop(c, REG_A1);
            }
        }

        if (const_rhs)
            emit32(c, rv_r(0x33, REG_A0, 7, REG_A0, REG_A1, 0)); /* and */
        else
            emit32(c, rv_r(0x33, REG_A0, 7, left_reg, REG_A0, 0)); /* and */
        t = type_int();
    }
    return t;
}

type_t expr_bitxor(compiler_t *c)
{
    type_t t = expr_bitand(c);
    while (c->tok == TOK_CARET) {
        next_token(c);

        int const_rhs = 0;
        if (c->tok == TOK_NUM) {
            emit_li(c, REG_A1, c->tok_num);
            next_token(c);
            const_rhs = 1;
        }

        int left_reg = REG_A1;

        if (!const_rhs) {
            if (c->save_depth < MAX_SAVE_DEPTH) {
                left_reg = t_save_regs[c->save_depth];
                emit_mv(c, left_reg, REG_A0);
                c->save_depth++;
                expr_bitand(c);
                c->save_depth--;
            } else {
                emit_push(c, REG_A0);
                expr_bitand(c);
                emit_pop(c, REG_A1);
            }
        }

        if (const_rhs)
            emit32(c, rv_r(0x33, REG_A0, 4, REG_A0, REG_A1, 0)); /* xor */
        else
            emit32(c, rv_r(0x33, REG_A0, 4, left_reg, REG_A0, 0)); /* xor */
        t = type_int();
    }
    return t;
}

type_t expr_bitor(compiler_t *c)
{
    type_t t = expr_bitxor(c);
    while (c->tok == TOK_PIPE) {
        next_token(c);

        int const_rhs = 0;
        if (c->tok == TOK_NUM) {
            emit_li(c, REG_A1, c->tok_num);
            next_token(c);
            const_rhs = 1;
        }

        int left_reg = REG_A1;

        if (!const_rhs) {
            if (c->save_depth < MAX_SAVE_DEPTH) {
                left_reg = t_save_regs[c->save_depth];
                emit_mv(c, left_reg, REG_A0);
                c->save_depth++;
                expr_bitxor(c);
                c->save_depth--;
            } else {
                emit_push(c, REG_A0);
                expr_bitxor(c);
                emit_pop(c, REG_A1);
            }
        }

        if (const_rhs)
            emit32(c, rv_r(0x33, REG_A0, 6, REG_A0, REG_A1, 0)); /* or */
        else
            emit32(c, rv_r(0x33, REG_A0, 6, left_reg, REG_A0, 0)); /* or */
        t = type_int();
    }
    return t;
}

type_t expr_and(compiler_t *c)
{
    type_t t = expr_bitor(c);
    while (c->tok == TOK_LAND) {
        next_token(c);
        /* Short-circuit: if a0==0 skip right side */
        uint32_t skip_offset = emit_cur(c);
        emit_branch(c, 0, REG_A0, REG_ZERO, 0); /* beq a0,zero,skip — placeholder */
        expr_bitor(c);
        /* Convert to bool: sltu a0, zero, a0 */
        emit32(c, rv_r(0x33, REG_A0, 3, REG_ZERO, REG_A0, 0));
        /* Patch skip branch */
        int skip_dist = (int)emit_cur(c) - (int)skip_offset;
        patch32(c, skip_offset, rv_b(0x63, 0, REG_A0, REG_ZERO, skip_dist));
        t = type_int();
    }
    return t;
}

type_t expr_or(compiler_t *c)
{
    type_t t = expr_and(c);
    while (c->tok == TOK_LOR) {
        next_token(c);
        /* Short-circuit: if a0!=0 skip right side */
        uint32_t skip_offset = emit_cur(c);
        emit_branch(c, 1, REG_A0, REG_ZERO, 0); /* bne a0,zero,skip — placeholder */
        expr_and(c);
        /* Patch skip */
        int skip_dist = (int)emit_cur(c) - (int)skip_offset;
        patch32(c, skip_offset, rv_b(0x63, 1, REG_A0, REG_ZERO, skip_dist));
        /* Convert to bool */
        emit32(c, rv_r(0x33, REG_A0, 3, REG_ZERO, REG_A0, 0));
        t = type_int();
    }
    return t;
}

type_t expr_ternary(compiler_t *c)
{
    /* Ternary not implemented — just pass through */
    return expr_or(c);
}

type_t expr_assign(compiler_t *c)
{
    /* Check if this is a simple lvalue = rhs */
    if (c->tok == TOK_IDENT) {
        char name[MAX_IDENT];
        strncpy(name, c->tok_ident, MAX_IDENT);
        name[MAX_IDENT-1] = '\0';

        int lid = find_local(c, name);
        int gid = (lid < 0) ? find_global(c, name) : -1;

        if (lid >= 0 || gid >= 0) {
            /* Peek ahead to see if next token is an assignment op */
            const char *save_pos = c->pos;
            const char *save_expand = c->expand_pos;
            int save_tok = c->tok;
            int save_line = c->line;
            int32_t save_num = c->tok_num;
            char save_ident[MAX_IDENT];
            memcpy(save_ident, c->tok_ident, MAX_IDENT);

            next_token(c);

            int assign_op = -1;
            if (c->tok == TOK_ASSIGN) assign_op = 0;
            else if (c->tok == TOK_PLUS_ASSIGN) assign_op = TOK_PLUS;
            else if (c->tok == TOK_MINUS_ASSIGN) assign_op = TOK_MINUS;
            else if (c->tok == TOK_STAR_ASSIGN) assign_op = TOK_STAR;
            else if (c->tok == TOK_SLASH_ASSIGN) assign_op = TOK_SLASH;
            else if (c->tok == TOK_PERCENT_ASSIGN) assign_op = TOK_PERCENT;
            else if (c->tok == TOK_AMP_ASSIGN) assign_op = TOK_AMP;
            else if (c->tok == TOK_PIPE_ASSIGN) assign_op = TOK_PIPE;
            else if (c->tok == TOK_CARET_ASSIGN) assign_op = TOK_CARET;
            else if (c->tok == TOK_SHL_ASSIGN) assign_op = TOK_SHL;
            else if (c->tok == TOK_SHR_ASSIGN) assign_op = TOK_SHR;

            /* Also check for [index] = ... pattern */
            int is_array_assign = (c->tok == TOK_LBRACKET);

            if (assign_op >= 0) {
                next_token(c); /* consume assignment op */
                type_t var_type;
                int var_offset = 0;

                if (lid >= 0) {
                    var_type = c->locals[lid].type;
                    var_offset = c->locals[lid].offset;

                    if (assign_op != 0) {
                        /* Load current value first */
                        emit_load(c, REG_A0, REG_S0, var_offset,
                                  type_sizeof(&var_type), var_type.is_unsigned);
                        emit_push(c, REG_A0);
                    }

                    type_t rhs = expr_assign(c);
                    (void)rhs;

                    if (assign_op != 0) {
                        emit_pop(c, REG_A1); /* a1 = old value */
                        /* Apply compound operator */
                        switch (assign_op) {
                        case TOK_PLUS:  emit_add(c, REG_A0, REG_A1, REG_A0); break;
                        case TOK_MINUS: emit_sub(c, REG_A0, REG_A1, REG_A0); break;
                        case TOK_STAR:  emit32(c, rv_r(0x33,REG_A0,0,REG_A1,REG_A0,0x01)); break;
                        case TOK_SLASH: emit32(c, rv_r(0x33,REG_A0,4,REG_A1,REG_A0,0x01)); break;
                        case TOK_PERCENT: emit32(c, rv_r(0x33,REG_A0,6,REG_A1,REG_A0,0x01)); break;
                        case TOK_AMP:  emit32(c, rv_r(0x33,REG_A0,7,REG_A1,REG_A0,0)); break;
                        case TOK_PIPE: emit32(c, rv_r(0x33,REG_A0,6,REG_A1,REG_A0,0)); break;
                        case TOK_CARET: emit32(c, rv_r(0x33,REG_A0,4,REG_A1,REG_A0,0)); break;
                        case TOK_SHL:  emit32(c, rv_r(0x33,REG_A0,1,REG_A1,REG_A0,0)); break;
                        case TOK_SHR:  emit32(c, rv_r(0x33,REG_A0,5,REG_A1,REG_A0,0)); break;
                        default: break;
                        }
                    }

                    emit_store(c, REG_A0, REG_S0, var_offset, type_sizeof(&var_type));
                    return var_type;
                } else {
                    /* Global */
                    var_type = c->globals[gid].type;
                    emit_global_addr(c, gid);
                    emit_push(c, REG_A0); /* push address */

                    if (assign_op != 0) {
                        emit_load(c, REG_A0, REG_A0, 0,
                                  type_sizeof(&var_type), var_type.is_unsigned);
                        emit_push(c, REG_A0);
                    }

                    type_t rhs = expr_assign(c);
                    (void)rhs;

                    if (assign_op != 0) {
                        emit_pop(c, REG_A1);
                        switch (assign_op) {
                        case TOK_PLUS:  emit_add(c, REG_A0, REG_A1, REG_A0); break;
                        case TOK_MINUS: emit_sub(c, REG_A0, REG_A1, REG_A0); break;
                        default: break; /* simplified */
                        }
                    }

                    emit_pop(c, REG_A1); /* a1 = address */
                    emit_store(c, REG_A0, REG_A1, 0, type_sizeof(&var_type));
                    return var_type;
                }
            }

            /* Check for pointer deref assignment: *ptr = ... */
            /* Not reached here — restore and fall through */

            /* Array element assignment: name[idx] = val */
            if (is_array_assign) {
                next_token(c); /* consume [ */
                type_t var_type;
                int elem_size;
                type_t elem_type;

                if (lid >= 0) {
                    var_type = c->locals[lid].type;
                } else {
                    var_type = c->globals[gid].type;
                }

                elem_size = type_deref_size(&var_type);
                elem_type = var_type.base ? *var_type.base : type_char();

                /* Fast path: local array with constant index — compute offset at compile time */
                if (lid >= 0 && var_type.kind == TY_ARRAY && c->tok == TOK_NUM) {
                    int32_t idx_val = c->tok_num;
                    next_token(c);
                    expect(c, TOK_RBRACKET);
                    int off = c->locals[lid].offset + idx_val * elem_size;
                    if (c->tok == TOK_ASSIGN) {
                        next_token(c);
                        expr_assign(c);
                        emit_store(c, REG_A0, REG_S0, off, type_sizeof(&elem_type));
                        return elem_type;
                    } else {
                        emit_load(c, REG_A0, REG_S0, off, type_sizeof(&elem_type),
                                  elem_type.is_unsigned);
                        return elem_type;
                    }
                }

                /* General path */
                if (lid >= 0) {
                    /* Get base address */
                    if (var_type.kind == TY_ARRAY)
                        emit_addi(c, REG_A0, REG_S0, c->locals[lid].offset);
                    else
                        emit_load(c, REG_A0, REG_S0, c->locals[lid].offset, 4, 1);
                } else {
                    emit_global_addr(c, gid);
                    if (var_type.kind != TY_ARRAY)
                        emit_load(c, REG_A0, REG_A0, 0, 4, 1);
                }

                emit_push(c, REG_A0); /* save base */

                type_t idx_t = expr(c);
                (void)idx_t;
                expect(c, TOK_RBRACKET);

                /* a0 = index, compute address */
                if (elem_size != 1) {
                    emit_li(c, REG_A1, elem_size);
                    emit32(c, rv_r(0x33, REG_A0, 0, REG_A0, REG_A1, 0x01)); /* mul */
                }
                emit_pop(c, REG_A1); /* base */
                emit_add(c, REG_A0, REG_A1, REG_A0); /* addr = base + index*elem */

                if (c->tok == TOK_ASSIGN) {
                    /* Array element assignment: name[idx] = val */
                    next_token(c); /* consume = */
                    emit_push(c, REG_A0); /* save address */

                    type_t rhs = expr_assign(c);
                    (void)rhs;

                    emit_pop(c, REG_A1); /* address */
                    emit_store(c, REG_A0, REG_A1, 0, type_sizeof(&elem_type));
                    return elem_type;
                } else {
                    /* Array element read: name[idx] as a value */
                    emit_load(c, REG_A0, REG_A0, 0, type_sizeof(&elem_type),
                              elem_type.is_unsigned);
                    return elem_type;
                }
            }

            /* Not an assignment — restore lexer state and parse normally */
            c->pos = save_pos;
            c->expand_pos = save_expand;
            c->tok = save_tok;
            c->line = save_line;
            c->tok_num = save_num;
            memcpy(c->tok_ident, save_ident, MAX_IDENT);
        }
    }

    /* Dereference assignment: *expr = rhs */
    if (c->tok == TOK_STAR) {
        const char *save_pos = c->pos;
        const char *save_expand = c->expand_pos;
        int save_tok = c->tok;
        int save_line = c->line;
        int32_t save_num = c->tok_num;
        char save_ident[MAX_IDENT];
        memcpy(save_ident, c->tok_ident, MAX_IDENT);

        next_token(c); /* skip * */
        type_t ptr_t = expr_postfix(c);

        if (c->tok == TOK_ASSIGN) {
            next_token(c);
            emit_push(c, REG_A0); /* save address */
            type_t rhs = expr_assign(c);
            (void)rhs;
            emit_pop(c, REG_A1); /* address */
            int sz = 4;
            if (type_is_ptr(&ptr_t) && ptr_t.base)
                sz = type_sizeof(ptr_t.base);
            emit_store(c, REG_A0, REG_A1, 0, sz);
            return ptr_t.base ? *ptr_t.base : type_int();
        }

        /* Not assignment — need to reload the value. But we've consumed tokens.
         * Restore and re-parse as expression. */
        c->pos = save_pos;
        c->expand_pos = save_expand;
        c->tok = save_tok;
        c->line = save_line;
        c->tok_num = save_num;
        memcpy(c->tok_ident, save_ident, MAX_IDENT);
    }

    return expr_ternary(c);
}

type_t expr(compiler_t *c)
{
    return expr_assign(c);
}
