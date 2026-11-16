/* common.h — the one header every translation unit includes first.
 *
 * Pulls in the only standard headers the tree may use outside src/os/
 * (<stddef.h> <stdint.h> <stdarg.h> <stdio.h> <stdlib.h> <string.h>
 * <limits.h>) and provides the small helpers that would otherwise come from
 * <ctype.h> or <assert.h>.
 */
#ifndef TERN_COMMON_H
#define TERN_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define TERN_VERSION "0.1.0"

/* TERN_STATIC_ASSERT(cond, name): file- or block-scope compile-time check.
 * `name` must be a valid identifier fragment, unique in its scope. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define TERN_STATIC_ASSERT(cond, name) _Static_assert(cond, #name)
#else
#define TERN_STATIC_ASSERT(cond, name) \
    typedef char tern_static_assert_##name[(cond) ? 1 : -1]
#endif

#define TERN_UNUSED(x) ((void)(x))
#define TERN_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define TERN_MIN(a, b) ((a) < (b) ? (a) : (b))
#define TERN_MAX(a, b) ((a) > (b) ? (a) : (b))

/* Printf-style format checking where the compiler supports it (attribute
 * syntax is only used under GCC/Clang, so the tree stays extension-free
 * for every other compiler). */
#if defined(__GNUC__) || defined(__clang__)
#define TERN_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#define TERN_NORETURN __attribute__((noreturn))
#else
#define TERN_PRINTF(fmt_idx, arg_idx)
#define TERN_NORETURN
#endif

/* Internal invariant check (replaces <assert.h>); always on. */
#define TERN_ASSERT(cond) \
    ((cond) ? (void)0 : tern_assert_fail(#cond, __FILE__, __LINE__))

/* --------------------------------------------------------------- memory -- */

/* All of these abort the program with a message on out-of-memory; they
 * never return NULL (xmalloc(0) returns a unique non-NULL pointer). */
void *xmalloc(size_t n);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void *xmemdup(const void *p, size_t n);

/* Print "tern: fatal: <msg>" to stderr and exit(70). For internal errors only. */
TERN_NORETURN void tern_fatal(const char *fmt, ...) TERN_PRINTF(1, 2);
TERN_NORETURN void tern_assert_fail(const char *expr, const char *file, int line);

/* Growth helper behind vec.h: returns `data` reallocated so that it holds at
 * least `need` elements of `elem` bytes, updating *cap. */
void *tern_vec_grow(void *data, size_t *cap, size_t need, size_t elem);

/* ---------------------------------------------------- character classes -- */
/* ASCII-only replacements for <ctype.h>; safe for any `int` (incl. negative
 * char values and EOF). */

int tern_is_digit(int c);   /* 0-9 */
int tern_is_xdigit(int c);  /* 0-9 a-f A-F */
int tern_is_alpha(int c);   /* a-z A-Z */
int tern_is_alnum(int c);   /* alpha or digit */
int tern_is_ident(int c);   /* alnum or '_' */
int tern_is_space(int c);   /* ' ' \t \n \v \f \r */
int tern_is_upper(int c);
int tern_is_lower(int c);
int tern_to_lower(int c);
int tern_to_upper(int c);
int tern_hex_value(int c);  /* 0..15, or -1 if not a hex digit */

/* ------------------------------------------------------ string helpers -- */

int tern_streq(const char *a, const char *b);            /* NULL-safe equality */
int tern_starts_with(const char *s, const char *prefix);
int tern_ends_with(const char *s, const char *suffix);
int tern_strcasecmp(const char *a, const char *b);        /* ASCII case-insensitive */

/* Parse a decimal (or 0x/0o/0b-prefixed) unsigned integer, allowing '_'
 * separators. Returns 1 and sets *out on success, 0 on syntax error/overflow. */
int tern_parse_u64(const char *s, uint64_t *out);

#endif /* TERN_COMMON_H */
