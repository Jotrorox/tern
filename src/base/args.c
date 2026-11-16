/* args.c — command-line option parsing. */
#include "args.h"

void args_init(ArgParser *ap, int argc, char **argv)
{
    memset(ap, 0, sizeof *ap);
    ap->argc = argc;
    ap->argv = argv;
    ap->cmd = argc > 0 && argv[0] ? argv[0] : "tern";
    ap->next = 1;
}

void args_error(ArgParser *ap, const char *fmt, ...)
{
    va_list va;
    fprintf(stderr, "tern %s: error: ", ap->cmd);
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
    fputc('\n', stderr);
    ap->errors++;
}

int args_next(ArgParser *ap)
{
    const char *a;
    if (ap->arg && !ap->handled && ap->name)
        args_unknown(ap); /* previous option was never matched */
    ap->arg = NULL;
    ap->name = NULL;
    ap->name_len = 0;
    ap->inline_val = NULL;
    ap->is_short = 0;
    ap->handled = 0;
    while (ap->next < ap->argc) {
        a = ap->argv[ap->next++];
        if (!a)
            continue;
        ap->arg = a;
        if (ap->only_positional || a[0] != '-' || a[1] == '\0')
            return 1; /* positional */
        if (a[1] == '-' && a[2] == '\0') {
            ap->only_positional = 1;
            ap->arg = NULL;
            continue;
        }
        if (a[1] == '-') {
            const char *eq = strchr(a + 2, '=');
            ap->name = a + 2;
            ap->name_len = eq ? (size_t)(eq - (a + 2)) : strlen(a + 2);
            ap->inline_val = eq ? eq + 1 : NULL;
        } else {
            ap->name = a + 1;
            ap->name_len = 1;
            ap->is_short = 1;
            ap->inline_val = a[2] ? (a[2] == '=' ? a + 3 : a + 2) : NULL;
        }
        return 1;
    }
    ap->arg = NULL;
    return 0;
}

static int args_matches(const ArgParser *ap, const char *names)
{
    const char *p = names;
    if (!ap->name || ap->handled)
        return 0;
    while (*p) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        int short_alt = n == 1;
        if (short_alt == ap->is_short && n == ap->name_len && memcmp(p, ap->name, n) == 0)
            return 1;
        if (!bar)
            break;
        p = bar + 1;
    }
    return 0;
}

int args_flag(ArgParser *ap, const char *names)
{
    if (!args_matches(ap, names))
        return 0;
    ap->handled = 1;
    if (ap->inline_val) {
        args_error(ap, "option `%s` does not take a value", ap->arg);
        return 0;
    }
    return 1;
}

int args_opt(ArgParser *ap, const char *names, const char **val)
{
    if (!args_matches(ap, names))
        return 0;
    ap->handled = 1;
    if (ap->inline_val) {
        *val = ap->inline_val;
        return 1;
    }
    if (ap->next < ap->argc && ap->argv[ap->next]) {
        *val = ap->argv[ap->next++];
        return 1;
    }
    args_error(ap, "option `%s` requires a value", ap->arg);
    return 0;
}

int args_opt_u64(ArgParser *ap, const char *names, uint64_t *val)
{
    const char *s;
    if (!args_opt(ap, names, &s))
        return 0;
    if (!tern_parse_u64(s, val)) {
        args_error(ap, "invalid number `%s` for option `%s`", s, ap->arg);
        return 0;
    }
    return 1;
}

int args_positional(ArgParser *ap)
{
    if (!ap->arg || ap->name || ap->handled)
        return 0;
    ap->handled = 1;
    return 1;
}

void args_unknown(ArgParser *ap)
{
    if (!ap->arg || ap->handled)
        return;
    ap->handled = 1;
    if (ap->name)
        args_error(ap, "unknown option `%s` (see `tern help %s`)", ap->arg, ap->cmd);
    else
        args_error(ap, "unexpected argument `%s` (see `tern help %s`)", ap->arg, ap->cmd);
}

int args_wants_help(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (!argv[i])
            continue;
        if (strcmp(argv[i], "--") == 0)
            return 0;
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return 1;
    }
    return 0;
}
