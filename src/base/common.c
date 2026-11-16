/* common.c — allocation wrappers, character classes, tiny string helpers. */
#include "common.h"

static void common_oom(size_t n)
{
    fprintf(stderr, "tern: fatal: out of memory (requested %lu bytes)\n", (unsigned long)n);
    exit(70);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        common_oom(n);
    return p;
}

void *xcalloc(size_t count, size_t size)
{
    void *p;
    if (count && size > (size_t)-1 / count)
        common_oom((size_t)-1);
    p = calloc(count ? count : 1, size ? size : 1);
    if (!p)
        common_oom(count * size);
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q)
        common_oom(n);
    return q;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *p = (char *)xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void *xmemdup(const void *p, size_t n)
{
    void *q = xmalloc(n);
    if (n)
        memcpy(q, p, n);
    return q;
}

void tern_fatal(const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fputs("tern: fatal: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(70);
}

void tern_assert_fail(const char *expr, const char *file, int line)
{
    tern_fatal("internal error: assertion `%s` failed at %s:%d", expr, file, line);
}

void *tern_vec_grow(void *data, size_t *cap, size_t need, size_t elem)
{
    size_t nc;
    if (need <= *cap)
        return data;
    nc = *cap ? *cap : 8;
    while (nc < need) {
        if (nc > (size_t)-1 / 2)
            common_oom((size_t)-1);
        nc *= 2;
    }
    if (elem && nc > (size_t)-1 / elem)
        common_oom((size_t)-1);
    data = xrealloc(data, nc * elem);
    *cap = nc;
    return data;
}

int tern_is_digit(int c) { return c >= '0' && c <= '9'; }
int tern_is_upper(int c) { return c >= 'A' && c <= 'Z'; }
int tern_is_lower(int c) { return c >= 'a' && c <= 'z'; }
int tern_is_alpha(int c) { return tern_is_upper(c) || tern_is_lower(c); }
int tern_is_alnum(int c) { return tern_is_alpha(c) || tern_is_digit(c); }
int tern_is_ident(int c) { return tern_is_alnum(c) || c == '_'; }
int tern_is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
int tern_is_xdigit(int c) { return tern_hex_value(c) >= 0; }
int tern_to_lower(int c) { return tern_is_upper(c) ? c - 'A' + 'a' : c; }
int tern_to_upper(int c) { return tern_is_lower(c) ? c - 'a' + 'A' : c; }

int tern_hex_value(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int tern_streq(const char *a, const char *b)
{
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    return strcmp(a, b) == 0;
}

int tern_starts_with(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

int tern_ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

int tern_strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = tern_to_lower((unsigned char)*a), cb = tern_to_lower((unsigned char)*b);
        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (!ca)
            return 0;
    }
}

int tern_parse_u64(const char *s, uint64_t *out)
{
    unsigned base = 10;
    uint64_t v = 0;
    int ndig = 0;
    if (!s || !*s)
        return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        base = 2;
        s += 2;
    } else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
        base = 8;
        s += 2;
    }
    for (; *s; s++) {
        int d;
        if (*s == '_')
            continue;
        d = tern_hex_value((unsigned char)*s);
        if (d < 0 || (unsigned)d >= base)
            return 0;
        if (v > (UINT64_MAX - (uint64_t)d) / base)
            return 0;
        v = v * base + (uint64_t)d;
        ndig++;
    }
    if (!ndig)
        return 0;
    *out = v;
    return 1;
}
