/*
 * rvcc — Minimal C compiler targeting uvm32 (RV32IM flat binary)
 *
 * Embeddable as a library: no stdio dependency.
 * File I/O is handled by the caller via a callback.
 */
#ifndef RVCC_H
#define RVCC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Result ──────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  *binary;       /* Flat binary output (caller frees via rvcc_result_free) */
    uint32_t  binary_len;
    char      error[512];   /* Error message (empty string if OK) */
    int       error_line;   /* Source line of error (0 if OK) */
} rvcc_result_t;

/* ── File reader callback ────────────────────────────────────────── */

/*
 * Called when the compiler needs to read an #include file.
 * Must return a malloc'd NUL-terminated string (compiler will free it),
 * or NULL on failure.
 */
typedef char *(*rvcc_file_reader_t)(const char *path, void *userdata);

/* ── Compile flags ───────────────────────────────────────────────── */

#define RVCC_OPT  1   /* enable peephole optimizer */

/* ── Compile ─────────────────────────────────────────────────────── */

/*
 * Compile C source to an RV32IM flat binary.
 *
 *   source      — NUL-terminated C source text
 *   filename    — source file name (for error messages; can be NULL)
 *   reader      — callback for #include resolution (can be NULL if no includes)
 *   reader_data — opaque pointer forwarded to reader
 *   flags       — compilation flags (0 or RVCC_OPT)
 *   result      — output: binary + error info
 *
 * Returns true on success, false on error (see result->error).
 */
bool rvcc_compile(const char *source, const char *filename,
                  rvcc_file_reader_t reader, void *reader_data,
                  uint32_t flags, rvcc_result_t *result);

/* Free resources in a result (binary buffer). Safe to call on a zeroed result. */
void rvcc_result_free(rvcc_result_t *result);

/* ── Header generation ───────────────────────────────────────────── */

/*
 * Generate a C header containing the binary as a const uint8_t array,
 * matching the format produced by the GCC toolchain makefile (xxd -i style).
 *
 *   binary     — flat binary data
 *   len        — length in bytes
 *   name       — base name for the array / guard (e.g. "hello")
 *   header_out — receives a malloc'd NUL-terminated string (caller frees)
 *   header_len — receives the length of the header string (can be NULL)
 *
 * Returns true on success.
 */
bool rvcc_gen_header(const uint8_t *binary, uint32_t len,
                     const char *name, char **header_out, uint32_t *header_len);

#ifdef __cplusplus
}
#endif

#endif /* RVCC_H */
