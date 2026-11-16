/* str.c — global intern table and StrBuf. */
#include "str.h"
#include "arena.h"
#include "map.h"

/* ------------------------------------------------------------ interning -- */

/* Each interned string is stored as [size_t len][bytes...]['\0'] in the
 * intern arena; the table maps to the byte pointer. Open addressing with
 * linear probing on the full 64-bit hash. */
typedef struct StrInternSlot {
    uint64_t hash;
    const char *str;   /* NULL = empty slot */
} StrInternSlot;

static Arena str_intern_arena;
static StrInternSlot *str_intern_slots;
static size_t str_intern_cap;   /* power of two */
static size_t str_intern_count;

static size_t str_stored_len(const char *s)
{
    size_t n;
    memcpy(&n, s - sizeof(size_t), sizeof(size_t));
    return n;
}

static void str_intern_grow(void)
{
    size_t ncap = str_intern_cap ? str_intern_cap * 2 : 1024;
    StrInternSlot *ns = (StrInternSlot *)xcalloc(ncap, sizeof(StrInternSlot));
    size_t i;
    for (i = 0; i < str_intern_cap; i++) {
        StrInternSlot *s = &str_intern_slots[i];
        size_t j;
        if (!s->str)
            continue;
        j = (size_t)s->hash & (ncap - 1);
        while (ns[j].str)
            j = (j + 1) & (ncap - 1);
        ns[j] = *s;
    }
    free(str_intern_slots);
    str_intern_slots = ns;
    str_intern_cap = ncap;
}

const char *intern(const char *s, size_t len)
{
    uint64_t h;
    size_t i;
    char *mem;
    if (str_intern_count * 4 >= str_intern_cap * 3)
        str_intern_grow();
    h = hash_bytes(s, len);
    i = (size_t)h & (str_intern_cap - 1);
    for (;;) {
        StrInternSlot *slot = &str_intern_slots[i];
        if (!slot->str)
            break;
        if (slot->hash == h && str_stored_len(slot->str) == len &&
            memcmp(slot->str, s, len) == 0)
            return slot->str;
        i = (i + 1) & (str_intern_cap - 1);
    }
    mem = (char *)arena_alloc(&str_intern_arena, sizeof(size_t) + len + 1);
    memcpy(mem, &len, sizeof(size_t));
    if (len)
        memcpy(mem + sizeof(size_t), s, len);
    mem[sizeof(size_t) + len] = '\0';
    str_intern_slots[i].hash = h;
    str_intern_slots[i].str = mem + sizeof(size_t);
    str_intern_count++;
    return mem + sizeof(size_t);
}

const char *intern_cstr(const char *s)
{
    return intern(s, strlen(s));
}

size_t intern_len(const char *interned)
{
    return str_stored_len(interned);
}

void intern_free_all(void)
{
    free(str_intern_slots);
    str_intern_slots = NULL;
    str_intern_cap = 0;
    str_intern_count = 0;
    arena_release(&str_intern_arena);
}

/* -------------------------------------------------------------- StrBuf --- */

void sb_reserve(StrBuf *sb, size_t extra)
{
    size_t need = sb->len + extra + 1; /* + NUL */
    if (need < sb->len)
        tern_fatal("string buffer too large");
    if (need > sb->cap) {
        size_t nc = sb->cap ? sb->cap : 64;
        while (nc < need)
            nc *= 2;
        sb->data = (char *)xrealloc(sb->data, nc);
        sb->cap = nc;
    }
}

void sb_append(StrBuf *sb, const void *p, size_t n)
{
    sb_reserve(sb, n);
    if (n)
        memcpy(sb->data + sb->len, p, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_putc(StrBuf *sb, int c)
{
    sb_reserve(sb, 1);
    sb->data[sb->len++] = (char)c;
    sb->data[sb->len] = '\0';
}

void sb_puts(StrBuf *sb, const char *s)
{
    sb_append(sb, s, strlen(s));
}

void sb_vprintf(StrBuf *sb, const char *fmt, va_list ap)
{
    va_list ap2;
    int n;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n <= 0) {
        sb_reserve(sb, 0);
        sb->data[sb->len] = '\0';
        return;
    }
    sb_reserve(sb, (size_t)n);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    sb->len += (size_t)n;
}

void sb_printf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(sb, fmt, ap);
    va_end(ap);
}

void sb_indent(StrBuf *sb, int n)
{
    if (n <= 0)
        return;
    sb_reserve(sb, (size_t)n);
    memset(sb->data + sb->len, ' ', (size_t)n);
    sb->len += (size_t)n;
    sb->data[sb->len] = '\0';
}

const char *sb_cstr(StrBuf *sb)
{
    sb_reserve(sb, 0);
    sb->data[sb->len] = '\0';
    return sb->data;
}

void sb_reset(StrBuf *sb)
{
    sb->len = 0;
    if (sb->data)
        sb->data[0] = '\0';
}

void sb_truncate(StrBuf *sb, size_t len)
{
    if (len < sb->len) {
        sb->len = len;
        sb->data[len] = '\0';
    }
}

char *sb_take(StrBuf *sb)
{
    char *p;
    sb_cstr(sb);
    p = sb->data;
    sb->data = NULL;
    sb->len = sb->cap = 0;
    return p;
}

void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}
