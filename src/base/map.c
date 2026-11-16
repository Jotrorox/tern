/* map.c — open addressing with linear probing and tombstones. */
#include "map.h"

#define MAP_EMPTY 0
#define MAP_FULL 1
#define MAP_DELETED 2

uint64_t hash_u64(uint64_t x)
{
    /* splitmix64 finalizer */
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return x;
}

uint64_t hash_bytes(const void *p, size_t n)
{
    /* FNV-1a, then a final avalanche so low bits are usable as an index. */
    const unsigned char *s = (const unsigned char *)p;
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= s[i];
        h *= UINT64_C(0x100000001b3);
    }
    return hash_u64(h ^ (uint64_t)n);
}

uint64_t hash_combine(uint64_t h, uint64_t x)
{
    return hash_u64(h ^ (x + UINT64_C(0x9e3779b97f4a7c15) + (h << 6) + (h >> 2)));
}

/* Returns the slot holding key, or (size_t)-1. */
static size_t map_find(const Map *m, uint64_t key)
{
    size_t mask, i;
    if (!m->cap)
        return (size_t)-1;
    mask = m->cap - 1;
    i = (size_t)hash_u64(key) & mask;
    for (;;) {
        unsigned char st = m->state[i];
        if (st == MAP_EMPTY)
            return (size_t)-1;
        if (st == MAP_FULL && m->keys[i] == key)
            return i;
        i = (i + 1) & mask;
    }
}

static void map_rehash(Map *m, size_t ncap)
{
    Map n;
    size_t i;
    n.keys = (uint64_t *)xmalloc(ncap * sizeof(uint64_t));
    n.vals = (uint64_t *)xmalloc(ncap * sizeof(uint64_t));
    n.state = (unsigned char *)xcalloc(ncap, 1);
    n.cap = ncap;
    n.len = m->len;
    n.used = m->len;
    for (i = 0; i < m->cap; i++) {
        size_t j;
        if (m->state[i] != MAP_FULL)
            continue;
        j = (size_t)hash_u64(m->keys[i]) & (ncap - 1);
        while (n.state[j] != MAP_EMPTY)
            j = (j + 1) & (ncap - 1);
        n.state[j] = MAP_FULL;
        n.keys[j] = m->keys[i];
        n.vals[j] = m->vals[i];
    }
    free(m->keys);
    free(m->vals);
    free(m->state);
    *m = n;
}

int map_get(const Map *m, uint64_t key, uint64_t *out)
{
    size_t i = map_find(m, key);
    if (i == (size_t)-1)
        return 0;
    if (out)
        *out = m->vals[i];
    return 1;
}

int map_has(const Map *m, uint64_t key)
{
    return map_find(m, key) != (size_t)-1;
}

uint64_t *map_ref(Map *m, uint64_t key, int *inserted)
{
    size_t mask, i, tomb = (size_t)-1;
    if ((m->used + 1) * 4 > m->cap * 3) {
        /* grow if mostly live, otherwise just clean out tombstones */
        size_t ncap = m->cap ? m->cap : 16;
        while ((m->len + 1) * 2 > ncap)
            ncap *= 2;
        map_rehash(m, ncap);
    }
    mask = m->cap - 1;
    i = (size_t)hash_u64(key) & mask;
    for (;;) {
        unsigned char st = m->state[i];
        if (st == MAP_EMPTY)
            break;
        if (st == MAP_FULL && m->keys[i] == key) {
            if (inserted)
                *inserted = 0;
            return &m->vals[i];
        }
        if (st == MAP_DELETED && tomb == (size_t)-1)
            tomb = i;
        i = (i + 1) & mask;
    }
    if (tomb != (size_t)-1)
        i = tomb;
    else
        m->used++;
    m->state[i] = MAP_FULL;
    m->keys[i] = key;
    m->vals[i] = 0;
    m->len++;
    if (inserted)
        *inserted = 1;
    return &m->vals[i];
}

void map_put(Map *m, uint64_t key, uint64_t val)
{
    *map_ref(m, key, NULL) = val;
}

int map_del(Map *m, uint64_t key)
{
    size_t i = map_find(m, key);
    if (i == (size_t)-1)
        return 0;
    m->state[i] = MAP_DELETED;
    m->len--;
    return 1;
}

size_t map_len(const Map *m)
{
    return m->len;
}

void map_clear(Map *m)
{
    if (m->cap)
        memset(m->state, 0, m->cap);
    m->len = 0;
    m->used = 0;
}

void map_free(Map *m)
{
    free(m->keys);
    free(m->vals);
    free(m->state);
    memset(m, 0, sizeof(*m));
}

int map_next(const Map *m, size_t *iter, uint64_t *key, uint64_t *val)
{
    size_t i;
    for (i = *iter; i < m->cap; i++) {
        if (m->state[i] == MAP_FULL) {
            if (key)
                *key = m->keys[i];
            if (val)
                *val = m->vals[i];
            *iter = i + 1;
            return 1;
        }
    }
    *iter = m->cap;
    return 0;
}

#undef MAP_EMPTY
#undef MAP_FULL
#undef MAP_DELETED
