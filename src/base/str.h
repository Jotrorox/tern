/* str.h — string interning and growable string buffers.
 *
 * Interning: intern() returns a canonical, NUL-terminated, immutable copy;
 * two interned strings are equal iff their pointers are equal. The table is
 * global and lives until intern_free_all() (normally: process exit).
 *
 * StrBuf: heap-backed byte buffer. Zero-init is a valid empty buffer.
 * `data` is always NUL-terminated once anything has been appended (or after
 * sb_cstr()), so `sb.data` can be passed to C string functions.
 */
#ifndef TERN_STR_H
#define TERN_STR_H

#include "common.h"

/* ------------------------------------------------------------ interning -- */

const char *intern(const char *s, size_t len);
const char *intern_cstr(const char *s);
size_t intern_len(const char *interned);   /* O(1) length of an interned string */
void intern_free_all(void);                /* invalidates every interned pointer */

/* -------------------------------------------------------------- StrBuf --- */

typedef struct StrBuf {
    char *data;
    size_t len, cap;
} StrBuf;

void sb_reserve(StrBuf *sb, size_t extra);           /* room for `extra` more bytes */
void sb_putc(StrBuf *sb, int c);
void sb_puts(StrBuf *sb, const char *s);
void sb_append(StrBuf *sb, const void *p, size_t n);
void sb_printf(StrBuf *sb, const char *fmt, ...) TERN_PRINTF(2, 3);
void sb_vprintf(StrBuf *sb, const char *fmt, va_list ap);
void sb_indent(StrBuf *sb, int n);                   /* append n spaces */
const char *sb_cstr(StrBuf *sb);                     /* NUL-terminated view ("" if empty) */
void sb_reset(StrBuf *sb);                           /* len = 0, keeps capacity */
void sb_truncate(StrBuf *sb, size_t len);            /* shrink to len (no-op if longer) */
char *sb_take(StrBuf *sb);                           /* heap string (caller frees); sb emptied */
void sb_free(StrBuf *sb);

#endif /* TERN_STR_H */
