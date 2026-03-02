/*
 * rvcc_type.c — type system and symbol table
 */
#include "rvcc_internal.h"
#include <string.h>

/* ── helpers ───────────────────────────────────────────────────────── */

static int round_up(int val, int align)
{
    return (val + align - 1) & ~(align - 1);
}

/* ── type constructors ─────────────────────────────────────────────── */

type_t type_void(void)
{
    type_t t;
    t.kind        = TY_VOID;
    t.size        = 0;
    t.is_unsigned = 0;
    t.base        = NULL;
    t.array_len   = 0;
    t.struct_id   = -1;
    return t;
}

type_t type_char(void)
{
    type_t t;
    t.kind        = TY_CHAR;
    t.size        = 1;
    t.is_unsigned = 0;
    t.base        = NULL;
    t.array_len   = 0;
    t.struct_id   = -1;
    return t;
}

type_t type_short(void)
{
    type_t t;
    t.kind        = TY_SHORT;
    t.size        = 2;
    t.is_unsigned = 0;
    t.base        = NULL;
    t.array_len   = 0;
    t.struct_id   = -1;
    return t;
}

type_t type_int(void)
{
    type_t t;
    t.kind        = TY_INT;
    t.size        = 4;
    t.is_unsigned = 0;
    t.base        = NULL;
    t.array_len   = 0;
    t.struct_id   = -1;
    return t;
}

type_t type_uchar(void)
{
    type_t t;
    t.kind        = TY_CHAR;
    t.size        = 1;
    t.is_unsigned = 1;
    t.base        = NULL;
    t.array_len   = 0;
    t.struct_id   = -1;
    return t;
}

type_t type_ushort(void)
{
    type_t t;
    t.kind        = TY_SHORT;
    t.size        = 2;
    t.is_unsigned = 1;
    t.base        = NULL;
    t.array_len   = 0;
    t.struct_id   = -1;
    return t;
}

type_t type_uint(void)
{
    type_t t;
    t.kind        = TY_INT;
    t.size        = 4;
    t.is_unsigned = 1;
    t.base        = NULL;
    t.array_len   = 0;
    t.struct_id   = -1;
    return t;
}

type_t type_ptr(type_t *base)
{
    type_t t;
    t.kind        = TY_PTR;
    t.size        = 4;
    t.is_unsigned = 0;
    t.base        = (type_t *)malloc(sizeof(type_t));
    *t.base       = *base;
    t.array_len   = 0;
    t.struct_id   = -1;
    return t;
}

/* ── type queries ──────────────────────────────────────────────────── */

int type_sizeof(type_t *t)
{
    return t->size;
}

bool type_is_ptr(type_t *t)
{
    return t->kind == TY_PTR || t->kind == TY_ARRAY;
}

int type_deref_size(type_t *t)
{
    if (t->kind == TY_PTR || t->kind == TY_ARRAY) {
        if (t->base)
            return t->base->size;
    }
    return 1;
}

/* ── parse_base_type ───────────────────────────────────────────────── */

type_t parse_base_type(compiler_t *c)
{
    /* skip "static" — we ignore it */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "static") == 0)
        next_token(c);

    /* "void" */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "void") == 0) {
        next_token(c);
        return type_void();
    }

    /* "char" */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "char") == 0) {
        next_token(c);
        return type_char();
    }

    /* "short" */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "short") == 0) {
        next_token(c);
        return type_short();
    }

    /* "int" */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "int") == 0) {
        next_token(c);
        return type_int();
    }

    /* "long" [long] [int] → treat as int on RV32 ILP32 */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "long") == 0) {
        next_token(c);
        if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "long") == 0)
            next_token(c); /* skip second "long" */
        if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "int") == 0)
            next_token(c); /* skip optional "int" */
        return type_int();
    }

    /* "unsigned" ... */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "unsigned") == 0) {
        next_token(c);
        if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "char") == 0) {
            next_token(c);
            return type_uchar();
        }
        if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "short") == 0) {
            next_token(c);
            return type_ushort();
        }
        if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "int") == 0) {
            next_token(c);
            return type_uint();
        }
        /* "unsigned long" [long] [int] → uint on RV32 */
        if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "long") == 0) {
            next_token(c);
            if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "long") == 0)
                next_token(c);
            if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "int") == 0)
                next_token(c);
            return type_uint();
        }
        /* "unsigned" alone → unsigned int */
        return type_uint();
    }

    /* "signed" ... */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "signed") == 0) {
        next_token(c);
        if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "char") == 0) {
            next_token(c);
            return type_char();
        }
        if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "short") == 0) {
            next_token(c);
            return type_short();
        }
        if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "int") == 0) {
            next_token(c);
            return type_int();
        }
        /* "signed long" [long] [int] → int on RV32 */
        if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "long") == 0) {
            next_token(c);
            if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "long") == 0)
                next_token(c);
            if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "int") == 0)
                next_token(c);
            return type_int();
        }
        /* "signed" alone → signed int */
        return type_int();
    }

    /* "struct NAME" */
    if (c->tok == TOK_IDENT && strcmp(c->tok_ident, "struct") == 0) {
        next_token(c);
        if (c->tok != TOK_IDENT) {
            compile_error(c, "expected struct name");
            return type_int();
        }
        char name[MAX_IDENT];
        strncpy(name, c->tok_ident, MAX_IDENT - 1);
        name[MAX_IDENT - 1] = '\0';
        next_token(c);

        int id = find_struct(c, name);
        if (id < 0)
            id = add_struct(c, name);

        type_t t;
        t.kind        = TY_STRUCT;
        t.size        = c->structs[id].size;
        t.is_unsigned = 0;
        t.base        = NULL;
        t.array_len   = 0;
        t.struct_id   = id;
        return t;
    }

    /* typedef name */
    if (c->tok == TOK_IDENT) {
        int td = find_typedef(c, c->tok_ident);
        if (td >= 0) {
            next_token(c);
            return c->typedefs[td].type;
        }
    }

    compile_error(c, "expected type");
    return type_int();
}

/* ── parse_declarator ──────────────────────────────────────────────── */

type_t parse_declarator(compiler_t *c, type_t base, char *name_out)
{
    type_t t = base;

    /* pointer stars */
    while (c->tok == TOK_STAR) {
        next_token(c);
        t = type_ptr(&t);
    }

    /* identifier */
    if (name_out)
        name_out[0] = '\0';
    if (c->tok == TOK_IDENT) {
        if (name_out) {
            strncpy(name_out, c->tok_ident, MAX_IDENT - 1);
            name_out[MAX_IDENT - 1] = '\0';
        }
        next_token(c);
    }

    /* array dimension(s) */
    while (c->tok == TOK_LBRACKET) {
        next_token(c); /* consume '[' */
        int len = 0;
        if (c->tok == TOK_NUM) {
            len = c->tok_num;
            next_token(c);
        }
        expect(c, TOK_RBRACKET);

        type_t arr;
        arr.kind        = TY_ARRAY;
        arr.is_unsigned = 0;
        arr.base        = (type_t *)malloc(sizeof(type_t));
        *arr.base       = t;
        arr.array_len   = len;
        arr.size        = type_sizeof(&t) * len;
        arr.struct_id   = -1;
        t = arr;
    }

    return t;
}

/* ── symbol table: find ────────────────────────────────────────────── */

int find_local(compiler_t *c, const char *name)
{
    for (int i = c->num_locals - 1; i >= 0; i--) {
        if (strcmp(c->locals[i].name, name) == 0)
            return i;
    }
    return -1;
}

int find_global(compiler_t *c, const char *name)
{
    for (int i = 0; i < c->num_globals; i++) {
        if (strcmp(c->globals[i].name, name) == 0)
            return i;
    }
    return -1;
}

int find_func(compiler_t *c, const char *name)
{
    for (int i = 0; i < c->num_funcs; i++) {
        if (strcmp(c->funcs[i].name, name) == 0)
            return i;
    }
    return -1;
}

int find_typedef(compiler_t *c, const char *name)
{
    for (int i = 0; i < c->num_typedefs; i++) {
        if (strcmp(c->typedefs[i].name, name) == 0)
            return i;
    }
    return -1;
}

int find_struct(compiler_t *c, const char *name)
{
    for (int i = 0; i < c->num_structs; i++) {
        if (strcmp(c->structs[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* ── symbol table: add ─────────────────────────────────────────────── */

int add_local(compiler_t *c, const char *name, type_t type)
{
    if (c->num_locals >= MAX_LOCALS) {
        compile_error(c, "too many locals");
        return -1;
    }
    int idx = c->num_locals++;
    local_t *l = &c->locals[idx];

    strncpy(l->name, name, MAX_IDENT - 1);
    l->name[MAX_IDENT - 1] = '\0';
    l->type        = type;
    l->scope_depth = c->scope_depth;

    int sz = type_sizeof(&type);
    c->max_local_offset += round_up(sz, 4);
    l->offset = -(c->max_local_offset + 8); /* +8 to skip saved ra and s0 */

    return idx;
}

int add_func(compiler_t *c, const char *name)
{
    if (c->num_funcs >= MAX_FUNCS) {
        compile_error(c, "too many functions");
        return -1;
    }
    int idx = c->num_funcs++;
    func_t *f = &c->funcs[idx];

    memset(f, 0, sizeof(*f));
    strncpy(f->name, name, MAX_IDENT - 1);
    f->name[MAX_IDENT - 1] = '\0';
    f->defined = 0;

    return idx;
}

int add_struct(compiler_t *c, const char *name)
{
    if (c->num_structs >= MAX_STRUCTS) {
        compile_error(c, "too many structs");
        return -1;
    }
    int idx = c->num_structs++;
    struct_def_t *s = &c->structs[idx];

    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, MAX_IDENT - 1);
    s->name[MAX_IDENT - 1] = '\0';
    s->defined = 0;

    return idx;
}

member_t *find_member(compiler_t *c, int struct_id, const char *name)
{
    if (struct_id < 0 || struct_id >= c->num_structs)
        return NULL;
    struct_def_t *s = &c->structs[struct_id];
    for (int i = 0; i < s->num_members; i++) {
        if (strcmp(s->members[i].name, name) == 0)
            return &s->members[i];
    }
    return NULL;
}

/* ── scope management ──────────────────────────────────────────────── */

void enter_scope(compiler_t *c)
{
    c->scope_depth++;
}

void leave_scope(compiler_t *c)
{
    while (c->num_locals > 0 &&
           c->locals[c->num_locals - 1].scope_depth == c->scope_depth) {
        c->num_locals--;
    }
    c->scope_depth--;
}
