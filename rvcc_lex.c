/*
 * rvcc_lex.c — lexer, preprocessor, error reporting
 *
 * Macro expansion uses src_stack (same mechanism as #include) so that
 * nested macro expansion works naturally through c->pos.
 */
#include "rvcc_internal.h"
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

/* ── Error ─────────────────────────────────────────────────────── */

void compile_error(compiler_t *c, const char *fmt, ...)
{
    if (c->had_error) return;
    c->had_error = true;
    c->error_line = c->line;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->error, sizeof(c->error), fmt, ap);
    va_end(ap);
}

/* ── Source stack helpers ──────────────────────────────────────── */

static void push_source(compiler_t *c, const char *new_pos, char *allocated)
{
    if (c->src_depth >= MAX_INCLUDE_DEPTH) {
        compile_error(c, "source nesting too deep");
        return;
    }
    source_state_t *ss = &c->src_stack[c->src_depth++];
    ss->pos       = c->pos;
    ss->filename  = c->filename;
    ss->line      = c->line;
    ss->allocated = allocated; /* non-NULL → free on pop */
    ss->src       = NULL;

    c->pos = new_pos;
    /* Don't change filename/line for macro expansions */
}

static void push_source_file(compiler_t *c, const char *new_pos, char *allocated,
                             const char *filename)
{
    if (c->src_depth >= MAX_INCLUDE_DEPTH) {
        compile_error(c, "#include depth exceeded");
        return;
    }
    source_state_t *ss = &c->src_stack[c->src_depth++];
    ss->pos       = c->pos;
    ss->filename  = c->filename;
    ss->line      = c->line;
    ss->allocated = allocated;
    ss->src       = NULL;

    c->pos      = new_pos;
    c->filename = filename;
    c->line     = 1;
}

static void pop_source(compiler_t *c)
{
    if (c->src_depth <= 0) return;
    c->src_depth--;
    source_state_t *ss = &c->src_stack[c->src_depth];
    c->pos      = ss->pos;
    c->filename = ss->filename;
    c->line     = ss->line;
    /* allocated buffers are leaked (acceptable for a build tool) */
}

/* ── Character helpers ─────────────────────────────────────────── */

static char peekch(compiler_t *c)
{
    if (c->pos && *c->pos) return *c->pos;
    return '\0';
}

static char nextch(compiler_t *c)
{
    if (c->pos && *c->pos) {
        char ch = *c->pos++;
        if (ch == '\n') c->line++;
        return ch;
    }
    return '\0';
}

static void skip_whitespace(compiler_t *c)
{
    for (;;) {
        char ch = peekch(c);
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            nextch(c); continue;
        }
        /* line comments */
        if (ch == '/' && c->pos[1] == '/') {
            while (*c->pos && *c->pos != '\n') c->pos++;
            continue;
        }
        /* block comments */
        if (ch == '/' && c->pos[1] == '*') {
            c->pos += 2;
            while (*c->pos) {
                if (*c->pos == '\n') c->line++;
                if (c->pos[0] == '*' && c->pos[1] == '/') {
                    c->pos += 2; break;
                }
                c->pos++;
            }
            continue;
        }
        break;
    }
}

/* Skip whitespace but not newlines (for directive parsing) */
static void skip_ws_nonnl(compiler_t *c)
{
    while (*c->pos == ' ' || *c->pos == '\t' || *c->pos == '\r') c->pos++;
}

/* ── Preprocessor helpers ──────────────────────────────────────── */

static int find_macro(compiler_t *c, const char *name)
{
    for (int i = 0; i < c->num_macros; i++) {
        if (c->macros[i].defined && strcmp(c->macros[i].name, name) == 0)
            return i;
    }
    return -1;
}

static void skip_to_eol(compiler_t *c)
{
    while (*c->pos && *c->pos != '\n') c->pos++;
}

static void read_ident_raw(compiler_t *c, char *buf, int bufsz)
{
    int n = 0;
    while (isalnum((unsigned char)*c->pos) || *c->pos == '_') {
        if (n < bufsz - 1) buf[n++] = *c->pos;
        c->pos++;
    }
    buf[n] = '\0';
}

static void read_to_eol(compiler_t *c, char *buf, int bufsz)
{
    int n = 0;
    skip_ws_nonnl(c);
    while (*c->pos && *c->pos != '\n') {
        if (n < bufsz - 1) buf[n++] = *c->pos;
        c->pos++;
    }
    while (n > 0 && (buf[n-1] == ' ' || buf[n-1] == '\t' || buf[n-1] == '\r'))
        n--;
    buf[n] = '\0';
}

/* Skip tokens in a false conditional block, tracking nesting */
static void skip_conditional_block(compiler_t *c)
{
    int depth = 1;
    while (*c->pos) {
        while (*c->pos && *c->pos != '\n') c->pos++;
        if (*c->pos == '\n') { c->pos++; c->line++; }
        if (!*c->pos) return;

        skip_ws_nonnl(c);
        if (*c->pos != '#') continue;
        c->pos++;
        skip_ws_nonnl(c);

        char dir[32];
        read_ident_raw(c, dir, sizeof(dir));

        if (strcmp(dir, "if") == 0 || strcmp(dir, "ifdef") == 0 ||
            strcmp(dir, "ifndef") == 0) {
            depth++;
        } else if (strcmp(dir, "endif") == 0) {
            depth--;
            if (depth == 0) return;
        } else if (depth == 1 && strcmp(dir, "else") == 0) {
            return;
        } else if (depth == 1 && strcmp(dir, "elif") == 0) {
            return;
        }
        skip_to_eol(c);
    }
}

static void handle_directive(compiler_t *c)
{
    skip_ws_nonnl(c);
    char dir[32];
    read_ident_raw(c, dir, sizeof(dir));
    skip_ws_nonnl(c);

    if (strcmp(dir, "define") == 0) {
        char name[MAX_IDENT];
        read_ident_raw(c, name, sizeof(name));
        if (c->num_macros >= MAX_MACROS) {
            compile_error(c, "too many macros"); return;
        }
        macro_t *m = &c->macros[c->num_macros++];
        memset(m, 0, sizeof(*m));
        strncpy(m->name, name, MAX_IDENT - 1);
        m->defined = 1;
        m->num_params = -1;

        /* function-like: '(' immediately after name (no space) */
        if (*c->pos == '(') {
            c->pos++;
            m->num_params = 0;
            skip_ws_nonnl(c);
            while (*c->pos && *c->pos != ')' && *c->pos != '\n') {
                if (m->num_params >= MAX_MACRO_PARAMS) {
                    compile_error(c, "too many macro params"); return;
                }
                char pname[MAX_IDENT];
                read_ident_raw(c, pname, sizeof(pname));
                strncpy(m->params[m->num_params], pname, MAX_IDENT - 1);
                m->num_params++;
                skip_ws_nonnl(c);
                if (*c->pos == ',') { c->pos++; skip_ws_nonnl(c); }
            }
            if (*c->pos == ')') c->pos++;
        }
        read_to_eol(c, m->body, sizeof(m->body));
        return;
    }

    if (strcmp(dir, "undef") == 0) {
        char name[MAX_IDENT];
        read_ident_raw(c, name, sizeof(name));
        int mi = find_macro(c, name);
        if (mi >= 0) c->macros[mi].defined = 0;
        skip_to_eol(c);
        return;
    }

    if (strcmp(dir, "ifdef") == 0 || strcmp(dir, "ifndef") == 0) {
        int negate = (dir[2] == 'n');
        char name[MAX_IDENT];
        read_ident_raw(c, name, sizeof(name));
        int found = find_macro(c, name) >= 0;
        int active = negate ? !found : found;
        if (c->cond_depth >= MAX_COND_DEPTH) {
            compile_error(c, "conditional nesting too deep"); return;
        }
        c->cond_stack[c->cond_depth++] = active ? 1 : 0;
        skip_to_eol(c);
        if (!active) skip_conditional_block(c);
        return;
    }

    if (strcmp(dir, "if") == 0) {
        char expr_buf[256];
        read_to_eol(c, expr_buf, sizeof(expr_buf));
        int val = 0;
        if (strncmp(expr_buf, "defined", 7) == 0) {
            char *p = expr_buf + 7;
            while (*p == ' ' || *p == '(') p++;
            char nm[MAX_IDENT]; int n = 0;
            while (isalnum((unsigned char)*p) || *p == '_')
                nm[n++] = *p++;
            nm[n] = '\0';
            val = find_macro(c, nm) >= 0 ? 1 : 0;
        } else {
            val = atoi(expr_buf);
        }
        if (c->cond_depth >= MAX_COND_DEPTH) {
            compile_error(c, "conditional nesting too deep"); return;
        }
        c->cond_stack[c->cond_depth++] = val ? 1 : 0;
        if (!val) skip_conditional_block(c);
        return;
    }

    if (strcmp(dir, "elif") == 0) {
        if (c->cond_depth <= 0) { compile_error(c, "#elif without #if"); return; }
        int prev = c->cond_stack[c->cond_depth - 1];
        if (prev == 1) {
            c->cond_stack[c->cond_depth - 1] = 2;
            skip_to_eol(c);
            skip_conditional_block(c);
        } else if (prev == 0) {
            char expr_buf[256];
            read_to_eol(c, expr_buf, sizeof(expr_buf));
            int val = 0;
            if (strncmp(expr_buf, "defined", 7) == 0) {
                char *p = expr_buf + 7;
                while (*p == ' ' || *p == '(') p++;
                char nm[MAX_IDENT]; int n = 0;
                while (isalnum((unsigned char)*p) || *p == '_') nm[n++] = *p++;
                nm[n] = '\0';
                val = find_macro(c, nm) >= 0 ? 1 : 0;
            } else { val = atoi(expr_buf); }
            c->cond_stack[c->cond_depth - 1] = val ? 1 : 0;
            if (!val) skip_conditional_block(c);
        } else {
            skip_to_eol(c);
            skip_conditional_block(c);
        }
        return;
    }

    if (strcmp(dir, "else") == 0) {
        if (c->cond_depth <= 0) { compile_error(c, "#else without #if"); return; }
        int prev = c->cond_stack[c->cond_depth - 1];
        if (prev == 1) {
            c->cond_stack[c->cond_depth - 1] = 2;
            skip_to_eol(c);
            skip_conditional_block(c);
        } else if (prev == 0) {
            c->cond_stack[c->cond_depth - 1] = 1;
        } else {
            skip_to_eol(c);
            skip_conditional_block(c);
        }
        return;
    }

    if (strcmp(dir, "endif") == 0) {
        if (c->cond_depth <= 0) { compile_error(c, "#endif without #if"); return; }
        c->cond_depth--;
        skip_to_eol(c);
        return;
    }

    if (strcmp(dir, "include") == 0) {
        skip_ws_nonnl(c);
        if (*c->pos != '"') {
            skip_to_eol(c); return; /* skip #include <...> silently */
        }
        c->pos++;
        char fname[256];
        int n = 0;
        while (*c->pos && *c->pos != '"' && *c->pos != '\n') {
            if (n < (int)sizeof(fname) - 1) fname[n++] = *c->pos;
            c->pos++;
        }
        fname[n] = '\0';
        if (*c->pos == '"') c->pos++;
        skip_to_eol(c);

        if (!c->reader) {
            compile_error(c, "no file reader for #include \"%s\"", fname); return;
        }

        char *inc = c->reader(fname, c->reader_data);
        if (!inc) {
            compile_error(c, "cannot read \"%s\"", fname); return;
        }

        char *fn_copy = (char *)malloc(strlen(fname) + 1);
        if (fn_copy) strcpy(fn_copy, fname);
        push_source_file(c, inc, inc, fn_copy ? fn_copy : fname);
        return;
    }

    /* Unknown directive — skip */
    skip_to_eol(c);
}

/* ── Macro expansion via src_stack ─────────────────────────────── */

/*
 * Expand a function-like macro: collect args from c->pos, substitute
 * params in body, push result onto src_stack.
 */
static void expand_func_macro(compiler_t *c, macro_t *m)
{
    /* c->pos should be at '(' after optional whitespace */
    skip_ws_nonnl(c);
    if (*c->pos != '(') return; /* not a call — treat ident as-is */
    c->pos++; /* skip ( */

    char args[MAX_MACRO_PARAMS][MAX_MACRO_BODY];
    int nargs = 0;
    memset(args, 0, sizeof(args));

    /* Collect arguments */
    skip_ws_nonnl(c);
    if (*c->pos != ')' && m->num_params > 0) {
        while (nargs < m->num_params) {
            int depth = 0, n = 0;
            skip_ws_nonnl(c);
            while (*c->pos) {
                if (*c->pos == '(') {
                    depth++;
                    if (n < MAX_MACRO_BODY-1) args[nargs][n++] = *c->pos;
                    c->pos++;
                } else if (*c->pos == ')') {
                    if (depth == 0) break;
                    depth--;
                    if (n < MAX_MACRO_BODY-1) args[nargs][n++] = *c->pos;
                    c->pos++;
                } else if (*c->pos == ',' && depth == 0) {
                    c->pos++; break;
                } else {
                    if (*c->pos == '\n') c->line++;
                    if (n < MAX_MACRO_BODY-1) args[nargs][n++] = *c->pos;
                    c->pos++;
                }
            }
            args[nargs][n] = '\0';
            /* trim whitespace */
            while (n > 0 && (args[nargs][n-1] == ' ' || args[nargs][n-1] == '\t'))
                args[nargs][--n] = '\0';
            char *s = args[nargs];
            while (*s == ' ' || *s == '\t') s++;
            if (s != args[nargs]) memmove(args[nargs], s, strlen(s)+1);
            nargs++;
        }
    }
    if (*c->pos == ')') c->pos++;

    /* Substitute parameters in body → heap-allocated result */
    int body_len = (int)strlen(m->body);
    const char *body = m->body;
    char *out = (char *)malloc(MAX_MACRO_BODY * 4);
    if (!out) { compile_error(c, "out of memory"); return; }
    int out_len = 0;

    for (int i = 0; i < body_len; ) {
        if (isalpha((unsigned char)body[i]) || body[i] == '_') {
            char word[MAX_IDENT];
            int wn = 0;
            while (i < body_len && (isalnum((unsigned char)body[i]) || body[i] == '_')) {
                if (wn < MAX_IDENT - 1) word[wn++] = body[i];
                i++;
            }
            word[wn] = '\0';

            int found_param = -1;
            for (int p = 0; p < m->num_params; p++) {
                if (strcmp(word, m->params[p]) == 0) { found_param = p; break; }
            }

            if (found_param >= 0 && found_param < nargs) {
                const char *arg = args[found_param];
                int alen = (int)strlen(arg);
                if (out_len + alen < MAX_MACRO_BODY * 4 - 1) {
                    memcpy(out + out_len, arg, alen);
                    out_len += alen;
                }
            } else {
                if (out_len + wn < MAX_MACRO_BODY * 4 - 1) {
                    memcpy(out + out_len, word, wn);
                    out_len += wn;
                }
            }
        } else {
            if (out_len < MAX_MACRO_BODY * 4 - 1)
                out[out_len++] = body[i];
            i++;
        }
    }
    out[out_len] = '\0';

    /* Push expansion onto src_stack; c->pos now reads from it */
    push_source(c, out, out);
}

static void expand_object_macro(compiler_t *c, macro_t *m)
{
    char *buf = (char *)malloc(strlen(m->body) + 1);
    if (!buf) { compile_error(c, "out of memory"); return; }
    strcpy(buf, m->body);
    push_source(c, buf, buf);
}

/* ── Token reading ─────────────────────────────────────────────── */

void next_token(compiler_t *c)
{
    if (c->had_error) { c->tok = TOK_EOF; return; }

restart:
    skip_whitespace(c);

    /* Handle EOF / stack pop */
    if (peekch(c) == '\0') {
        if (c->src_depth > 0) {
            pop_source(c);
            goto restart;
        }
        c->tok = TOK_EOF;
        return;
    }

    /* Preprocessor directives (only in real source, not macro expansions).
     * Heuristic: directives start with '#' at the current pos. In macro
     * expansions this won't normally happen. */
    if (*c->pos == '#') {
        c->pos++;
        handle_directive(c);
        if (c->had_error) { c->tok = TOK_EOF; return; }
        goto restart;
    }

    char ch = peekch(c);

    /* Numbers */
    if (isdigit((unsigned char)ch)) {
        c->tok = TOK_NUM;
        c->tok_num = 0;

        char first = nextch(c);
        if (first == '0' && (peekch(c) == 'x' || peekch(c) == 'X')) {
            nextch(c); /* consume 'x' */
            while (isxdigit((unsigned char)peekch(c))) {
                char d = nextch(c);
                int v = 0;
                if (d >= '0' && d <= '9') v = d - '0';
                else if (d >= 'a' && d <= 'f') v = 10 + d - 'a';
                else if (d >= 'A' && d <= 'F') v = 10 + d - 'A';
                c->tok_num = c->tok_num * 16 + v;
            }
        } else if (first == '0' && peekch(c) >= '0' && peekch(c) <= '7') {
            /* octal */
            while (peekch(c) >= '0' && peekch(c) <= '7')
                c->tok_num = c->tok_num * 8 + (nextch(c) - '0');
        } else {
            c->tok_num = first - '0';
            while (isdigit((unsigned char)peekch(c)))
                c->tok_num = c->tok_num * 10 + (nextch(c) - '0');
        }
        /* skip type suffixes */
        while (peekch(c) == 'U' || peekch(c) == 'u' ||
               peekch(c) == 'L' || peekch(c) == 'l')
            nextch(c);
        return;
    }

    /* Character literal */
    if (ch == '\'') {
        nextch(c);
        c->tok = TOK_NUM;
        char v = nextch(c);
        if (v == '\\') {
            char e = nextch(c);
            switch (e) {
            case 'n':  c->tok_num = '\n'; break;
            case 't':  c->tok_num = '\t'; break;
            case '0':  c->tok_num = '\0'; break;
            case '\\': c->tok_num = '\\'; break;
            case '\'': c->tok_num = '\''; break;
            case 'r':  c->tok_num = '\r'; break;
            case 'x': {
                int hv = 0;
                for (int i = 0; i < 2 && isxdigit((unsigned char)peekch(c)); i++) {
                    char d = nextch(c);
                    if (d >= '0' && d <= '9') hv = hv*16 + d - '0';
                    else if (d >= 'a' && d <= 'f') hv = hv*16 + 10 + d - 'a';
                    else hv = hv*16 + 10 + d - 'A';
                }
                c->tok_num = hv;
                break;
            }
            default: c->tok_num = e; break;
            }
        } else {
            c->tok_num = (unsigned char)v;
        }
        if (peekch(c) == '\'') nextch(c);
        return;
    }

    /* String literal */
    if (ch == '"') {
        nextch(c);
        c->tok = TOK_STR;
        c->tok_str_len = 0;
        while (peekch(c) && peekch(c) != '"') {
            char sc = nextch(c);
            if (sc == '\\') {
                char e = nextch(c);
                switch (e) {
                case 'n':  sc = '\n'; break;
                case 't':  sc = '\t'; break;
                case '0':  sc = '\0'; break;
                case '\\': sc = '\\'; break;
                case '"':  sc = '"';  break;
                case 'r':  sc = '\r'; break;
                case 'x': {
                    int hv = 0;
                    for (int i = 0; i < 2 && isxdigit((unsigned char)peekch(c)); i++) {
                        char d = nextch(c);
                        if (d >= '0' && d <= '9') hv = hv*16 + d - '0';
                        else if (d >= 'a' && d <= 'f') hv = hv*16 + 10 + d - 'a';
                        else hv = hv*16 + 10 + d - 'A';
                    }
                    sc = (char)hv;
                    break;
                }
                default: sc = e; break;
                }
            }
            if (c->tok_str_len < (int)sizeof(c->tok_str) - 1)
                c->tok_str[c->tok_str_len++] = sc;
        }
        c->tok_str[c->tok_str_len] = '\0';
        if (peekch(c) == '"') nextch(c);

        /* Adjacent string literal concatenation */
        skip_whitespace(c);
        while (peekch(c) == '"') {
            nextch(c);
            while (peekch(c) && peekch(c) != '"') {
                char sc = nextch(c);
                if (sc == '\\') {
                    char e = nextch(c);
                    switch (e) {
                    case 'n':  sc = '\n'; break;
                    case 't':  sc = '\t'; break;
                    case '0':  sc = '\0'; break;
                    case '\\': sc = '\\'; break;
                    case '"':  sc = '"';  break;
                    case 'r':  sc = '\r'; break;
                    default:   sc = e;    break;
                    }
                }
                if (c->tok_str_len < (int)sizeof(c->tok_str) - 1)
                    c->tok_str[c->tok_str_len++] = sc;
            }
            c->tok_str[c->tok_str_len] = '\0';
            if (peekch(c) == '"') nextch(c);
            skip_whitespace(c);
        }
        return;
    }

    /* Identifier / keyword / macro */
    if (isalpha((unsigned char)ch) || ch == '_') {
        int n = 0;
        while (isalnum((unsigned char)peekch(c)) || peekch(c) == '_') {
            if (n < MAX_IDENT - 1) c->tok_ident[n++] = nextch(c);
            else nextch(c);
        }
        c->tok_ident[n] = '\0';
        c->tok = TOK_IDENT;

        /* Macro expansion */
        int mi = find_macro(c, c->tok_ident);
        if (mi >= 0) {
            macro_t *m = &c->macros[mi];
            if (m->num_params >= 0) {
                /* Function-like: check if '(' follows */
                const char *save = c->pos;
                int save_line = c->line;
                skip_ws_nonnl(c);
                if (*c->pos == '(') {
                    expand_func_macro(c, m);
                    goto restart;
                } else {
                    /* No '(' — not an invocation, return as ident */
                    c->pos = save;
                    c->line = save_line;
                    return;
                }
            } else {
                /* Object-like */
                expand_object_macro(c, m);
                goto restart;
            }
        }
        return;
    }

    /* Operators */
    nextch(c);
    char ch2 = peekch(c);

    switch (ch) {
    case '+':
        if (ch2 == '+') { nextch(c); c->tok = TOK_INC; return; }
        if (ch2 == '=') { nextch(c); c->tok = TOK_PLUS_ASSIGN; return; }
        c->tok = TOK_PLUS; return;
    case '-':
        if (ch2 == '-') { nextch(c); c->tok = TOK_DEC; return; }
        if (ch2 == '>') { nextch(c); c->tok = TOK_ARROW; return; }
        if (ch2 == '=') { nextch(c); c->tok = TOK_MINUS_ASSIGN; return; }
        c->tok = TOK_MINUS; return;
    case '*':
        if (ch2 == '=') { nextch(c); c->tok = TOK_STAR_ASSIGN; return; }
        c->tok = TOK_STAR; return;
    case '/':
        if (ch2 == '=') { nextch(c); c->tok = TOK_SLASH_ASSIGN; return; }
        c->tok = TOK_SLASH; return;
    case '%':
        if (ch2 == '=') { nextch(c); c->tok = TOK_PERCENT_ASSIGN; return; }
        c->tok = TOK_PERCENT; return;
    case '&':
        if (ch2 == '&') { nextch(c); c->tok = TOK_LAND; return; }
        if (ch2 == '=') { nextch(c); c->tok = TOK_AMP_ASSIGN; return; }
        c->tok = TOK_AMP; return;
    case '|':
        if (ch2 == '|') { nextch(c); c->tok = TOK_LOR; return; }
        if (ch2 == '=') { nextch(c); c->tok = TOK_PIPE_ASSIGN; return; }
        c->tok = TOK_PIPE; return;
    case '^':
        if (ch2 == '=') { nextch(c); c->tok = TOK_CARET_ASSIGN; return; }
        c->tok = TOK_CARET; return;
    case '<':
        if (ch2 == '<') {
            nextch(c);
            if (peekch(c) == '=') { nextch(c); c->tok = TOK_SHL_ASSIGN; return; }
            c->tok = TOK_SHL; return;
        }
        if (ch2 == '=') { nextch(c); c->tok = TOK_LE; return; }
        c->tok = TOK_LT; return;
    case '>':
        if (ch2 == '>') {
            nextch(c);
            if (peekch(c) == '=') { nextch(c); c->tok = TOK_SHR_ASSIGN; return; }
            c->tok = TOK_SHR; return;
        }
        if (ch2 == '=') { nextch(c); c->tok = TOK_GE; return; }
        c->tok = TOK_GT; return;
    case '=':
        if (ch2 == '=') { nextch(c); c->tok = TOK_EQ; return; }
        c->tok = TOK_ASSIGN; return;
    case '!':
        if (ch2 == '=') { nextch(c); c->tok = TOK_NE; return; }
        c->tok = TOK_BANG; return;
    case '~': c->tok = TOK_TILDE; return;
    case '(': c->tok = TOK_LPAREN; return;
    case ')': c->tok = TOK_RPAREN; return;
    case '{': c->tok = TOK_LBRACE; return;
    case '}': c->tok = TOK_RBRACE; return;
    case '[': c->tok = TOK_LBRACKET; return;
    case ']': c->tok = TOK_RBRACKET; return;
    case ';': c->tok = TOK_SEMICOLON; return;
    case ',': c->tok = TOK_COMMA; return;
    case '.':
        if (ch2 == '.' && peekch(c) == '.') {
            nextch(c); c->tok = TOK_ELLIPSIS; return;
        }
        c->tok = TOK_DOT; return;
    case ':': c->tok = TOK_COLON; return;
    case '#': c->tok = TOK_HASH; return;
    default:
        compile_error(c, "unexpected character '%c' (0x%02x)", ch, (unsigned char)ch);
        c->tok = TOK_EOF;
        return;
    }
}

/* ── Helpers ───────────────────────────────────────────────────── */

void expect(compiler_t *c, int tok)
{
    if (c->tok == tok) {
        next_token(c);
        return;
    }
    static const char *tok_names[] = {
        "EOF", "number", "string", "identifier",
        "+","-","*","/","%","&","|","^","~","!",
        "<",">","<=",">=","==","!=","&&","||","<<",">>",
        "=","+=","-=","*=","/=","%=","&=","|=","^=","<<=",">>=",
        "++","--","->",".",
        "(",")","{","}","[","]",";",",",":",
        "#","..."
    };
    const char *expected = (tok >= 0 && tok < (int)(sizeof(tok_names)/sizeof(tok_names[0])))
                           ? tok_names[tok] : "?";
    compile_error(c, "expected '%s'", expected);
}

bool match(compiler_t *c, int tok)
{
    if (c->tok == tok) { next_token(c); return true; }
    return false;
}

bool peek(compiler_t *c, int tok)
{
    return c->tok == tok;
}

bool is_type_token(compiler_t *c)
{
    if (c->tok != TOK_IDENT) return false;
    const char *id = c->tok_ident;
    if (strcmp(id, "void") == 0 || strcmp(id, "int") == 0 ||
        strcmp(id, "char") == 0 || strcmp(id, "short") == 0 ||
        strcmp(id, "long") == 0 ||
        strcmp(id, "unsigned") == 0 || strcmp(id, "signed") == 0 ||
        strcmp(id, "struct") == 0 || strcmp(id, "static") == 0 ||
        strcmp(id, "typedef") == 0 || strcmp(id, "const") == 0)
        return true;
    if (find_typedef(c, id) >= 0) return true;
    return false;
}

void lex_init(compiler_t *c, const char *source, const char *filename)
{
    c->pos = source;
    c->filename = filename;
    c->line = 1;
    c->expand_pos = NULL;
    c->expanding = 0;
    c->src_depth = 0;
    c->cond_depth = 0;
    next_token(c);
}
