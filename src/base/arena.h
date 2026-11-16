/* arena.h — bump allocator. One arena per phase; released as a whole.
 *
 *     Arena a = {0};                  // zero-init is a valid empty arena
 *     Foo *f = ANEW(&a, Foo);         // zeroed
 *     int *xs = ANEWN(&a, int, 100);  // zeroed array
 *     arena_release(&a);              // frees everything, arena reusable
 */
#ifndef TERN_ARENA_H
#define TERN_ARENA_H

#include "common.h"

typedef struct ArenaBlock ArenaBlock;

typedef struct Arena {
    ArenaBlock *head;   /* current block (blocks are chained backwards) */
    size_t total;       /* bytes handed out so far (statistics) */
} Arena;

/* A position in an arena that can be rewound to (frees everything allocated
 * after the mark). */
typedef struct ArenaMark {
    ArenaBlock *block;
    size_t used;
    size_t total;
} ArenaMark;

/* Every allocation is aligned to ARENA_ALIGN (16) bytes. Never returns NULL. */
#define ARENA_ALIGN 16

void *arena_alloc(Arena *a, size_t size);   /* uninitialized */
void *arena_zalloc(Arena *a, size_t size);  /* zeroed */
void *arena_memdup(Arena *a, const void *p, size_t size);
char *arena_strndup(Arena *a, const char *s, size_t n);  /* NUL-terminated copy */
char *arena_strdup(Arena *a, const char *s);
char *arena_printf(Arena *a, const char *fmt, ...) TERN_PRINTF(2, 3);
char *arena_vprintf(Arena *a, const char *fmt, va_list ap);
void arena_release(Arena *a);               /* free all blocks; arena is empty again */

ArenaMark arena_mark(Arena *a);
void arena_rewind(Arena *a, ArenaMark m);

#define ANEW(arena, T) ((T *)arena_zalloc((arena), sizeof(T)))
#define ANEWN(arena, T, n) ((T *)arena_zalloc((arena), sizeof(T) * (size_t)(n)))

#endif /* TERN_ARENA_H */
