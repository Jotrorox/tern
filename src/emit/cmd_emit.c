/* cmd_emit.c — `tern emit` (internal): emit SystemVerilog, constraints and
 * regmap outputs from an IR JSON file (docs/ir-schema.md), for tests and
 * external tools.  `tern build` produces the same files from source.
 *
 *     tern emit --ir design.json [--std sv|verilog2005] [--emit-assertions]
 *               [--target-kind fpga|asic] [--mem-init mif,coe]
 *               [--constraints xdc,sdc|none] [--no-regmaps] [--no-line-markers]
 *               [-o <dir> | --stdout]
 */
#include "../base/common.h"
#include "../base/arena.h"
#include "../base/str.h"
#include "../base/diag.h"
#include "../base/args.h"
#include "../os/os.h"
#include "../ir/ir.h"
#include "../cmds.h"
#include "emit.h"

static int emitcmd_list(ArgParser *ap, const char *val, const char *what, unsigned *out, const char *const *names,
                        const unsigned *bits, size_t n)
{
    const char *p = val;
    *out = 0;
    while (*p) {
        const char *e = p;
        size_t i, len;
        int ok = 0;
        while (*e && *e != ',')
            e++;
        len = (size_t)(e - p);
        for (i = 0; i < n; i++)
            if (strlen(names[i]) == len && strncmp(names[i], p, len) == 0) {
                *out |= bits[i];
                ok = 1;
            }
        if (!ok) {
            args_error(ap, "invalid %s `%.*s`", what, (int)len, p);
            return 0;
        }
        p = *e ? e + 1 : e;
    }
    return 1;
}

int cmd_emit(int argc, char **argv)
{
    ArgParser ap;
    const char *ir = NULL, *outdir = NULL, *val = NULL;
    int to_stdout = 0, rc = TERN_EXIT_OK;
    SvOptions opt;
    char *text = NULL;
    size_t len = 0;
    Arena arena;
    IrDesign *d;
    SvOutput out;
    static const char *const mem_names[] = {"hex", "mif", "coe"};
    static const unsigned mem_bits[] = {0, SV_MEMINIT_MIF, SV_MEMINIT_COE};
    static const char *const cons_names[] = {"none", "xdc", "sdc"};
    static const unsigned cons_bits[] = {0, EMIT_CONS_XDC, EMIT_CONS_SDC};

    sv_options_init(&opt);
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            cmd_print_usage("emit");
            return TERN_EXIT_OK;
        } else if (args_opt(&ap, "ir", &ir)) {
        } else if (args_opt(&ap, "output|o", &outdir)) {
        } else if (args_flag(&ap, "stdout")) {
            to_stdout = 1;
        } else if (args_opt(&ap, "std", &val)) {
            if (strcmp(val, "sv") == 0 || strcmp(val, "sv2012") == 0 || strcmp(val, "systemverilog") == 0)
                opt.std = SV_STD_SV2012;
            else if (strcmp(val, "verilog2005") == 0 || strcmp(val, "v2005") == 0)
                opt.std = SV_STD_V2005;
            else
                args_error(&ap, "invalid --std `%s` (sv or verilog2005)", val);
        } else if (args_flag(&ap, "emit-assertions")) {
            opt.emit_assertions = 1;
        } else if (args_opt(&ap, "target-kind", &val)) {
            if (strcmp(val, "fpga") == 0)
                opt.target_kind = SV_TARGET_FPGA;
            else if (strcmp(val, "asic") == 0)
                opt.target_kind = SV_TARGET_ASIC;
            else
                args_error(&ap, "invalid --target-kind `%s` (fpga or asic)", val);
        } else if (args_opt(&ap, "mem-init", &val)) {
            emitcmd_list(&ap, val, "--mem-init format", &opt.mem_init_formats, mem_names, mem_bits, 3);
        } else if (args_opt(&ap, "constraints", &val)) {
            emitcmd_list(&ap, val, "--constraints kind", &opt.constraints, cons_names, cons_bits, 3);
        } else if (args_flag(&ap, "no-regmaps")) {
            opt.regmaps = 0;
        } else if (args_flag(&ap, "no-line-markers")) {
            opt.line_markers = 0;
        } else if (args_positional(&ap) && !ir) {
            ir = ap.arg;
        } else {
            args_unknown(&ap);
        }
    }
    if (ap.errors)
        return TERN_EXIT_USAGE;
    if (!ir) {
        fprintf(stderr, "tern emit: error: no IR file (use --ir design.json)\n");
        return TERN_EXIT_USAGE;
    }
    if (outdir && to_stdout) {
        fprintf(stderr, "tern emit: error: -o and --stdout are mutually exclusive\n");
        return TERN_EXIT_USAGE;
    }
    if (os_read_file(ir, &text, &len) != 0) {
        diag_error(span_none(), "cannot read `%s`", ir);
        return TERN_EXIT_FAIL;
    }
    memset(&arena, 0, sizeof arena);
    memset(&out, 0, sizeof out);
    d = ir_from_json(&arena, text, len);
    free(text);
    if (!d) {
        arena_release(&arena);
        return TERN_EXIT_FAIL;
    }
    if (emit_design(d, &opt, &out) != 0)
        rc = TERN_EXIT_FAIL;
    if (rc == TERN_EXIT_OK) {
        if (outdir) {
            if (sv_output_write_dir(&out, outdir) != 0)
                rc = TERN_EXIT_FAIL;
        } else {
            StrBuf sb = {0};
            sv_output_concat(&out, &sb);
            fwrite(sb.data ? sb.data : "", 1, sb.len, stdout);
            sb_free(&sb);
        }
    }
    sv_output_free(&out);
    arena_release(&arena);
    return rc;
}
