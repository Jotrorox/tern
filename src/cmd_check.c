/* cmd_check.c — `tern check [paths] [--strict] [--unused] [--cdc-report]
 * [--top M] [--const-eval-limit N] [--features a,-b]`: parse, resolve, elaborate every
 * root (the given --top, else every #[top], else every module that needs
 * no generic arguments, plus the modules the tests instantiate with
 * `sim`), run the IR checks (src/check/check.h) and report diagnostics.
 * The CDC report (every clock-domain crossing with its primitive) goes to
 * stdout when the design has crossings, or always with --cdc-report.
 * Exit status 1 when there are errors.
 *
 *     tern ir [--json] [--top M] [paths]     (internal, for the golden tests)
 * prints the elaborated design (ir_dump, or the JSON form of
 * docs/ir-schema.md) of one top: --top, else the first #[top], else the
 * first module that needs no generic arguments. */
#include "base/common.h"
#include "base/args.h"
#include "base/arena.h"
#include "base/str.h"
#include "base/vec.h"
#include "base/diag.h"
#include "driver.h"
#include "check/check.h"
#include "cmds.h"
#include "os/os.h"

typedef VEC(const char *) CmdCheckPaths;

/* --verbose: what was elaborated, and which user modules never were */
static void cmd_check_report(Driver *dr, IrDesign *d)
{
    Sema *S = driver_sema(dr);
    size_t i;
    uint32_t k, m, never = 0, nmods = 0;
    for (i = 0; i < S->files.len; i++) {
        FileUnit *f = S->files.data[i];
        if (!f->is_user)
            continue;
        for (k = 0; k < f->ast->nitems; k++) {
            Item *it = f->ast->items[k];
            int found = 0;
            if (it->kind != ITEM_MODULE && it->kind != ITEM_EXTERN)
                continue;
            nmods++;
            for (m = 0; m < d->nmodules; m++)
                if (d->modules[m]->tern_name == it->name.name && d->modules[m]->loc.file == it->name.span.file)
                    found = 1;
            if (!found) {
                fprintf(stderr, "tern check: not elaborated: %s (%s)\n", it->name.name, f->ast->path);
                never++;
            }
        }
    }
    fprintf(stderr, "tern check: %u modules declared, %u instantiations elaborated, %u never elaborated\n",
            (unsigned)nmods, (unsigned)d->nmodules, (unsigned)never);
}

/* --hover LINE:COL[,LINE:COL...]: the semantic info (src/sema/sema.h) at
 * positions of the first user file, one line each (tests the LSP data) */
static void cmd_check_hover(Driver *dr, const char *spec)
{
    Sema *S = driver_sema(dr);
    FileUnit *f = NULL;
    size_t i;
    const char *p = spec;
    for (i = 0; i < S->files.len; i++)
        if (S->files.data[i]->is_user) {
            f = S->files.data[i];
            break;
        }
    if (!f)
        return;
    while (*p) {
        unsigned line = 0, col = 0;
        const SourceFile *sf = src_get(f->ast->src);
        const SemInfo *si = NULL;
        uint32_t off = 0;
        StrBuf sb = {0};
        while (*p >= '0' && *p <= '9')
            line = line * 10 + (unsigned)(*p++ - '0');
        if (*p == ':')
            p++;
        while (*p >= '0' && *p <= '9')
            col = col * 10 + (unsigned)(*p++ - '0');
        if (*p == ',')
            p++;
        if (sf && line >= 1 && line <= sf->nlines) {
            off = sf->lines[line - 1] + (col ? col - 1 : 0);
            si = sema_info_at(S, f->ast->src, off);
        }
        if (si) {
            sema_info_hover(S, si, &sb);
            if (si->def.file) {
                const SourceFile *df = src_get(si->def.file);
                sb_printf(&sb, " [def %s:%u:%u]", df ? os_path_basename(df->path) : "?", (unsigned)si->def.line,
                          (unsigned)si->def.col);
            }
        } else {
            sb_puts(&sb, "(nothing)");
        }
        printf("%u:%u: %s\n", line, col, sb_cstr(&sb));
        sb_free(&sb);
        if (!*p || (*p < '0' || *p > '9'))
            break;
    }
}

/* --unused reports only signals declared in the user's files */
static int cmd_check_is_user_file(uint32_t file, void *user)
{
    Sema *S = driver_sema((Driver *)user);
    size_t i;
    for (i = 0; i < S->files.len; i++)
        if (S->files.data[i]->ast && S->files.data[i]->ast->src == file)
            return S->files.data[i]->is_user;
    return 0;
}

static int cmd_check_common(int argc, char **argv, int ir_mode)
{
    ArgParser ap;
    CmdCheckPaths paths = {0};
    DriverOptions o;
    CheckOptions co;
    Driver *dr;
    const char *top = NULL, *val = NULL, *hover = NULL;
    int json = 0, rc, verbose = 0;
    uint64_t limit = 0;
    Arena ira = {0};
    memset(&o, 0, sizeof o);
    memset(&co, 0, sizeof co);
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            cmd_print_usage(ir_mode ? "ir" : "check");
            vec_free(paths);
            return TERN_EXIT_OK;
        } else if (args_flag(&ap, "unused"))
            co.unused = 1;
        else if (!ir_mode && args_flag(&ap, "cdc-report"))
            co.cdc_report = 1;
        else if (args_flag(&ap, "strict"))
            co.strict = 1;
        else if (args_flag(&ap, "verbose|v"))
            verbose = 1;
        else if (ir_mode && args_flag(&ap, "json"))
            json = 1;
        else if (ir_mode && args_opt(&ap, "hover", &val))
            hover = val;
        else if (args_opt(&ap, "top", &val))
            top = val;
        else if (args_opt(&ap, "features", &val))
            o.features = val;
        else if (args_opt_u64(&ap, "const-eval-limit", &limit))
            ;
        else if (args_positional(&ap))
            vec_push(paths, ap.arg);
        else
            args_unknown(&ap);
    }
    if (ap.errors) {
        vec_free(paths);
        return TERN_EXIT_USAGE;
    }
    if (co.strict)
        diag_set_strict(1);
    o.paths = paths.data;
    o.npaths = paths.len;
    o.const_eval_limit = limit;
    o.cmd = ir_mode ? "ir" : "check";
    dr = driver_open(&o);
    vec_free(paths);
    if (!dr)
        return TERN_EXIT_USAGE;
    if (!driver_parse_errors(dr)) {
        if (ir_mode) {
            IrDesign *d = driver_elaborate_top(dr, top, &ira);
            if (d && hover) {
                cmd_check_hover(dr, hover);
            } else if (d) {
                StrBuf sb = {0};
                if (json)
                    ir_to_json(d, &sb);
                else
                    ir_dump(d, &sb);
                fwrite(sb.data ? sb.data : "", 1, sb.len, stdout);
                sb_free(&sb);
            }
        } else {
            IrDesign *d = driver_elaborate_all(dr, top, &ira);
            co.is_user_file = cmd_check_is_user_file;
            co.user = dr;
            if (diag_error_count() == 0)
                check_design(d, &co);
            if (verbose)
                cmd_check_report(dr, d);
        }
    }
    rc = diag_error_count() ? TERN_EXIT_FAIL : TERN_EXIT_OK;
    driver_free(dr);
    arena_release(&ira);
    return rc;
}

int cmd_check(int argc, char **argv)
{
    return cmd_check_common(argc, argv, 0);
}

int cmd_ir(int argc, char **argv)
{
    return cmd_check_common(argc, argv, 1);
}
