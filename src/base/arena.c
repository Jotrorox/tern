/* arena.c — bump allocator with chained blocks. */
#include "arena.h"

#define ARENA_BLOCK_MIN (64u * 1024u)

struct ArenaBlock {
    ArenaBlock *prev;
    size_t used;     /* bytes used in data[] */
    size_t cap;      /* bytes available in data[] */
    char *data;      /* ARENA_ALIGN-aligned start of the usable area */
};

static size_t arena_round_up(size_t n)
{
    return (n + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);
}

static ArenaBlock *arena_new_block(ArenaBlock *prev, size_t need)
{
    size_t cap = need > ARENA_BLOCK_MIN ? need : ARENA_BLOCK_MIN;
    size_t hdr = arena_round_up(sizeof(ArenaBlock));
    char *raw;
    ArenaBlock *b;
    uintptr_t p;
    if (cap > (size_t)-1 - hdr - ARENA_ALIGN)
        tern_fatal("arena allocation too large (%lu bytes)", (unsigned long)need);
    raw = (char *)xmalloc(hdr + cap + ARENA_ALIGN);
    b = (ArenaBlock *)(void *)raw;
    p = (uintptr_t)(raw + hdr);
    p = (p + (ARENA_ALIGN - 1)) & ~(uintptr_t)(ARENA_ALIGN - 1);
    b->prev = prev;
    b->used = 0;
    b->cap = cap;
    b->data = (char *)p;
    return b;
}

void *arena_alloc(Arena *a, size_t size)
{
    ArenaBlock *b = a->head;
    size_t n = arena_round_up(size ? size : 1);
    void *p;
    if (n < size)
        tern_fatal("arena allocation too large");
    if (!b || b->cap - b->used < n) {
        b = arena_new_block(b, n);
        a->head = b;
    }
    p = b->data + b->used;
    b->used += n;
    a->total += n;
    return p;
}

void *arena_zalloc(Arena *a, size_t size)
{
    void *p = arena_alloc(a, size);
    memset(p, 0, size);
    return p;
}

void *arena_memdup(Arena *a, const void *p, size_t size)
{
    void *q = arena_alloc(a, size);
    if (size)
        memcpy(q, p, size);
    return q;
}

char *arena_strndup(Arena *a, const char *s, size_t n)
{
    char *p = (char *)arena_alloc(a, n + 1);
    if (n)
        memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *arena_strdup(Arena *a, const char *s)
{
    return arena_strndup(a, s, strlen(s));
}

char *arena_vprintf(Arena *a, const char *fmt, va_list ap)
{
    va_list ap2;
    int n;
    char *p;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0)
        n = 0;
    p = (char *)arena_alloc(a, (size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    return p;
}

char *arena_printf(Arena *a, const char *fmt, ...)
{
    va_list ap;
    char *p;
    va_start(ap, fmt);
    p = arena_vprintf(a, fmt, ap);
    va_end(ap);
    return p;
}

void arena_release(Arena *a)
{
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *prev = b->prev;
        free(b);
        b = prev;
    }
    a->head = NULL;
    a->total = 0;
}

ArenaMark arena_mark(Arena *a)
{
    ArenaMark m;
    m.block = a->head;
    m.used = a->head ? a->head->used : 0;
    m.total = a->total;
    return m;
}

void arena_rewind(Arena *a, ArenaMark m)
{
    while (a->head && a->head != m.block) {
        ArenaBlock *prev = a->head->prev;
        free(a->head);
        a->head = prev;
    }
    if (a->head)
        a->head->used = m.used;
    a->total = m.total;
}

#undef ARENA_BLOCK_MIN
