/* map.h — open-addressing hash map from uint64_t to uint64_t.
 *
 * Pointer keys/values go through (uint64_t)(uintptr_t)p. Interned strings
 * are pointers, so a Map keyed by interned names is a symbol table.
 * Zero-init is a valid empty map.
 *
 *     Map m = {0};
 *     map_put(&m, (uint64_t)(uintptr_t)name, 42);
 *     uint64_t v;
 *     if (map_get(&m, (uint64_t)(uintptr_t)name, &v)) ...
 *     size_t it = 0; uint64_t k;
 *     while (map_next(&m, &it, &k, &v)) ...   // unordered iteration
 *     map_free(&m);
 */
#ifndef TERN_MAP_H
#define TERN_MAP_H

#include "common.h"

typedef struct Map {
    uint64_t *keys;
    uint64_t *vals;
    unsigned char *state;   /* 0 empty, 1 full, 2 deleted */
    size_t cap;             /* power of two, or 0 */
    size_t len;             /* live entries */
    size_t used;            /* live + deleted slots */
} Map;

int map_get(const Map *m, uint64_t key, uint64_t *out);  /* 1 if found (out may be NULL) */
int map_has(const Map *m, uint64_t key);
void map_put(Map *m, uint64_t key, uint64_t val);         /* insert or overwrite */
/* Pointer to the value slot for key, inserting it with value 0 if absent
 * (*inserted, if non-NULL, says which). Valid until the next insertion. */
uint64_t *map_ref(Map *m, uint64_t key, int *inserted);
int map_del(Map *m, uint64_t key);                        /* 1 if it was present */
size_t map_len(const Map *m);
void map_clear(Map *m);                                   /* remove all, keep capacity */
void map_free(Map *m);

/* Iteration: start with *iter = 0; returns 1 and fills key/val (either may
 * be NULL) while entries remain. Do not insert while iterating; deleting
 * the current key is allowed. */
int map_next(const Map *m, size_t *iter, uint64_t *key, uint64_t *val);

/* Hashing (64-bit, non-cryptographic, deterministic across platforms). */
uint64_t hash_bytes(const void *p, size_t n);
uint64_t hash_u64(uint64_t x);
uint64_t hash_combine(uint64_t h, uint64_t x);

#define MAP_PTR(p) ((uint64_t)(uintptr_t)(p))

#endif /* TERN_MAP_H */
