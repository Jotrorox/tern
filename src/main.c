/* main.c — the `tern` command-line entry point: global options, command
 * dispatch and help. */
#include "base/common.h"
#include "base/diag.h"
#include "os/os.h"
#include "cmds.h"

typedef int (*MainCmdFn)(int argc, char **argv);

typedef struct MainCmd {
    const char *name;
    MainCmdFn fn;
    const char *group;      /* help section; NULL = hidden (internal) */
    const char *synopsis;   /* arguments shown after the name */
    const char *summary;    /* one line */
    const char *details;    /* option list for `tern help <cmd>`, or NULL */
} MainCmd;

static const MainCmd main_cmds[] = {
    {"new", cmd_new, "Project", "<name>",
     "create a project skeleton (tern.toml, src/, tests/)", NULL},
    {"build", cmd_build, "Project", "[--target <t>] [--std sv|verilog2005]",
     "elaborate, check and emit build/<target>/*.sv, constraints, regmaps",
     "  --target <t>        build target from tern.toml ([target.<t>])\n"
     "  --std <s>           output dialect: sv (default) or verilog2005\n"},
    {"check", cmd_check, "Project", "[<file.tern>...]",
     "run every compile-time check without emitting; CDC report, unused signals", NULL},
    {"fmt", cmd_fmt, "Project", "[<file.tern>...]",
     "format sources canonically (no options)", NULL},
    {"doc", cmd_doc, "Project", "[<pkg>]",
     "generate static HTML docs from /// comments", NULL},
    {"test", cmd_test, "Verification", "[<filter>] [--waves] [--seed <n>] [--cover]",
     "run test blocks on the built-in simulator",
     "  <filter>            run only tests whose name contains <filter>\n"
     "  --waves             write a VCD waveform per test\n"
     "  --seed <n>          seed for randomized stimulus\n"
     "  --cover             report coverage counters\n"
     "  --backend <b>       builtin (default) or verilator\n"
     "  -j <n>              number of parallel workers\n"},
    {"sim", cmd_sim, "Verification", "<top> [--vcd <file>] [<script>]",
     "interactive stimulus REPL or a stimulus script against a top", NULL},
    {"prove", cmd_prove, "Verification", "<module> [--depth <n>] [--induct]",
     "formal verification via Yosys and SymbiYosys",
     "  --depth <n>         bounded model checking depth\n"
     "  --induct            also try k-induction for an unbounded proof\n"},
    {"synth", cmd_synth, "Verification", "[--target <t>]",
     "drive Yosys / Vivado / Quartus; utilization and timing summary", NULL},
    {"add", cmd_add, "Packages", "<pkg>[@<ver>] | <git-url>",
     "add a dependency (index short name or git URL); writes tern.lock", NULL},
    {"update", cmd_update, "Packages", "[<pkg>...]",
     "move dependencies to newer compatible tags", NULL},
    {"vendor", cmd_vendor, "Packages", "",
     "copy every dependency into vendor/", NULL},
    {"publish", cmd_publish, "Packages", "",
     "push the version tag and prepare the registry index entry", NULL},
    {"import", cmd_import, "Interop", "<file.sv>...",
     "generate extern module declarations from SystemVerilog", NULL},
    {"remap", cmd_remap, "Interop", "[<log>]",
     "translate synthesis/simulator logs to Tern source locations", NULL},
    {"lsp", cmd_lsp, "Interop", "",
     "language server on stdio: diagnostics, go-to-definition, hover", NULL},
    {"parse", cmd_parse, NULL, "--dump <file.tern>",
     "(internal) print the AST of a file", NULL},
};

static const char *const main_groups[] = {"Project", "Verification", "Packages", "Interop"};

static const MainCmd *main_find(const char *name)
{
    size_t i;
    for (i = 0; i < TERN_ARRAY_LEN(main_cmds); i++) {
        if (strcmp(main_cmds[i].name, name) == 0)
            return &main_cmds[i];
    }
    return NULL;
}

static void main_print_version(void)
{
    printf("tern %s\n", TERN_VERSION);
}

static void main_print_help(FILE *out)
{
    size_t g, i;
    fprintf(out,
            "tern %s - the Tern hardware description language\n"
            "\n"
            "Usage: tern <command> [options] [arguments]\n",
            TERN_VERSION);
    for (g = 0; g < TERN_ARRAY_LEN(main_groups); g++) {
        fprintf(out, "\n%s:\n", main_groups[g]);
        for (i = 0; i < TERN_ARRAY_LEN(main_cmds); i++) {
            const MainCmd *c = &main_cmds[i];
            if (!c->group || strcmp(c->group, main_groups[g]) != 0)
                continue;
            fprintf(out, "  %-9s %s\n", c->name, c->summary);
        }
    }
    fprintf(out,
            "\n"
            "Global options (accepted anywhere on the command line):\n"
            "  --color=<when>          coloured diagnostics: auto (default), always, never\n"
            "  --diag-format=<fmt>     diagnostics as human (default), short or json\n"
            "  --strict                treat warnings as errors\n"
            "  -h, --help              show help (`tern help <command>` for one command)\n"
            "  -V, --version           print the version\n"
            "\n"
            "See `tern help <command>` for the options of a command.\n");
}

int cmd_print_usage(const char *name)
{
    const MainCmd *c = main_find(name);
    if (!c)
        return -1;
    printf("tern %s - %s\n\nUsage: tern %s%s%s\n", c->name, c->summary, c->name,
           c->synopsis[0] ? " " : "", c->synopsis);
    if (c->details)
        printf("\nOptions:\n%s", c->details);
    return 0;
}

/* Levenshtein distance, for "did you mean" suggestions. */
static size_t main_edit_distance(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b), i, j;
    size_t row[32];
    if (lb >= TERN_ARRAY_LEN(row))
        return (size_t)-1;
    for (j = 0; j <= lb; j++)
        row[j] = j;
    for (i = 1; i <= la; i++) {
        size_t prev = row[0];
        row[0] = i;
        for (j = 1; j <= lb; j++) {
            size_t cur = row[j];
            size_t sub = prev + (a[i - 1] == b[j - 1] ? 0 : 1);
            size_t del = row[j] + 1, ins = row[j - 1] + 1;
            row[j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
            prev = cur;
        }
    }
    return row[lb];
}

static void main_unknown_command(const char *name)
{
    size_t i, best = (size_t)-1;
    const char *sugg = NULL;
    for (i = 0; i < TERN_ARRAY_LEN(main_cmds); i++) {
        size_t d;
        if (!main_cmds[i].group)
            continue;
        d = main_edit_distance(name, main_cmds[i].name);
        if (d < best) {
            best = d;
            sugg = main_cmds[i].name;
        }
    }
    fprintf(stderr, "tern: error: unknown command `%s`\n", name);
    if (sugg && best <= 2)
        fprintf(stderr, "  = help: did you mean `tern %s`?\n", sugg);
    fprintf(stderr, "  = note: run `tern help` for the list of commands\n");
}

/* Removes the global options from argv (in place) and applies them.
 * Returns the new argc, or -1 on a bad global option. */
static int main_global_options(int argc, char **argv)
{
    int i, out = 1, color = -1, only_pos = 0;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = NULL;
        if (only_pos || a[0] != '-') {
            argv[out++] = argv[i];
            continue;
        }
        if (strcmp(a, "--") == 0) {
            only_pos = 1;
            argv[out++] = argv[i];
            continue;
        }
        if (strcmp(a, "--strict") == 0) {
            diag_set_strict(1);
        } else if (strcmp(a, "--no-color") == 0) {
            color = 0;
        } else if (strncmp(a, "--color", 7) == 0 && (a[7] == '=' || a[7] == '\0')) {
            val = a[7] == '=' ? a + 8 : "always";
            if (strcmp(val, "always") == 0)
                color = 1;
            else if (strcmp(val, "never") == 0)
                color = 0;
            else if (strcmp(val, "auto") == 0)
                color = -1;
            else {
                fprintf(stderr, "tern: error: invalid value `%s` for --color (auto, always, never)\n", val);
                return -1;
            }
        } else if (strncmp(a, "--diag-format", 13) == 0 && (a[13] == '=' || a[13] == '\0')) {
            DiagFormat f;
            val = a[13] == '=' ? a + 14 : (i + 1 < argc ? argv[++i] : "");
            if (!diag_parse_format(val, &f)) {
                fprintf(stderr, "tern: error: invalid value `%s` for --diag-format (human, short, json)\n", val);
                return -1;
            }
            diag_set_format(f);
        } else {
            argv[out++] = argv[i];
        }
    }
    argv[out] = NULL;
    diag_set_color(color < 0 ? os_color_enabled(2) : color);
    return out;
}

int main(int argc, char **argv)
{
    const MainCmd *c;
    const char *name;
    int rc;
    os_set_argv0(argc > 0 ? argv[0] : NULL);
    argc = main_global_options(argc, argv);
    if (argc < 0)
        return TERN_EXIT_USAGE;
    if (argc < 2) {
        main_print_help(stderr);
        return TERN_EXIT_USAGE;
    }
    name = argv[1];
    if (strcmp(name, "version") == 0 || strcmp(name, "--version") == 0 || strcmp(name, "-V") == 0) {
        main_print_version();
        return TERN_EXIT_OK;
    }
    if (strcmp(name, "help") == 0 || strcmp(name, "--help") == 0 || strcmp(name, "-h") == 0) {
        if (argc > 2) {
            if (cmd_print_usage(argv[2]) != 0) {
                main_unknown_command(argv[2]);
                return TERN_EXIT_USAGE;
            }
            return TERN_EXIT_OK;
        }
        main_print_help(stdout);
        return TERN_EXIT_OK;
    }
    c = main_find(name);
    if (!c) {
        main_unknown_command(name);
        return TERN_EXIT_USAGE;
    }
    rc = c->fn(argc - 1, argv + 1);
    diag_flush();
    fflush(stdout);
    return rc;
}
