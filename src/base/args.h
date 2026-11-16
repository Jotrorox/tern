/* args.h — tiny getopt-like command-line parser shared by all commands.
 *
 * Supports `--flag`, `--opt value`, `--opt=value`, single-letter aliases
 * (`-h`, `-o value`), positional arguments, and `--` (everything after it
 * is positional). A lone `-` is positional (conventionally stdin).
 *
 *     int cmd_test(int argc, char **argv)          // argv[0] == "test"
 *     {
 *         ArgParser ap;
 *         const char *seed = NULL, *filter = NULL;
 *         int waves = 0;
 *         args_init(&ap, argc, argv);
 *         while (args_next(&ap)) {
 *             if (args_flag(&ap, "waves")) waves = 1;
 *             else if (args_opt(&ap, "seed", &seed)) ;
 *             else if (args_flag(&ap, "help|h")) { cmd_print_usage("test"); return 0; }
 *             else if (args_positional(&ap)) filter = ap.arg;
 *             else args_unknown(&ap);
 *         }
 *         if (ap.errors) return 2;
 *         ...
 *     }
 *
 * Names are `|`-separated alternatives; a one-character alternative matches
 * the single-dash form (`"output|o"` matches `--output` and `-o`).
 * Errors are printed to stderr as `tern <cmd>: error: ...` and counted in
 * ap.errors.
 */
#ifndef TERN_ARGS_H
#define TERN_ARGS_H

#include "common.h"

typedef struct ArgParser {
    int argc;
    char **argv;
    const char *cmd;       /* argv[0], used in error messages */
    int next;              /* index of the next argv element to read */
    int only_positional;   /* after `--` */
    /* current item (valid after args_next returns 1) */
    const char *arg;       /* the raw argument (positional: its text) */
    const char *name;      /* option name without dashes (NULL if positional) */
    size_t name_len;
    const char *inline_val;/* value after '=' in --opt=value, or NULL */
    int is_short;          /* single-dash option */
    int handled;           /* set when a matcher consumed the item */
    int errors;
} ArgParser;

void args_init(ArgParser *ap, int argc, char **argv);   /* starts after argv[0] */
int args_next(ArgParser *ap);                           /* 1 while items remain */
int args_flag(ArgParser *ap, const char *names);        /* boolean option */
int args_opt(ArgParser *ap, const char *names, const char **val);  /* option with a value */
int args_opt_u64(ArgParser *ap, const char *names, uint64_t *val); /* numeric value */
int args_positional(ArgParser *ap);                     /* current item is positional */
void args_unknown(ArgParser *ap);                       /* report an unrecognized item */
void args_error(ArgParser *ap, const char *fmt, ...) TERN_PRINTF(2, 3);

/* 1 if argv[1..] contains --help or -h before any `--`. */
int args_wants_help(int argc, char **argv);

#endif /* TERN_ARGS_H */
