/*
 * rvcc CLI — compiles a C source file to RV32IM flat binary or C header.
 *
 * Usage:
 *   rvcc [-O] [-I dir] [-H name] [-o output] input.c
 *
 *   -O         Enable peephole optimizer
 *   -I dir     Add include search path (may be repeated)
 *   -H name    Generate C header instead of raw binary (name = array base name)
 *   -o output  Output file (default: stdout for header, input.bin for binary)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rvcc.h"

#define MAX_INCLUDE_DIRS 16

static const char *include_dirs[MAX_INCLUDE_DIRS];
static int num_include_dirs;

/* Try to read a file by searching include dirs, then relative to source dir */
static char *file_reader(const char *path, void *userdata) {
    const char *source_dir = (const char *)userdata;
    char fullpath[1024];
    FILE *f;

    /* Try include directories first */
    for (int i = 0; i < num_include_dirs; i++) {
        snprintf(fullpath, sizeof(fullpath), "%s/%s", include_dirs[i], path);
        f = fopen(fullpath, "rb");
        if (f) goto found;
    }

    /* Try relative to source file directory */
    if (source_dir && source_dir[0]) {
        snprintf(fullpath, sizeof(fullpath), "%s/%s", source_dir, path);
        f = fopen(fullpath, "rb");
        if (f) goto found;
    }

    /* Try current directory */
    snprintf(fullpath, sizeof(fullpath), "%s", path);
    f = fopen(fullpath, "rb");
    if (f) goto found;

    return NULL;

found:
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* Extract directory part of a path */
static void get_dir(const char *path, char *dir, int dirsize) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        dir[0] = '\0';
    } else {
        int len = (int)(slash - path);
        if (len >= dirsize) len = dirsize - 1;
        memcpy(dir, path, len);
        dir[len] = '\0';
    }
}

int main(int argc, char **argv) {
    const char *input = NULL;
    const char *output = NULL;
    const char *header_name = NULL;
    uint32_t flags = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-O") == 0) {
            flags |= RVCC_OPT;
        } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            if (num_include_dirs < MAX_INCLUDE_DIRS)
                include_dirs[num_include_dirs++] = argv[++i];
        } else if (strcmp(argv[i], "-H") == 0 && i + 1 < argc) {
            header_name = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else if (argv[i][0] != '-') {
            input = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (!input) {
        fprintf(stderr, "Usage: rvcc [-O] [-I dir] [-H name] [-o output] input.c\n");
        return 1;
    }

    char *source = read_file(input);
    if (!source) {
        fprintf(stderr, "Cannot read %s\n", input);
        return 1;
    }

    /* Source directory for relative includes */
    char source_dir[1024];
    get_dir(input, source_dir, sizeof(source_dir));

    rvcc_result_t result = {0};
    if (!rvcc_compile(source, input, file_reader, source_dir, flags, &result)) {
        fprintf(stderr, "%s:%d: error: %s\n",
                input, result.error_line, result.error);
        free(source);
        rvcc_result_free(&result);
        return 1;
    }
    free(source);

    if (header_name) {
        /* Generate C header */
        char *header;
        uint32_t header_len;
        if (!rvcc_gen_header(result.binary, result.binary_len,
                             header_name, &header, &header_len)) {
            fprintf(stderr, "Failed to generate header\n");
            rvcc_result_free(&result);
            return 1;
        }

        if (output) {
            FILE *f = fopen(output, "w");
            if (!f) {
                fprintf(stderr, "Cannot write %s\n", output);
                free(header);
                rvcc_result_free(&result);
                return 1;
            }
            fwrite(header, 1, header_len, f);
            fclose(f);
        } else {
            fwrite(header, 1, header_len, stdout);
        }
        free(header);
    } else {
        /* Write raw binary */
        const char *outpath = output;
        char default_out[1024];
        if (!outpath) {
            /* input.c -> input.bin */
            strncpy(default_out, input, sizeof(default_out) - 5);
            default_out[sizeof(default_out) - 5] = '\0';
            char *dot = strrchr(default_out, '.');
            if (dot) *dot = '\0';
            strcat(default_out, ".bin");
            outpath = default_out;
        }
        FILE *f = fopen(outpath, "wb");
        if (!f) {
            fprintf(stderr, "Cannot write %s\n", outpath);
            rvcc_result_free(&result);
            return 1;
        }
        fwrite(result.binary, 1, result.binary_len, f);
        fclose(f);
    }

    rvcc_result_free(&result);
    return 0;
}
