/*
 * rvcc_stmt.c — statement parser/codegen and top-level declarations
 */
#include "rvcc_internal.h"
#include <string.h>

/* ── Statements ────────────────────────────────────────────────── */

static void parse_local_decl(compiler_t *c, type_t base)
{
    /* Parse one or more declarators: type name [= init], name2 ...; */
    do {
        char name[MAX_IDENT];
        type_t t = parse_declarator(c, base, name);

        if (name[0] == '\0') {
            compile_error(c, "expected variable name");
            return;
        }

        /* Infer array size for unsized arrays with initializer */
        if (t.kind == TY_ARRAY && t.array_len == 0 && c->tok == TOK_ASSIGN) {
            /* Save lexer state */
            const char *save_pos = c->pos;
            int save_line = c->line;
            const char *save_expand = c->expand_pos;

            next_token(c); /* consume '=' */

            if (c->tok == TOK_LBRACE) {
                /* Count brace-initializer elements */
                next_token(c); /* consume '{' */
                int count = 0;
                int depth = 1;
                if (c->tok != TOK_RBRACE) count = 1;
                while (c->tok != TOK_EOF && depth > 0) {
                    if (c->tok == TOK_LBRACE) depth++;
                    else if (c->tok == TOK_RBRACE) {
                        depth--;
                        if (depth == 0) break;
                    } else if (c->tok == TOK_COMMA && depth == 1) {
                        count++;
                    }
                    next_token(c);
                }
                int elem_sz = type_deref_size(&t);
                t.array_len = count;
                t.size = count * elem_sz;
            } else if (c->tok == TOK_STR) {
                /* String initializer: char buf[] = "str" */
                int elem_sz = type_deref_size(&t);
                t.array_len = c->tok_str_len + 1; /* +1 for NUL */
                t.size = t.array_len * elem_sz;
            }

            /* Restore lexer state to '=' */
            c->pos = save_pos;
            c->line = save_line;
            c->expand_pos = save_expand;
            c->tok = TOK_ASSIGN;
        }

        int lid = add_local(c, name, t);
        if (lid < 0) return;

        /* Array initializer: = { ... } */
        if (c->tok == TOK_ASSIGN && t.kind == TY_ARRAY) {
            next_token(c);
            if (c->tok == TOK_LBRACE) {
                next_token(c);
                int idx = 0;
                int elem_sz = type_deref_size(&t);
                while (c->tok != TOK_RBRACE && c->tok != TOK_EOF) {
                    expr(c);
                    /* store a0 at s0 + offset + idx*elem_sz */
                    int off = c->locals[lid].offset + idx * elem_sz;
                    emit_store(c, REG_A0, REG_S0, off, elem_sz);
                    idx++;
                    if (c->tok == TOK_COMMA) next_token(c);
                }
                expect(c, TOK_RBRACE);
            } else {
                /* String initializer for char arrays: char buf[] = "str" */
                if (c->tok == TOK_STR) {
                    int slen = c->tok_str_len + 1;
                    for (int i = 0; i < slen && i < t.array_len; i++) {
                        emit_li(c, REG_A0, (unsigned char)c->tok_str[i]);
                        emit_store(c, REG_A0, REG_S0, c->locals[lid].offset + i, 1);
                    }
                    next_token(c);
                } else {
                    expr(c);
                }
            }
        } else if (c->tok == TOK_ASSIGN) {
            next_token(c);
            expr(c);
            emit_store(c, REG_A0, REG_S0, c->locals[lid].offset, type_sizeof(&t));
        }
    } while (match(c, TOK_COMMA));

    expect(c, TOK_SEMICOLON);
}

void parse_stmt(compiler_t *c)
{
    if (c->had_error) return;

    /* Skip "const" qualifier */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "const") == 0)
        next_token(c);

    /* Block */
    if (c->tok == TOK_LBRACE) {
        parse_block(c);
        return;
    }

    /* Empty statement */
    if (match(c, TOK_SEMICOLON)) return;

    /* return */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "return") == 0) {
        next_token(c);
        if (c->tok != TOK_SEMICOLON)
            expr(c);
        expect(c, TOK_SEMICOLON);
        /* Emit jump to epilogue (patched after frame size is known) */
        if (c->cur_return && c->cur_return->count < MAX_BREAK_PATCHES) {
            c->cur_return->offsets[c->cur_return->count++] = emit_cur(c);
            emit_jal(c, REG_ZERO, 0); /* placeholder jump to epilogue */
        } else {
            emit_ret(c); /* fallback */
        }
        return;
    }

    /* if */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "if") == 0) {
        next_token(c);
        expect(c, TOK_LPAREN);
        expr(c);
        expect(c, TOK_RPAREN);

        /* beq a0, zero, else_branch */
        uint32_t branch_off = emit_cur(c);
        emit_branch(c, 0, REG_A0, REG_ZERO, 0); /* placeholder */

        parse_stmt(c);

        if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "else") == 0) {
            next_token(c);
            /* jump past else */
            uint32_t jump_off = emit_cur(c);
            emit_jal(c, REG_ZERO, 0); /* placeholder */

            /* Patch if-branch to here */
            int dist = (int)emit_cur(c) - (int)branch_off;
            patch32(c, branch_off, rv_b(0x63, 0, REG_A0, REG_ZERO, dist));

            parse_stmt(c);

            /* Patch jump past else */
            int jdist = (int)emit_cur(c) - (int)jump_off;
            patch32(c, jump_off, rv_j(0x6F, REG_ZERO, jdist));
        } else {
            /* Patch branch */
            int dist = (int)emit_cur(c) - (int)branch_off;
            patch32(c, branch_off, rv_b(0x63, 0, REG_A0, REG_ZERO, dist));
        }
        return;
    }

    /* while */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "while") == 0) {
        next_token(c);
        expect(c, TOK_LPAREN);

        break_list_t brk = {{0}, 0};
        break_list_t cont = {{0}, 0};
        break_list_t *old_brk = c->cur_break;
        break_list_t *old_cont = c->cur_continue;
        c->cur_break = &brk;
        c->cur_continue = &cont;

        uint32_t loop_start = emit_cur(c);
        expr(c);
        expect(c, TOK_RPAREN);

        /* beq a0, zero, end */
        uint32_t cond_off = emit_cur(c);
        emit_branch(c, 0, REG_A0, REG_ZERO, 0); /* placeholder */

        parse_stmt(c);

        /* Patch continue targets */
        for (int i = 0; i < cont.count; i++) {
            int cd = (int)loop_start - (int)cont.offsets[i];
            patch32(c, cont.offsets[i], rv_j(0x6F, REG_ZERO, cd));
        }

        /* Jump back to condition */
        int back_dist = (int)loop_start - (int)emit_cur(c);
        emit_jal(c, REG_ZERO, back_dist);

        /* Patch condition branch to here */
        int end_dist = (int)emit_cur(c) - (int)cond_off;
        patch32(c, cond_off, rv_b(0x63, 0, REG_A0, REG_ZERO, end_dist));

        /* Patch break targets */
        for (int i = 0; i < brk.count; i++) {
            int d = (int)emit_cur(c) - (int)brk.offsets[i];
            patch32(c, brk.offsets[i], rv_j(0x6F, REG_ZERO, d));
        }

        c->cur_break = old_brk;
        c->cur_continue = old_cont;
        return;
    }

    /* for */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "for") == 0) {
        next_token(c);
        expect(c, TOK_LPAREN);

        enter_scope(c);

        /* Init */
        if (c->tok != TOK_SEMICOLON) {
            if (is_type_token(c)) {
                type_t base = parse_base_type(c);
                parse_local_decl(c, base);
                /* parse_local_decl already consumed the semicolon */
            } else {
                expr(c);
                expect(c, TOK_SEMICOLON);
            }
        } else {
            next_token(c); /* skip ; */
        }

        break_list_t brk = {{0}, 0};
        break_list_t cont = {{0}, 0};
        break_list_t *old_brk = c->cur_break;
        break_list_t *old_cont = c->cur_continue;
        c->cur_break = &brk;
        c->cur_continue = &cont;

        uint32_t cond_start = emit_cur(c);

        /* Condition */
        if (c->tok != TOK_SEMICOLON)
            expr(c);
        else
            emit_li(c, REG_A0, 1); /* always true */
        expect(c, TOK_SEMICOLON);

        uint32_t cond_off = emit_cur(c);
        emit_branch(c, 0, REG_A0, REG_ZERO, 0); /* placeholder */

        /* Skip over increment to body */
        uint32_t body_jump = emit_cur(c);
        emit_jal(c, REG_ZERO, 0); /* placeholder */

        /* Increment */
        uint32_t inc_start = emit_cur(c);
        if (c->tok != TOK_RPAREN)
            expr(c);
        expect(c, TOK_RPAREN);

        /* Jump back to condition */
        int back_to_cond = (int)cond_start - (int)emit_cur(c);
        emit_jal(c, REG_ZERO, back_to_cond);

        /* Patch body_jump to here */
        int body_dist = (int)emit_cur(c) - (int)body_jump;
        patch32(c, body_jump, rv_j(0x6F, REG_ZERO, body_dist));

        /* Body */
        parse_stmt(c);

        /* Patch continue targets to increment */
        for (int i = 0; i < cont.count; i++) {
            int cd = (int)inc_start - (int)cont.offsets[i];
            patch32(c, cont.offsets[i], rv_j(0x6F, REG_ZERO, cd));
        }

        /* Jump back to increment */
        int back_to_inc = (int)inc_start - (int)emit_cur(c);
        emit_jal(c, REG_ZERO, back_to_inc);

        /* Patch condition branch to here */
        int end_dist = (int)emit_cur(c) - (int)cond_off;
        patch32(c, cond_off, rv_b(0x63, 0, REG_A0, REG_ZERO, end_dist));

        /* Patch break targets */
        for (int i = 0; i < brk.count; i++) {
            int d = (int)emit_cur(c) - (int)brk.offsets[i];
            patch32(c, brk.offsets[i], rv_j(0x6F, REG_ZERO, d));
        }

        c->cur_break = old_brk;
        c->cur_continue = old_cont;
        leave_scope(c);
        return;
    }

    /* switch
     * Switch value kept in t1 register (no stack traffic).
     * Layout per case:
     *   fall_through_jmp:  jal zero, body  (skip comparison when falling through)
     *   comparison:        li a0, N
     *                      bne t1, a0, next_comparison
     *   body:              ... statements ...
     */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "switch") == 0) {
        next_token(c);
        expect(c, TOK_LPAREN);
        expr(c);
        expect(c, TOK_RPAREN);

        /* Save switch value in t1 register */
        emit_mv(c, REG_T1, REG_A0);

        break_list_t brk = {{0}, 0};
        break_list_t *old_brk = c->cur_break;
        c->cur_break = &brk;

        expect(c, TOK_LBRACE);

        uint32_t next_case_patch = 0; /* bne placeholder → next comparison */
        int has_next_patch = 0;
        uint32_t fall_jmp_patch = 0;  /* fall-through jmp → body */
        int has_fall_jmp = 0;
        int first_case = 1;

        while (c->tok != TOK_RBRACE && c->tok != TOK_EOF && !c->had_error) {
            if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "case") == 0) {
                next_token(c);

                /* Emit fall-through jump only for 2nd+ case (skips comparison) */
                if (!first_case) {
                    fall_jmp_patch = emit_cur(c);
                    emit_jal(c, REG_ZERO, 0); /* placeholder → body */
                    has_fall_jmp = 1;
                }
                first_case = 0;

                /* Patch previous bne to land here (at comparison) */
                if (has_next_patch) {
                    int d = (int)emit_cur(c) - (int)next_case_patch;
                    patch32(c, next_case_patch, rv_b(0x63, 1, REG_T1, REG_A0, d));
                    has_next_patch = 0;
                }

                /* Case constant */
                int negate = 0;
                if (c->tok == TOK_MINUS) { negate = 1; next_token(c); }
                if (c->tok != TOK_NUM) {
                    compile_error(c, "expected constant in case");
                    break;
                }
                int32_t case_val = negate ? -c->tok_num : c->tok_num;
                next_token(c);
                expect(c, TOK_COLON);

                /* Compare switch value (t1) with case constant */
                emit_li(c, REG_A0, case_val);
                next_case_patch = emit_cur(c);
                emit_branch(c, 1, REG_T1, REG_A0, 0); /* bne placeholder */
                has_next_patch = 1;

                /* Patch fall-through jump to land here (at body) */
                if (has_fall_jmp) {
                    int d = (int)emit_cur(c) - (int)fall_jmp_patch;
                    patch32(c, fall_jmp_patch, rv_j(0x6F, REG_ZERO, d));
                    has_fall_jmp = 0;
                }

            } else if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "default") == 0) {
                next_token(c);
                expect(c, TOK_COLON);

                /* Fall-through jump only if not first label */
                if (!first_case) {
                    fall_jmp_patch = emit_cur(c);
                    emit_jal(c, REG_ZERO, 0);
                    has_fall_jmp = 1;
                }
                first_case = 0;

                /* Patch previous bne here */
                if (has_next_patch) {
                    int d = (int)emit_cur(c) - (int)next_case_patch;
                    patch32(c, next_case_patch, rv_b(0x63, 1, REG_T1, REG_A0, d));
                    has_next_patch = 0;
                }

                /* Patch fall-through to body */
                if (has_fall_jmp) {
                    int d = (int)emit_cur(c) - (int)fall_jmp_patch;
                    patch32(c, fall_jmp_patch, rv_j(0x6F, REG_ZERO, d));
                    has_fall_jmp = 0;
                }

            } else {
                parse_stmt(c);
            }
        }
        expect(c, TOK_RBRACE);

        /* Patch last unresolved bne */
        uint32_t end_off = emit_cur(c);
        if (has_next_patch) {
            int d = (int)end_off - (int)next_case_patch;
            patch32(c, next_case_patch, rv_b(0x63, 1, REG_T1, REG_A0, d));
        }

        /* Patch break targets */
        for (int i = 0; i < brk.count; i++) {
            int d = (int)end_off - (int)brk.offsets[i];
            patch32(c, brk.offsets[i], rv_j(0x6F, REG_ZERO, d));
        }

        c->cur_break = old_brk;
        return;
    }

    /* break */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "break") == 0) {
        next_token(c);
        expect(c, TOK_SEMICOLON);
        if (c->cur_break && c->cur_break->count < MAX_BREAK_PATCHES) {
            c->cur_break->offsets[c->cur_break->count++] = emit_cur(c);
            emit_jal(c, REG_ZERO, 0); /* placeholder */
        } else {
            compile_error(c, "break outside loop");
        }
        return;
    }

    /* continue */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "continue") == 0) {
        next_token(c);
        expect(c, TOK_SEMICOLON);
        if (c->cur_continue && c->cur_continue->count < MAX_BREAK_PATCHES) {
            c->cur_continue->offsets[c->cur_continue->count++] = emit_cur(c);
            emit_jal(c, REG_ZERO, 0); /* placeholder */
        } else {
            compile_error(c, "continue outside loop");
        }
        return;
    }

    /* Local variable declaration */
    if (is_type_token(c)) {
        type_t base = parse_base_type(c);
        parse_local_decl(c, base);
        return;
    }

    /* Expression statement */
    expr(c);
    expect(c, TOK_SEMICOLON);
}

void parse_block(compiler_t *c)
{
    expect(c, TOK_LBRACE);
    enter_scope(c);
    while (c->tok != TOK_RBRACE && c->tok != TOK_EOF && !c->had_error) {
        parse_stmt(c);
    }
    leave_scope(c);
    expect(c, TOK_RBRACE);
}

/* ── Function definition ───────────────────────────────────────── */

void parse_function_def(compiler_t *c, type_t ret_type, const char *name)
{
    int fid = find_func(c, name);
    if (fid < 0) fid = add_func(c, name);
    if (fid < 0) return;

    func_t *f = &c->funcs[fid];
    f->defined = 1;
    f->ret_type = ret_type;
    f->addr = emit_cur(c);

    c->current_func = fid;
    c->num_locals = 0;
    c->scope_depth = 0;
    c->max_local_offset = 0;
    c->has_calls = 0;
    c->save_depth = 0;

    /* Parse parameter list */
    expect(c, TOK_LPAREN);
    f->num_params = 0;
    enter_scope(c);

    /* Try parsing params unless it's bare "void)" */
    {
        bool is_bare_void = false;
        if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "void") == 0) {
            /* Check if next is ')' or '*' */
            const char *sp = c->pos;
            int sl = c->line;
            next_token(c);
            if (c->tok == TOK_RPAREN) {
                is_bare_void = true;
            } else {
                /* It was "void *" or "void name" — not bare void.
                 * Need to re-parse. Problem: we consumed "void".
                 * Treat as void type and continue. */
                type_t pt = type_void();
                char pname[MAX_IDENT];
                pt = parse_declarator(c, pt, pname);
                if (pname[0] && f->num_params < 8) {
                    f->params[f->num_params] = pt;
                    strncpy(f->param_names[f->num_params], pname, MAX_IDENT);
                    int pid = add_local(c, pname, pt);
                    f->num_params++;
                    (void)pid;
                }
                while (match(c, TOK_COMMA)) {
                    if (c->tok == TOK_ELLIPSIS) { next_token(c); break; }
                    type_t bt = parse_base_type(c);
                    type_t pt2 = parse_declarator(c, bt, pname);
                    if (pname[0] && f->num_params < 8) {
                        f->params[f->num_params] = pt2;
                        strncpy(f->param_names[f->num_params], pname, MAX_IDENT);
                        add_local(c, pname, pt2);
                        f->num_params++;
                    }
                }
                (void)sp; (void)sl;
                goto params_done;
            }
        }

        if (!is_bare_void && c->tok != TOK_RPAREN) {
            while (c->tok != TOK_RPAREN && c->tok != TOK_EOF) {
                if (c->tok == TOK_ELLIPSIS) { next_token(c); break; }
                type_t bt = parse_base_type(c);
                char pname[MAX_IDENT];
                type_t pt = parse_declarator(c, bt, pname);
                if (pname[0] && f->num_params < 8) {
                    f->params[f->num_params] = pt;
                    strncpy(f->param_names[f->num_params], pname, MAX_IDENT);
                    add_local(c, pname, pt);
                    f->num_params++;
                }
                if (c->tok == TOK_COMMA) next_token(c);
            }
        }
    }

params_done:
    expect(c, TOK_RPAREN);

    /* Set up return-to-epilogue patch list */
    break_list_t ret_patches = {{0}, 0};
    c->cur_return = &ret_patches;

    /* Emit prologue placeholder (frame size unknown) */
    c->prologue_offset = emit_cur(c);
    c->frame_size = 0;
    /* Reserve space for: addi sp, sp, -frame; sw ra; sw s0; addi s0 */
    emit_nop(c); /* addi sp, sp, -frame (placeholder) */
    emit_nop(c); /* sw ra, frame-4(sp) (placeholder) */
    emit_nop(c); /* sw s0, frame-8(sp) (placeholder) */
    emit_nop(c); /* addi s0, sp, frame  (placeholder) */

    /* Store parameters from registers to stack */
    for (int i = 0; i < f->num_params; i++) {
        int lid = find_local(c, f->param_names[i]);
        if (lid >= 0) {
            int sz = type_sizeof(&c->locals[lid].type);
            emit_store(c, REG_A0 + i, REG_S0, c->locals[lid].offset, sz);
        }
    }

    /* Parse body */
    expect(c, TOK_LBRACE);
    while (c->tok != TOK_RBRACE && c->tok != TOK_EOF && !c->had_error)
        parse_stmt(c);
    expect(c, TOK_RBRACE);

    /* Compute frame size and patch prologue/epilogue */
    int locals_sz = c->max_local_offset;
    int is_leaf = !c->has_calls;

    if (is_leaf && locals_sz == 0) {
        /* No calls, no locals: no frame needed at all.
         * Patch all 4 prologue slots to NOPs (optimizer will remove). */
        c->frame_size = 0;
        patch32(c, c->prologue_offset + 0,  rv_i(0x13, REG_ZERO, 0, REG_ZERO, 0));  /* nop */
        patch32(c, c->prologue_offset + 4,  rv_i(0x13, REG_ZERO, 0, REG_ZERO, 0));  /* nop */
        patch32(c, c->prologue_offset + 8,  rv_i(0x13, REG_ZERO, 0, REG_ZERO, 0));  /* nop */
        patch32(c, c->prologue_offset + 12, rv_i(0x13, REG_ZERO, 0, REG_ZERO, 0));  /* nop */

        /* Epilogue: just ret */
        uint32_t epilogue_off = emit_cur(c);
        emit_ret(c);

        /* Patch return jumps to epilogue */
        for (int i = 0; i < ret_patches.count; i++) {
            int d = (int)epilogue_off - (int)ret_patches.offsets[i];
            patch32(c, ret_patches.offsets[i], rv_j(0x6F, REG_ZERO, d));
        }
        c->cur_return = NULL;
        leave_scope(c);
        c->current_func = -1;
        return;
    }

    if (is_leaf) {
        /* Leaf with locals: need s0 frame but not ra save/restore.
         * Frame = 4 (s0 only) + locals, rounded up to 16 */
        c->frame_size = ((8 + locals_sz + 15) / 16) * 16;
        int frame = c->frame_size;

        patch32(c, c->prologue_offset + 0,
                rv_i(0x13, REG_SP, 0, REG_SP, -frame));        /* addi sp,sp,-frame */
        patch32(c, c->prologue_offset + 4,
                rv_i(0x13, REG_ZERO, 0, REG_ZERO, 0));         /* nop (no ra save) */
        patch32(c, c->prologue_offset + 8,
                rv_s(0x23, 2, REG_SP, REG_S0, frame - 4));     /* sw s0,frame-4(sp) */
        patch32(c, c->prologue_offset + 12,
                rv_i(0x13, REG_S0, 0, REG_SP, frame));         /* addi s0,sp,frame */

        /* Epilogue: restore s0, adjust sp, ret (no ra restore) */
        uint32_t epilogue_off = emit_cur(c);
        emit_load(c, REG_S0, REG_SP, frame - 4, 4, 0);
        emit_addi(c, REG_SP, REG_SP, frame);
        emit_ret(c);

        /* Patch return jumps to epilogue */
        for (int i = 0; i < ret_patches.count; i++) {
            int d = (int)epilogue_off - (int)ret_patches.offsets[i];
            patch32(c, ret_patches.offsets[i], rv_j(0x6F, REG_ZERO, d));
        }
        c->cur_return = NULL;
        leave_scope(c);
        c->current_func = -1;
        return;
    }

    /* Non-leaf: full frame with ra + s0 */
    c->frame_size = ((8 + locals_sz + 15) / 16) * 16;
    int frame = c->frame_size;

    /* Patch prologue */
    patch32(c, c->prologue_offset + 0,
            rv_i(0x13, REG_SP, 0, REG_SP, -frame));        /* addi sp,sp,-frame */
    patch32(c, c->prologue_offset + 4,
            rv_s(0x23, 2, REG_SP, REG_RA, frame - 4));     /* sw ra,frame-4(sp) */
    patch32(c, c->prologue_offset + 8,
            rv_s(0x23, 2, REG_SP, REG_S0, frame - 8));     /* sw s0,frame-8(sp) */
    patch32(c, c->prologue_offset + 12,
            rv_i(0x13, REG_S0, 0, REG_SP, frame));         /* addi s0,sp,frame */

    /* Emit epilogue — all return stmts jump here */
    uint32_t epilogue_off = emit_cur(c);
    emit_load(c, REG_RA, REG_SP, frame - 4, 4, 0);
    emit_load(c, REG_S0, REG_SP, frame - 8, 4, 0);
    emit_addi(c, REG_SP, REG_SP, frame);
    emit_ret(c);

    /* Patch return jumps to epilogue */
    for (int i = 0; i < ret_patches.count; i++) {
        int d = (int)epilogue_off - (int)ret_patches.offsets[i];
        patch32(c, ret_patches.offsets[i], rv_j(0x6F, REG_ZERO, d));
    }
    c->cur_return = NULL;

    leave_scope(c);
    c->current_func = -1;
}

/* ── Struct body parsing ───────────────────────────────────────── */

static void parse_struct_body(compiler_t *c, int sid)
{
    struct_def_t *s = &c->structs[sid];
    expect(c, TOK_LBRACE);
    int offset = 0;

    while (c->tok != TOK_RBRACE && c->tok != TOK_EOF) {
        type_t bt = parse_base_type(c);
        do {
            if (s->num_members >= MAX_MEMBERS) {
                compile_error(c, "too many struct members");
                return;
            }
            char mname[MAX_IDENT];
            type_t mt = parse_declarator(c, bt, mname);

            /* Align offset */
            int align = type_sizeof(&mt);
            if (align > 4) align = 4;
            if (align < 1) align = 1;
            offset = ((offset + align - 1) / align) * align;

            member_t *m = &s->members[s->num_members++];
            strncpy(m->name, mname, MAX_IDENT - 1);
            m->name[MAX_IDENT-1] = '\0';
            m->type = mt;
            m->offset = offset;
            offset += type_sizeof(&mt);
        } while (match(c, TOK_COMMA));
        expect(c, TOK_SEMICOLON);
    }
    expect(c, TOK_RBRACE);

    /* Round size up to 4 */
    s->size = ((offset + 3) / 4) * 4;
    s->defined = 1;
}

/* ── Top-level ─────────────────────────────────────────────────── */

void parse_top_level(compiler_t *c)
{
    while (c->tok != TOK_EOF && !c->had_error) {
        /* Skip stray semicolons */
        if (match(c, TOK_SEMICOLON)) continue;

        /* typedef */
        if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "typedef") == 0) {
            next_token(c);
            type_t bt = parse_base_type(c);

            /* Handle struct body right here if { follows */
            if (bt.kind == TY_STRUCT && c->tok == TOK_LBRACE) {
                parse_struct_body(c, bt.struct_id);
                bt.size = c->structs[bt.struct_id].size;
            }

            char name[MAX_IDENT];
            type_t t = parse_declarator(c, bt, name);
            if (name[0]) {
                if (c->num_typedefs >= MAX_TYPEDEFS) {
                    compile_error(c, "too many typedefs");
                    continue;
                }
                typedef_t *td = &c->typedefs[c->num_typedefs++];
                strncpy(td->name, name, MAX_IDENT - 1);
                td->name[MAX_IDENT-1] = '\0';
                td->type = t;
            }
            expect(c, TOK_SEMICOLON);
            continue;
        }

        /* struct definition (standalone) */
        if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "struct") == 0) {
            type_t st = parse_base_type(c);
            if (st.kind == TY_STRUCT && c->tok == TOK_LBRACE) {
                parse_struct_body(c, st.struct_id);
                st.size = c->structs[st.struct_id].size;
            }
            /* Could be a variable declaration or just struct def */
            if (c->tok == TOK_SEMICOLON) {
                next_token(c);
                continue;
            }
            /* Variable or function */
            char name[MAX_IDENT];
            type_t t = parse_declarator(c, st, name);
            if (c->tok == TOK_LPAREN) {
                parse_function_def(c, t, name);
            } else {
                /* Global variable */
                if (c->num_globals >= MAX_GLOBALS) {
                    compile_error(c, "too many globals");
                    continue;
                }
                global_t *g = &c->globals[c->num_globals++];
                strncpy(g->name, name, MAX_IDENT - 1);
                g->name[MAX_IDENT-1] = '\0';
                g->type = t;
                g->size = type_sizeof(&t);
                g->data_offset = c->data_len;
                /* Reserve space in data section */
                for (int i = 0; i < g->size; i++)
                    data_emit8(c, 0);
                data_align4(c);
                expect(c, TOK_SEMICOLON);
            }
            continue;
        }

        /* Normal declaration: type name ... */
        type_t bt = parse_base_type(c);
        if (c->had_error) break;

        char name[MAX_IDENT];
        type_t t = parse_declarator(c, bt, name);

        if (c->tok == TOK_LPAREN) {
            /* Function definition or declaration */
            if (name[0]) {
                parse_function_def(c, t, name);
            } else {
                compile_error(c, "expected function name");
            }
        } else if (c->tok == TOK_SEMICOLON) {
            /* Global variable declaration (no initializer) */
            next_token(c);
            if (name[0]) {
                if (c->num_globals >= MAX_GLOBALS) {
                    compile_error(c, "too many globals");
                    continue;
                }
                global_t *g = &c->globals[c->num_globals++];
                strncpy(g->name, name, MAX_IDENT - 1);
                g->name[MAX_IDENT-1] = '\0';
                g->type = t;
                g->size = type_sizeof(&t);
                g->data_offset = c->data_len;
                for (int i = 0; i < g->size; i++)
                    data_emit8(c, 0);
                data_align4(c);
            }
        } else if (c->tok == TOK_ASSIGN) {
            /* Global with initializer — put in data section */
            next_token(c);
            if (name[0]) {
                if (c->num_globals >= MAX_GLOBALS) {
                    compile_error(c, "too many globals");
                    continue;
                }
                global_t *g = &c->globals[c->num_globals++];
                strncpy(g->name, name, MAX_IDENT - 1);
                g->name[MAX_IDENT-1] = '\0';
                g->type = t;
                g->size = type_sizeof(&t);
                g->data_offset = c->data_len;

                /* Simple constant initializer */
                if (c->tok == TOK_NUM) {
                    int32_t val = c->tok_num;
                    next_token(c);
                    /* Write value to data section */
                    int sz = g->size;
                    for (int i = 0; i < sz; i++)
                        data_emit8(c, (val >> (i*8)) & 0xFF);
                    data_align4(c);
                } else {
                    /* Zero-fill and skip */
                    for (int i = 0; i < g->size; i++)
                        data_emit8(c, 0);
                    data_align4(c);
                    /* skip to semicolon */
                    while (c->tok != TOK_SEMICOLON && c->tok != TOK_EOF)
                        next_token(c);
                }
            }
            expect(c, TOK_SEMICOLON);
        } else if (c->tok == TOK_COMMA) {
            /* Multiple declarations: int a, b; */
            if (name[0]) {
                if (c->num_globals >= MAX_GLOBALS) {
                    compile_error(c, "too many globals");
                    continue;
                }
                global_t *g = &c->globals[c->num_globals++];
                strncpy(g->name, name, MAX_IDENT - 1);
                g->name[MAX_IDENT-1] = '\0';
                g->type = t;
                g->size = type_sizeof(&t);
                g->data_offset = c->data_len;
                for (int i = 0; i < g->size; i++)
                    data_emit8(c, 0);
                data_align4(c);
            }
            while (match(c, TOK_COMMA)) {
                char name2[MAX_IDENT];
                type_t t2 = parse_declarator(c, bt, name2);
                if (name2[0]) {
                    if (c->num_globals >= MAX_GLOBALS) {
                        compile_error(c, "too many globals");
                        break;
                    }
                    global_t *g = &c->globals[c->num_globals++];
                    strncpy(g->name, name2, MAX_IDENT - 1);
                    g->name[MAX_IDENT-1] = '\0';
                    g->type = t2;
                    g->size = type_sizeof(&t2);
                    g->data_offset = c->data_len;
                    for (int i = 0; i < g->size; i++)
                        data_emit8(c, 0);
                    data_align4(c);
                }
            }
            expect(c, TOK_SEMICOLON);
        } else {
            compile_error(c, "expected function body or ';'");
        }
    }
}
