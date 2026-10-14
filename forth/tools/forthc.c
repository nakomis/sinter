/*
 * forthc — host command-line front end to the compiler.
 *
 *   forthc [-o FILE] [-x] [-r] [SOURCE]
 *
 * Compiles SOURCE (or standard input) and writes the raw image to FILE,
 * or as hex text with -x (the format of the .golden files in tests/golden). With -r
 * the image is also executed on this host (x86-64 Linux/macOS only) and
 * the result region summarised. Compile errors go to standard error in
 * exactly the form the agentic loop will feed back to the LLM.
 *
 * Disassemble an image with:
 *   objdump -D -b binary -m i386:x86-64 -M intel FILE
 */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exec.h"
#include "sinter_forth.h"

#define MAX_SOURCE (256u * 1024u)
#define MAX_IMAGE  (256u * 1024u)

static uint64_t get_cell(const uint8_t *region, unsigned off)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | region[off + (unsigned)i];
    }
    return v;
}

static void usage(void)
{
    fprintf(stderr, "usage: forthc [-o FILE] [-x] [-r] [SOURCE]\n");
}

int main(int argc, char **argv)
{
    static char src[MAX_SOURCE];
    static uint8_t image[MAX_IMAGE];
    static forth_ctx ctx;
    static uint8_t region[FORTH_REGION_SIZE];
    const char *out_path = NULL, *in_path = NULL;
    int hex = 0, run = 0;
    size_t src_len;
    FILE *in, *out;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "-x") == 0) {
            hex = 1;
        } else if (strcmp(argv[i], "-r") == 0) {
            run = 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            usage();
            return 2;
        } else if (in_path == NULL) {
            in_path = argv[i];
        } else {
            usage();
            return 2;
        }
    }

    in = in_path ? fopen(in_path, "rb") : stdin;
    if (in == NULL) {
        perror(in_path);
        return 2;
    }
    src_len = fread(src, 1, sizeof src, in);
    if (in != stdin) {
        fclose(in);
    }
    if (src_len == sizeof src) {
        fprintf(stderr, "forthc: source larger than %u bytes\n", MAX_SOURCE);
        return 2;
    }

    if (forth_compile(&ctx, src, src_len, image, sizeof image) != FORTH_OK) {
        fprintf(stderr, "%s: %s\n", in_path ? in_path : "<stdin>",
                ctx.error.message);
        return 1;
    }

    if (out_path != NULL) {
        out = fopen(out_path, hex ? "w" : "wb");
        if (out == NULL) {
            perror(out_path);
            return 2;
        }
        if (hex) {
            for (size_t i = 0; i < ctx.code_len; i++) {
                fprintf(out, "%02x%s", image[i],
                        (i % 16 == 15 || i + 1 == ctx.code_len) ? "\n" : " ");
            }
        } else {
            fwrite(image, 1, ctx.code_len, out);
        }
        fclose(out);
    }

    if (run) {
        forth_run_status rs = forth_host_run(image, ctx.code_len, 0, region, 10);
        if (rs != FORTH_RUN_OK) {
            fprintf(stderr, "forthc: execution failed: %s\n",
                    forth_run_status_name(rs));
            return 3;
        }
        printf("result %lld done %llu depth %lld flags %llu\n",
               (long long)get_cell(region, FORTH_REG_RESULT),
               (unsigned long long)get_cell(region, FORTH_REG_DONE),
               (long long)get_cell(region, FORTH_REG_DEPTH),
               (unsigned long long)get_cell(region, FORTH_REG_FLAGS));
    } else if (out_path == NULL) {
        fprintf(stderr, "forthc: compiled %zu bytes (main at 0x%zx); "
                        "use -o to write them\n", ctx.code_len, ctx.main_offset);
    }
    return 0;
}
