/* cmd_sim.c — `tern sim <top> [--vcd file] [script] [paths...]` (spec §13.2):
 * an interactive stimulus REPL on stdin, or a stimulus script, against a
 * simulation of <top> (a module name, optionally with generic arguments:
 * 'Fifo<u8, 4>').
 *
 * The design is simulated as `dut` in a test-interpreter session
 * (src/sim/testrun.h), so every line is either a command or Tern statements
 * with the semantics of a test body:
 *
 *     poke <path> <expr>     dut.<path> = <expr>       (also `<path> = <expr>`)
 *     peek <path>...         print dut.<path>          (also a bare `<path>`)
 *     step [n] | run <ns> | reset | settle | dump <file.vcd>
 *     signals | help | quit
 *     <statements>           e.g. `expect dut.y == 3`, `log("{}", dut.y)`,
 *                            `for i in 0..4 { dut.a = i; dut.step() }`;
 *                            a trailing expression is printed
 *
 * `#` and `//` start comments. A script stops at the first line that fails
 * (error, failed expect/assert, design assertion) with exit status 1. */
#include "../base/common.h"
#include "../base/args.h"
#include "../base/str.h"
#include "../base/vec.h"
#include "../base/diag.h"
#include "../syntax/parser.h"
#include "../driver.h"
#include "../cmds.h"
#include "../os/os.h"
#include "testrun.h"

typedef struct SimCmd {
    Driver *dr;
    TestSession *ts;
    StrBuf report;            /* session diagnostics / failures */
    Arena ast;                /* ASTs of the executed lines */
    int interactive;
    uint32_t lineno;
    const char *srcname;      /* script path or "<stdin>" (diagnostic locations) */
} SimCmd;

static void simc_help(void)
{
    printf("commands (the design is `dut`; paths are relative to it):\n"
           "  poke <path> <expr>    drive an input: dut.<path> = <expr>   (or: <path> = <expr>)\n"
           "  peek <path>...        print signals                          (or: <path>)\n"
           "  step [n]              advance n cycles of the default domain (default 1)\n"
           "  run <ns>              advance all clocks by <ns> nanoseconds\n"
           "  reset                 assert the resets for 2 cycles, then release\n"
           "  settle                evaluate combinational logic\n"
           "  dump <file.vcd>       record a VCD waveform from now on\n"
           "  signals               list the signals of the design\n"
           "  help, quit\n"
           "anything else runs as Tern statements of a test body, e.g.\n"
           "  expect dut.count == 3      log(\"y = {}\", dut.y)      dut.step_until(dut.done, max: 100)\n");
}

static int simc_is_ident_path(const char *s)
{
    if (!tern_is_alpha((unsigned char)*s) && *s != '_')
        return 0;
    for (; *s; s++)
        if (!tern_is_ident((unsigned char)*s) && *s != '.' && *s != '[' && *s != ']')
            return 0;
    return 1;
}

static void simc_flush_report(SimCmd *c)
{
    if (c->report.len) {
        const char *p = sb_cstr(&c->report);
        /* the report is indented for `tern test`; print it flush left */
        while (*p) {
            const char *e = strchr(p, '\n');
            if (!e)
                e = p + strlen(p);
            if (strncmp(p, "    ", 4) == 0)
                p += 4;
            fwrite(p, 1, (size_t)(e - p), stdout);
            putchar('\n');
            p = *e ? e + 1 : e;
        }
        sb_reset(&c->report);
    }
    fflush(stdout);
}

/* runs Tern statements; returns 1 on success. `label` (or NULL) is printed
 * before the value of a trailing expression (`peek`), only when it runs. */
static int simc_exec(SimCmd *c, const char *code, const char *label)
{
    StrBuf src = {0};
    StrBuf out = {0};
    uint32_t id, i;
    Block *body;
    int ok, nerr = 0;
    /* The statements are parsed as a block body; leading newlines put them
     * on their line of the script, so diagnostics read `stim.txt:7:1` and
     * the excerpt shows the script line. */
    for (i = 1; i < c->lineno; i++)
        sb_putc(&src, '\n');
    sb_printf(&src, "%s\n", code);
    id = src_add(c->lineno ? c->srcname : "<command line>", src.data, src.len);
    sb_free(&src);
    body = parse_block_string(&c->ast, id, &nerr);
    diag_flush();
    if (nerr) {
        simc_flush_report(c);
        return 0;
    }
    ok = testrun_session_exec(c->ts, body, &out);
    simc_flush_report(c);
    if (out.len) {
        if (label)
            printf("%s = ", label);
        fputs(sb_cstr(&out), stdout);
    }
    sb_free(&out);
    if (!ok)
        testrun_session_clear(c->ts);
    fflush(stdout);
    return ok;
}

/* one input line; returns 1 ok, 0 failed, -1 quit */
static int simc_line(SimCmd *c, char *line)
{
    char *p = line, *cmd, *rest;
    size_t n = strlen(line);
    StrBuf code = {0};
    int r;
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t'))
        line[--n] = 0;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '#' || (p[0] == '/' && p[1] == '/'))
        return 1;
    cmd = p;
    while (*p && *p != ' ' && *p != '\t')
        p++;
    rest = p;
    while (*rest == ' ' || *rest == '\t')
        rest++;
    {
        size_t cl = (size_t)(p - cmd);
        char word[256];
        int word_path;
        snprintf(word, sizeof word, "%.*s", (int)TERN_MIN(cl, sizeof word - 1), cmd);
        word_path = cl < sizeof word && simc_is_ident_path(word) && strcmp(word, "dut") != 0 &&
                    strncmp(word, "dut.", 4) != 0 && strncmp(word, "dut[", 4) != 0;
#define SIMC_IS(w) (cl == strlen(w) && strncmp(cmd, w, cl) == 0)
        if (SIMC_IS("quit") || SIMC_IS("exit") || SIMC_IS("q"))
            return -1;
        if (SIMC_IS("help") || SIMC_IS("?")) {
            simc_help();
            return 1;
        }
        if (SIMC_IS("signals")) {
            StrBuf sb = {0};
            testrun_session_signals(c->ts, "dut", &sb);
            fputs(sb_cstr(&sb), stdout);
            sb_free(&sb);
            return 1;
        }
        if (SIMC_IS("step"))
            sb_printf(&code, "dut.step(%s)", *rest ? rest : "1");
        else if (SIMC_IS("run"))
            sb_printf(&code, "dut.run(ns: %s)", rest);
        else if (SIMC_IS("reset"))
            sb_puts(&code, "dut.reset()");
        else if (SIMC_IS("settle"))
            sb_puts(&code, "dut.settle()");
        else if (SIMC_IS("dump")) {
            const char *q = rest;
            sb_puts(&code, "dut.dump(\"");
            for (; *q; q++) {
                if (*q == '"' || *q == '\\')
                    sb_putc(&code, '\\');
                sb_putc(&code, *q);
            }
            sb_puts(&code, "\")");
        } else if (SIMC_IS("poke")) {
            char *e = rest;
            while (*e && *e != ' ' && *e != '\t')
                e++;
            if (!*e) {
                printf("usage: poke <path> <expr>\n");
                return 0;
            }
            *e++ = 0;
            sb_printf(&code, "dut.%s = %s", rest, e);
        } else if (SIMC_IS("peek")) {
            /* one `path = value` line per argument */
            char *q = rest;
            int ok = 1;
            if (!*q) {
                printf("usage: peek <path>...\n");
                return 0;
            }
            while (*q) {
                char *e = q;
                StrBuf one = {0};
                while (*e && *e != ' ' && *e != '\t')
                    e++;
                if (*e)
                    *e++ = 0;
                sb_printf(&one, "dut.%s", q);
                if (!simc_exec(c, sb_cstr(&one), q))
                    ok = 0;
                sb_free(&one);
                q = e;
                while (*q == ' ' || *q == '\t')
                    q++;
            }
            return ok;
        } else if (word_path && !*rest) {
            sb_printf(&code, "dut.%s", word);
            r = simc_exec(c, sb_cstr(&code), word);
            sb_free(&code);
            return r;
        } else if (word_path && rest[0] == '=' && rest[1] != '=') {
            sb_printf(&code, "dut.%s %s", word, rest);
        } else {
            sb_puts(&code, cmd);
        }
#undef SIMC_IS
    }
    r = simc_exec(c, sb_cstr(&code), NULL);
    sb_free(&code);
    return r;
}

int cmd_sim(int argc, char **argv)
{
    ArgParser ap;
    DriverOptions o;
    SimCmd c;
    VEC(const char *) paths = {0};
    const char *top = NULL, *script = NULL, *vcd = NULL, *val;
    TestRunOptions to;
    FileUnit *scope = NULL;
    uint64_t seed = 1;
    int rc = TERN_EXIT_OK;
    char *script_text = NULL;
    size_t script_len = 0;
    memset(&o, 0, sizeof o);
    memset(&c, 0, sizeof c);
    memset(&to, 0, sizeof to);
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            cmd_print_usage("sim");
            vec_free(paths);
            return TERN_EXIT_OK;
        } else if (args_opt(&ap, "vcd", &val))
            vcd = val;
        else if (args_opt(&ap, "script", &val))
            script = val;
        else if (args_opt_u64(&ap, "seed|s", &seed))
            ;
        else if (args_opt(&ap, "features", &val))
            o.features = val;
        else if (args_positional(&ap)) {
            if (!top && !os_is_dir(ap.arg) && strcmp(os_path_ext(ap.arg), ".tern") != 0)
                top = ap.arg;
            else if (os_is_dir(ap.arg) || strcmp(os_path_ext(ap.arg), ".tern") == 0)
                vec_push(paths, ap.arg);
            else if (!script)
                script = ap.arg;
            else {
                fprintf(stderr, "tern sim: error: unexpected argument `%s`\n", ap.arg);
                ap.errors++;
            }
        } else
            args_unknown(&ap);
    }
    if (ap.errors) {
        vec_free(paths);
        return TERN_EXIT_USAGE;
    }
    if (script && os_read_file(script, &script_text, &script_len) != 0) {
        fprintf(stderr, "tern sim: error: cannot read script `%s`\n", script);
        vec_free(paths);
        return TERN_EXIT_USAGE;
    }
    o.paths = paths.data;
    o.npaths = paths.len;
    o.include_tests = 0;
    o.cmd = "sim";
    c.dr = driver_open(&o);
    vec_free(paths);
    if (!c.dr) {
        free(script_text);
        return TERN_EXIT_USAGE;
    }
    if (driver_parse_errors(c.dr)) {
        rc = TERN_EXIT_FAIL;
        goto done;
    }
    driver_sema(c.dr)->record_info = 0;   /* no hover/definition info needed */
    if (!top) {
        DriverItemVec v = {0};
        driver_tops(c.dr, &v);
        if (v.len == 1) {
            top = v.data[0].item->name.name;
            scope = v.data[0].file;
        }
        vec_free(v);
        if (!top) {
            fprintf(stderr, "tern sim: error: name the module to simulate: tern sim <top> [files]\n");
            rc = TERN_EXIT_USAGE;
            goto done;
        }
    }
    if (!scope)
        scope = driver_module_scope(c.dr, top); /* its generic arguments resolve there */
    to.seed = testrun_test_seed(seed, "tern sim");
    c.ts = testrun_session_new(c.dr, scope, &to, &c.report);
    c.interactive = !script_text && os_isatty(0);
    c.srcname = script ? script : "<stdin>";
    {
        StrBuf init = {0};
        int ok;
        sb_printf(&init, "let dut = sim %s()", top);
        c.lineno = 0;
        ok = simc_exec(&c, sb_cstr(&init), NULL);
        sb_free(&init);
        if (ok && vcd) {
            StrBuf d = {0};
            sb_printf(&d, "dump %s", vcd);
            ok = simc_line(&c, d.data) == 1;
            sb_free(&d);
        }
        if (!ok) {
            rc = TERN_EXIT_FAIL;
            goto done;
        }
    }
    if (c.interactive)
        printf("simulating %s; `help` lists the commands\n", top);
    if (script_text) {
        char *p = script_text;
        while (*p) {
            char *e = strchr(p, '\n');
            int r;
            if (e)
                *e = 0;
            c.lineno++;
            r = simc_line(&c, p);
            if (r < 0)
                break;
            if (r == 0) {
                fprintf(stderr, "tern sim: %s:%u: stopped at this line\n", script, (unsigned)c.lineno);
                rc = TERN_EXIT_FAIL;
                break;
            }
            if (!e)
                break;
            p = e + 1;
        }
    } else {
        char buf[4096];
        for (;;) {
            int r;
            if (c.interactive) {
                fputs("tern> ", stdout);
                fflush(stdout);
            }
            if (!fgets(buf, sizeof buf, stdin))
                break;
            c.lineno++;
            r = simc_line(&c, buf);
            if (r < 0)
                break;
            if (r == 0 && !c.interactive) {
                /* piped stdin behaves like a script: stop, exit status 1 */
                fprintf(stderr, "tern sim: <stdin>:%u: stopped at this line\n", (unsigned)c.lineno);
                rc = TERN_EXIT_FAIL;
                break;
            }
        }
        if (c.interactive)
            putchar('\n');
    }
done:
    if (c.ts)
        testrun_session_free(c.ts);
    simc_flush_report(&c);
    sb_free(&c.report);
    driver_free(c.dr);
    arena_release(&c.ast);
    free(script_text);
    return rc;
}
