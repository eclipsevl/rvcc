/*
 * rvcc_api.c — public API: rvcc_compile, rvcc_result_free, rvcc_gen_header
 */
#include "rvcc_internal.h"
#include <stdio.h>
#include <string.h>

/* ── Finalization: patch forward references, emit data ─────────── */

static void emit_startup(compiler_t *c)
{
    /*
     * Startup code at offset 0 (16 bytes):
     *   0x00: sw   ra, 12(sp)
     *   0x04: jal  ra, <main offset>    (patched below)
     *   0x08: lui  a7, 0x1000
     *   0x0C: ecall
     */
    /* sw ra, 12(sp) */
    patch32(c, 0, rv_s(0x23, 2, REG_SP, REG_RA, 12));

    /* jal ra, main — find main function */
    int main_id = find_func(c, "main");
    if (main_id < 0 || !c->funcs[main_id].defined) {
        compile_error(c, "undefined function 'main'");
        return;
    }
    int main_offset = (int)c->funcs[main_id].addr - 4; /* relative to instruction at offset 4 */
    patch32(c, 4, rv_j(0x6F, REG_RA, main_offset));

    /* lui a7, 0x1000 → a7 = 0x01000000 */
    patch32(c, 8, rv_u(0x37, REG_A7, 0x01000000));

    /* ecall */
    patch32(c, 12, 0x00000073);
}

static void finalize(compiler_t *c)
{
    /* Align code to 4 bytes */
    while (c->code_len & 3)
        emit8(c, 0);

    uint32_t data_base_offset = c->code_len; /* offset within binary where data starts */

    /* Emit string literals into data section */
    for (int i = 0; i < c->num_strings; i++) {
        /* Align strings to 4 bytes within data */
        data_align4(c);
        c->strings[i].addr = RAM_BASE + data_base_offset + c->data_len;
        for (int j = 0; j < c->strings[i].len; j++)
            data_emit8(c, (uint8_t)c->strings[i].data[j]);
    }

    /* Compute global addresses */
    /* Globals were placed in data section from offset 0.
     * Their actual address = RAM_BASE + data_base_offset + data_offset */
    /* Note: globals were added to data section during parsing, before strings.
     * Wait — actually globals and strings share the data section. Let me check.
     * Globals are added during parse_top_level via data_emit8 calls.
     * Strings are added above. So globals come first, then strings. Good. */

    /* Patch string literal references */
    for (int i = 0; i < c->num_string_patches; i++) {
        string_patch_t *sp = &c->string_patches[i];
        uint32_t addr = c->strings[sp->string_id].addr;
        int32_t upper = (int32_t)(addr + 0x800) >> 12; /* compensate for sign-extend */
        int32_t lower = (int32_t)(addr & 0xFFF);
        if (lower & 0x800) lower -= 0x1000; /* sign-extend */

        patch32(c, sp->lui_offset, rv_u(0x37, REG_A0, upper << 12));
        patch32(c, sp->addi_offset, rv_i(0x13, REG_A0, 0, REG_A0, lower & 0xFFF));
    }

    /* Patch global variable references */
    for (int i = 0; i < c->num_global_patches; i++) {
        global_patch_t *gp = &c->global_patches[i];
        if (gp->global_id < 0 || gp->global_id >= c->num_globals) continue;
        uint32_t addr = RAM_BASE + data_base_offset + c->globals[gp->global_id].data_offset;
        int32_t upper = (int32_t)(addr + 0x800) >> 12;
        int32_t lower = (int32_t)(addr & 0xFFF);
        if (lower & 0x800) lower -= 0x1000;

        patch32(c, gp->lui_offset, rv_u(0x37, REG_A0, upper << 12));
        patch32(c, gp->addi_offset, rv_i(0x13, REG_A0, 0, REG_A0, lower & 0xFFF));
    }

    /* Patch forward function call references */
    for (int i = 0; i < c->num_call_patches; i++) {
        call_patch_t *cp = &c->call_patches[i];
        if (cp->func_id < 0 || cp->func_id >= c->num_funcs) continue;
        func_t *f = &c->funcs[cp->func_id];
        if (!f->defined) {
            compile_error(c, "undefined function '%s'", f->name);
            return;
        }
        int offset = (int)f->addr - (int)cp->code_offset;
        patch32(c, cp->code_offset, rv_j(0x6F, REG_RA, offset));
    }

    /* Patch startup code */
    emit_startup(c);

    /* Append data section to code */
    for (uint32_t i = 0; i < c->data_len; i++)
        emit8(c, c->data[i]);
}

/* ── Public API ────────────────────────────────────────────────── */

bool rvcc_compile(const char *source, const char *filename,
                  rvcc_file_reader_t reader, void *reader_data,
                  uint32_t flags, rvcc_result_t *result)
{
    memset(result, 0, sizeof(*result));

    compiler_t c;
    memset(&c, 0, sizeof(c));

    c.reader = reader;
    c.reader_data = reader_data;
    c.current_func = -1;
    c.opt_level = (flags & RVCC_OPT) ? 1 : 0;

    emit_init(&c);
    data_init(&c);

    /* Reserve startup code area (16 bytes) */
    for (int i = 0; i < STARTUP_SIZE; i++)
        emit8(&c, 0);

    lex_init(&c, source, filename);

    /* Parse entire translation unit */
    parse_top_level(&c);

    if (c.had_error) goto fail;

    /* Optional peephole optimization pass */
    if (c.opt_level > 0)
        optimize(&c);

    if (c.had_error) goto fail;

    /* Finalize: patch references, append data */
    finalize(&c);

    if (c.had_error) goto fail;

    /* Build result */
    result->binary = c.code;
    result->binary_len = c.code_len;
    c.code = NULL; /* prevent free */

    /* Cleanup */
    free(c.data);
    for (int i = 0; i < c.num_strings; i++)
        free(c.strings[i].data);
    /* Free allocated type bases (leak — acceptable for a build tool) */

    return true;

fail:
    strncpy(result->error, c.error, sizeof(result->error) - 1);
    result->error_line = c.error_line;
    free(c.code);
    free(c.data);
    for (int i = 0; i < c.num_strings; i++)
        free(c.strings[i].data);
    return false;
}

void rvcc_result_free(rvcc_result_t *result)
{
    if (result->binary) {
        free(result->binary);
        result->binary = NULL;
    }
    result->binary_len = 0;
}

bool rvcc_gen_header(const uint8_t *binary, uint32_t len,
                     const char *name, char **header_out, uint32_t *header_len)
{
    /* Estimate output size: header boilerplate + 6 bytes per binary byte */
    uint32_t cap = 512 + len * 6;
    char *buf = (char *)malloc(cap);
    if (!buf) return false;

    int pos = 0;

    /* Upper-case guard name */
    char upper[MAX_IDENT];
    int i;
    for (i = 0; name[i] && i < MAX_IDENT - 1; i++)
        upper[i] = (name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i];
    upper[i] = '\0';

    pos += snprintf(buf + pos, cap - pos,
        "// Auto-generated from %s.bin\n"
        "#ifndef %s_ROM_H\n"
        "#define %s_ROM_H\n"
        "\n"
        "#include <stdint.h>\n"
        "\n"
        "static const uint8_t %s_rom[] = {\n",
        name, upper, upper, name);

    /* xxd -i style hex dump */
    for (uint32_t j = 0; j < len; j++) {
        if (j % 12 == 0)
            pos += snprintf(buf + pos, cap - pos, "  ");
        pos += snprintf(buf + pos, cap - pos, "0x%02x", binary[j]);
        if (j < len - 1)
            pos += snprintf(buf + pos, cap - pos, ", ");
        if (j % 12 == 11 || j == len - 1)
            pos += snprintf(buf + pos, cap - pos, "\n");
    }

    pos += snprintf(buf + pos, cap - pos,
        "};\n"
        "\n"
        "static const uint32_t %s_rom_len = sizeof(%s_rom);\n"
        "\n"
        "#endif\n",
        name, name);

    *header_out = buf;
    if (header_len) *header_len = (uint32_t)pos;
    return true;
}
