/* vec.h — typed growable arrays (heap-backed macros).
 *
 *     typedef VEC(Foo *) FooVec;
 *     FooVec v = {0};               // zero-init is a valid empty vector
 *     vec_push(v, foo);             // `v` is the vector lvalue itself, not a pointer
 *     for (i = 0; i < v.len; i++) use(v.data[i]);
 *     vec_free(v);
 *
 * Through a pointer: vec_push(*pv, x) or vec_push(p->items, x).
 * Arguments may be evaluated more than once; pass simple lvalues.
 */
#ifndef TERN_VEC_H
#define TERN_VEC_H

#include "common.h"

#define VEC(T) struct { T *data; size_t len, cap; }

/* Ensure capacity for at least `n` elements in total. */
#define vec_reserve(v, n) \
    ((v).data = tern_vec_grow((v).data, &(v).cap, (size_t)(n), sizeof(*(v).data)))

/* Append x. */
#define vec_push(v, x) \
    (vec_reserve((v), (v).len + 1), (v).data[(v).len++] = (x))

/* Remove and return the last element (vector must be non-empty). */
#define vec_pop(v) ((v).data[--(v).len])

/* The last element as an lvalue (vector must be non-empty). */
#define vec_last(v) ((v).data[(v).len - 1])

/* Set the length to n; new elements (if growing) are zeroed. */
#define vec_resize(v, n) \
    (vec_reserve((v), (n)), \
     ((size_t)(n) > (v).len \
          ? (void)memset((v).data + (v).len, 0, ((size_t)(n) - (v).len) * sizeof(*(v).data)) \
          : (void)0), \
     (v).len = (size_t)(n))

/* Remove element i, shifting the tail down (order-preserving). */
#define vec_remove(v, i) \
    (memmove((v).data + (i), (v).data + (i) + 1, ((v).len - (size_t)(i) - 1) * sizeof(*(v).data)), \
     (v).len--)

/* Forget all elements, keep the allocation. */
#define vec_clear(v) ((v).len = 0)

/* Release the allocation; the vector is empty and reusable afterwards. */
#define vec_free(v) (free((v).data), (v).data = NULL, (v).len = 0, (v).cap = 0)

#endif /* TERN_VEC_H */
