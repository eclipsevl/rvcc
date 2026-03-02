/*
 * rvcc_internal.h — shared compiler internals
 */
#ifndef RVCC_INTERNAL_H
#define RVCC_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "rvcc.h"

/* ── Limits ─────────────────────────────────────────────────────── */
#define MAX_IDENT       64
#define MAX_LOCALS      256
#define MAX_GLOBALS     128
#define MAX_FUNCS       128
#define MAX_STRINGS     256
#define MAX_PATCHES     512
#define MAX_TYPEDEFS    64
#define MAX_STRUCTS     64
#define MAX_MEMBERS     32
#define MAX_MACROS      256
#define MAX_MACRO_PARAMS 16
#define MAX_MACRO_BODY  1024
#define MAX_INCLUDE_DEPTH 8
#define MAX_COND_DEPTH  32
#define MAX_BREAK_PATCHES 64
#define EMIT_INIT_CAP   4096
#define RAM_BASE        0x80000000U
#define STARTUP_SIZE    16

/* ── RV32 registers ─────────────────────────────────────────────── */
#define REG_ZERO 0
#define REG_RA   1
#define REG_SP   2
#define REG_S0   8
#define REG_A0   10
#define REG_A1   11
#define REG_A2   12
#define REG_A7   17
#define REG_T0   5
#define REG_T1   6
#define REG_T2   7
#define REG_T3   28
#define REG_T4   29
#define REG_T5   30
#define REG_T6   31

/* ── Token types ────────────────────────────────────────────────── */
enum {
    TOK_EOF = 0, TOK_NUM, TOK_STR, TOK_IDENT,
    /* punctuation / operators */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_AMP, TOK_PIPE, TOK_CARET, TOK_TILDE, TOK_BANG,
    TOK_LT, TOK_GT, TOK_LE, TOK_GE, TOK_EQ, TOK_NE,
    TOK_LAND, TOK_LOR,
    TOK_SHL, TOK_SHR,
    TOK_ASSIGN,
    TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN, TOK_STAR_ASSIGN,
    TOK_SLASH_ASSIGN, TOK_PERCENT_ASSIGN,
    TOK_AMP_ASSIGN, TOK_PIPE_ASSIGN, TOK_CARET_ASSIGN,
    TOK_SHL_ASSIGN, TOK_SHR_ASSIGN,
    TOK_INC, TOK_DEC,
    TOK_ARROW, TOK_DOT,
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_SEMICOLON, TOK_COMMA, TOK_COLON,
    TOK_HASH, TOK_ELLIPSIS,
};

/* ── Type kinds ─────────────────────────────────────────────────── */
enum {
    TY_VOID = 0, TY_CHAR, TY_SHORT, TY_INT,
    TY_PTR, TY_ARRAY, TY_STRUCT,
};

/* ── Type ───────────────────────────────────────────────────────── */
typedef struct member_s member_t;

typedef struct type_s {
    int     kind;
    int     size;
    int     is_unsigned;
    struct type_s *base;    /* for PTR and ARRAY */
    int     array_len;      /* for ARRAY */
    int     struct_id;      /* index into structs table for TY_STRUCT */
} type_t;

struct member_s {
    char    name[MAX_IDENT];
    type_t  type;
    int     offset;
};

typedef struct {
    char     name[MAX_IDENT];
    member_t members[MAX_MEMBERS];
    int      num_members;
    int      size;          /* total sizeof */
    int      defined;       /* body has been parsed */
} struct_def_t;

/* ── Symbols ────────────────────────────────────────────────────── */
typedef struct {
    char    name[MAX_IDENT];
    type_t  type;
    int     offset;         /* negative offset from s0 for locals */
    int     scope_depth;
} local_t;

typedef struct {
    char    name[MAX_IDENT];
    type_t  type;
    uint32_t data_offset;   /* offset within data section */
    int     size;           /* total bytes */
} global_t;

typedef struct {
    char    name[MAX_IDENT];
    uint32_t addr;          /* code offset where function starts */
    int     defined;
    int     referenced;     /* 1 if called from another function */
    type_t  ret_type;
    type_t  params[8];
    char    param_names[8][MAX_IDENT];
    int     num_params;
} func_t;

typedef struct {
    char    name[MAX_IDENT];
    type_t  type;
} typedef_t;

/* ── Patches (forward references) ──────────────────────────────── */
typedef struct {
    uint32_t code_offset;   /* where the placeholder instruction is */
    int      func_id;       /* index into funcs[] */
} call_patch_t;

typedef struct {
    uint32_t lui_offset;    /* offset of lui instruction */
    uint32_t addi_offset;   /* offset of addi instruction */
    int      string_id;     /* index into strings[] */
} string_patch_t;

typedef struct {
    uint32_t lui_offset;
    uint32_t addi_offset;
    int      global_id;
} global_patch_t;

/* ── Macro ──────────────────────────────────────────────────────── */
typedef struct {
    char    name[MAX_IDENT];
    char    body[MAX_MACRO_BODY];
    char    params[MAX_MACRO_PARAMS][MAX_IDENT];
    int     num_params;     /* -1 = object-like, >=0 = function-like */
    int     defined;
} macro_t;

/* ── Source state (for #include push/pop) ───────────────────────── */
typedef struct {
    const char *src;
    const char *pos;
    const char *filename;
    int         line;
    char       *allocated;  /* if non-NULL, free when popping */
} source_state_t;

/* ── String literal storage ─────────────────────────────────────── */
typedef struct {
    char    *data;
    int      len;           /* including NUL */
    uint32_t addr;          /* filled during finalization */
} string_entry_t;

/* ── Break/continue patch ───────────────────────────────────────── */
typedef struct {
    uint32_t offsets[MAX_BREAK_PATCHES];
    int      count;
} break_list_t;

/* ── Compiler state ─────────────────────────────────────────────── */
typedef struct {
    /* emit buffer */
    uint8_t *code;
    uint32_t code_len;
    uint32_t code_cap;

    /* source tracking */
    source_state_t src_stack[MAX_INCLUDE_DEPTH];
    int            src_depth;
    const char    *pos;
    const char    *filename;
    int            line;

    /* current token */
    int     tok;
    int32_t tok_num;
    char    tok_str[MAX_MACRO_BODY];
    int     tok_str_len;
    char    tok_ident[MAX_IDENT];

    /* preprocessor */
    macro_t macros[MAX_MACROS];
    int     num_macros;
    /* macro expansion buffer */
    char    expand_buf[MAX_MACRO_BODY * 4];
    const char *expand_pos;    /* if non-NULL, reading from expand_buf */
    int     expanding;         /* prevent recursive expansion */

    /* conditional compilation */
    int     cond_stack[MAX_COND_DEPTH]; /* 0=skip, 1=active, 2=was-active (else should skip) */
    int     cond_depth;

    /* symbols */
    local_t    locals[MAX_LOCALS];
    int        num_locals;
    int        scope_depth;

    global_t   globals[MAX_GLOBALS];
    int        num_globals;

    func_t     funcs[MAX_FUNCS];
    int        num_funcs;
    int        current_func;    /* index of function being compiled, -1 if none */

    typedef_t  typedefs[MAX_TYPEDEFS];
    int        num_typedefs;

    struct_def_t structs[MAX_STRUCTS];
    int          num_structs;

    /* string literals */
    string_entry_t strings[MAX_STRINGS];
    int            num_strings;

    /* patches */
    call_patch_t   call_patches[MAX_PATCHES];
    int            num_call_patches;
    string_patch_t string_patches[MAX_PATCHES];
    int            num_string_patches;
    global_patch_t global_patches[MAX_PATCHES];
    int            num_global_patches;

    /* frame info for current function */
    int     frame_size;
    int     max_local_offset;   /* tracks how much stack space locals need */
    uint32_t prologue_offset;   /* code offset of the prologue addi sp */
    int     has_calls;          /* 1 if current function calls other functions */

    /* for break/continue */
    break_list_t *cur_break;
    break_list_t *cur_continue;

    /* for return-to-epilogue jumps */
    break_list_t *cur_return;

    /* file reader */
    rvcc_file_reader_t reader;
    void              *reader_data;

    /* t-register save depth for binary ops (0..3 → t3..t6, ≥4 → stack) */
    int     save_depth;

    /* optimization */
    int     opt_level;

    /* error state */
    bool    had_error;
    char    error[512];
    int     error_line;

    /* data section */
    uint8_t *data;
    uint32_t data_len;
    uint32_t data_cap;
} compiler_t;

/* ── rvcc_emit.c ────────────────────────────────────────────────── */
void emit_init(compiler_t *c);
void emit8(compiler_t *c, uint8_t val);
void emit32(compiler_t *c, uint32_t val);
void patch32(compiler_t *c, uint32_t offset, uint32_t val);
uint32_t emit_cur(compiler_t *c);

/* RV32IM instruction encoding */
uint32_t rv_r(int op, int rd, int f3, int rs1, int rs2, int f7);
uint32_t rv_i(int op, int rd, int f3, int rs1, int imm);
uint32_t rv_s(int op, int f3, int rs1, int rs2, int imm);
uint32_t rv_b(int op, int f3, int rs1, int rs2, int imm);
uint32_t rv_u(int op, int rd, int imm);
uint32_t rv_j(int op, int rd, int imm);

/* High-level emit helpers */
void emit_nop(compiler_t *c);
void emit_li(compiler_t *c, int rd, int32_t imm);
void emit_mv(compiler_t *c, int rd, int rs);
void emit_push(compiler_t *c, int reg);
void emit_pop(compiler_t *c, int reg);
void emit_ret(compiler_t *c);
void emit_load(compiler_t *c, int rd, int rs, int off, int size, int is_unsigned);
void emit_store(compiler_t *c, int rs2, int rs1, int off, int size);
void emit_add(compiler_t *c, int rd, int rs1, int rs2);
void emit_sub(compiler_t *c, int rd, int rs1, int rs2);
void emit_jal(compiler_t *c, int rd, int imm);
void emit_jalr(compiler_t *c, int rd, int rs, int imm);
void emit_lui(compiler_t *c, int rd, int imm);
void emit_addi(compiler_t *c, int rd, int rs, int imm);
void emit_branch(compiler_t *c, int f3, int rs1, int rs2, int imm);

/* Data section helpers */
void data_init(compiler_t *c);
void data_emit8(compiler_t *c, uint8_t val);
void data_align4(compiler_t *c);

/* ── rvcc_lex.c ─────────────────────────────────────────────────── */
void lex_init(compiler_t *c, const char *source, const char *filename);
void next_token(compiler_t *c);
void expect(compiler_t *c, int tok);
bool match(compiler_t *c, int tok);
bool peek(compiler_t *c, int tok);
bool is_type_token(compiler_t *c);
void compile_error(compiler_t *c, const char *fmt, ...);

/* ── rvcc_type.c ────────────────────────────────────────────────── */
type_t type_void(void);
type_t type_char(void);
type_t type_short(void);
type_t type_int(void);
type_t type_uchar(void);
type_t type_ushort(void);
type_t type_uint(void);
type_t type_ptr(type_t *base);
int    type_sizeof(type_t *t);
bool   type_is_ptr(type_t *t);
int    type_deref_size(type_t *t);

type_t parse_base_type(compiler_t *c);
type_t parse_declarator(compiler_t *c, type_t base, char *name_out);

int  find_local(compiler_t *c, const char *name);
int  find_global(compiler_t *c, const char *name);
int  find_func(compiler_t *c, const char *name);
int  find_typedef(compiler_t *c, const char *name);
int  find_struct(compiler_t *c, const char *name);
int  add_local(compiler_t *c, const char *name, type_t type);
int  add_func(compiler_t *c, const char *name);
int  add_struct(compiler_t *c, const char *name);
member_t *find_member(compiler_t *c, int struct_id, const char *name);
void enter_scope(compiler_t *c);
void leave_scope(compiler_t *c);

/* ── rvcc_expr.c ────────────────────────────────────────────────── */
type_t expr(compiler_t *c);            /* assignment expression */
type_t expr_assign(compiler_t *c);
type_t expr_ternary(compiler_t *c);
type_t expr_or(compiler_t *c);
type_t expr_and(compiler_t *c);
type_t expr_bitor(compiler_t *c);
type_t expr_bitxor(compiler_t *c);
type_t expr_bitand(compiler_t *c);
type_t expr_equality(compiler_t *c);
type_t expr_relational(compiler_t *c);
type_t expr_shift(compiler_t *c);
type_t expr_additive(compiler_t *c);
type_t expr_multiplicative(compiler_t *c);
type_t expr_cast(compiler_t *c);
type_t expr_unary(compiler_t *c);
type_t expr_postfix(compiler_t *c);
type_t expr_primary(compiler_t *c);

/* context for lvalue handling */
typedef struct {
    int kind;       /* 0=none, 1=local, 2=deref(a0), 3=global */
    int local_id;
    int global_id;
    type_t type;
} lvalue_t;

type_t expr_lvalue(compiler_t *c, lvalue_t *lv);
void   emit_lvalue_store(compiler_t *c, lvalue_t *lv);

/* ── rvcc_stmt.c ────────────────────────────────────────────────── */
void parse_stmt(compiler_t *c);
void parse_block(compiler_t *c);
void parse_function_def(compiler_t *c, type_t ret_type, const char *name);
void parse_top_level(compiler_t *c);

/* ── rvcc_opt.c ─────────────────────────────────────────────────── */
void optimize(compiler_t *c);

/* ── rvcc_api.c ─────────────────────────────────────────────────── */
/* (public API already declared in rvcc.h) */

#endif /* RVCC_INTERNAL_H */
