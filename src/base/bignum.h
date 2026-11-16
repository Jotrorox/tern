/* bignum.h — arbitrary-precision signed integers for compile-time arithmetic.
 *
 * Sign-magnitude representation with 32-bit limbs. A BigInt owns a heap
 * buffer: zero-init (`BigInt x = {0};`) is the value 0, and every BigInt
 * that has been written to must eventually be released with bn_free().
 *
 * All operations write their result to the first argument `r`, which may
 * alias any input. Bitwise operations and bit access use infinite-width
 * two's-complement semantics (like Python ints): -1 has every bit set.
 */
#ifndef TERN_BIGNUM_H
#define TERN_BIGNUM_H

#include "common.h"

struct StrBuf;

typedef struct BigInt {
    uint32_t *d;    /* magnitude, little-endian limbs; d[len-1] != 0 */
    uint32_t len;   /* 0 means the value is zero */
    uint32_t cap;
    int neg;        /* 1 if negative; always 0 for zero */
} BigInt;

/* --------------------------------------------------------- lifecycle ---- */

void bn_init(BigInt *r);                         /* r = 0 (does not free) */
void bn_free(BigInt *r);                         /* release; r becomes 0 */
void bn_copy(BigInt *r, const BigInt *a);        /* r = a */
void bn_swap(BigInt *a, BigInt *b);
void bn_move(BigInt *r, BigInt *a);              /* r = a, a = 0 (frees old r) */

/* ------------------------------------------------------- conversions ---- */

void bn_set_i64(BigInt *r, int64_t v);
void bn_set_u64(BigInt *r, uint64_t v);
int bn_get_i64(const BigInt *a, int64_t *out);   /* 1 if a fits in int64 */
int bn_get_u64(const BigInt *a, uint64_t *out);  /* 1 if 0 <= a <= UINT64_MAX */
int bn_fits_i64(const BigInt *a);
int bn_fits_u64(const BigInt *a);
uint64_t bn_low_u64(const BigInt *a);            /* a mod 2^64 (two's complement) */

/* Little-endian 64-bit limb arrays of `width` bits (IR_LIMBS(width) limbs,
 * the IrBits layout). from: the bits are read as unsigned or as two's
 * complement signed. to: writes a mod 2^width (two's complement wrap) with
 * all bits at positions >= width cleared. */
void bn_from_limbs(BigInt *r, const uint64_t *w, uint32_t width, int is_signed);
void bn_to_limbs(const BigInt *a, uint64_t *w, uint32_t width);
/* Does a fit in `width` bits as unsigned (0 <= a < 2^width) or signed
 * (-2^(width-1) <= a < 2^(width-1))? width 0 only holds 0. */
int bn_fits_width(const BigInt *a, uint32_t width, int is_signed);

/* Parse digits in `base` (2, 8, 10 or 16; 0 = auto-detect a 0x/0o/0b prefix,
 * default decimal). Accepts an optional leading '+' or '-', '_' separators
 * anywhere after the first digit or prefix, and upper/lower-case hex digits.
 * Returns 1 on success, 0 on malformed input (r is then 0). */
int bn_parse(BigInt *r, const char *s, size_t n, int base);
int bn_parse_cstr(BigInt *r, const char *s, int base);

/* Format in base 2, 8, 10 or 16 (lower-case, no prefix, '-' if negative). */
char *bn_to_str(const BigInt *a, int base);                /* heap string; caller frees */
void bn_to_sb(struct StrBuf *out, const BigInt *a, int base);

/* -------------------------------------------------------- arithmetic ---- */

void bn_neg(BigInt *r, const BigInt *a);
void bn_abs(BigInt *r, const BigInt *a);
void bn_add(BigInt *r, const BigInt *a, const BigInt *b);
void bn_sub(BigInt *r, const BigInt *a, const BigInt *b);
void bn_mul(BigInt *r, const BigInt *a, const BigInt *b);
void bn_add_i64(BigInt *r, const BigInt *a, int64_t b);
void bn_mul_i64(BigInt *r, const BigInt *a, int64_t b);
/* Truncating division (C semantics): q = trunc(a/b), rem = a - q*b, rem has
 * the sign of a. Floor division (Python semantics): q = floor(a/b),
 * rem has the sign of b. q and rem may each be NULL. Return 0 (and leave
 * q/rem untouched) if b == 0, else 1. */
int bn_divmod_trunc(BigInt *q, BigInt *rem, const BigInt *a, const BigInt *b);
int bn_divmod_floor(BigInt *q, BigInt *rem, const BigInt *a, const BigInt *b);
void bn_pow(BigInt *r, const BigInt *a, uint64_t e);     /* a ** e (0**0 == 1) */

/* Shifts: shl multiplies by 2^n; shr is an arithmetic shift (floor(a/2^n)). */
void bn_shl(BigInt *r, const BigInt *a, uint64_t n);
void bn_shr(BigInt *r, const BigInt *a, uint64_t n);

/* Infinite-width two's complement bitwise operations. */
void bn_and(BigInt *r, const BigInt *a, const BigInt *b);
void bn_or(BigInt *r, const BigInt *a, const BigInt *b);
void bn_xor(BigInt *r, const BigInt *a, const BigInt *b);
void bn_not(BigInt *r, const BigInt *a);                 /* -a - 1 */

/* ------------------------------------------------------------ queries --- */

int bn_cmp(const BigInt *a, const BigInt *b);            /* -1, 0, 1 */
int bn_cmp_i64(const BigInt *a, int64_t b);
int bn_cmp_abs(const BigInt *a, const BigInt *b);        /* compare |a| and |b| */
int bn_sign(const BigInt *a);                            /* -1, 0, 1 */
int bn_is_zero(const BigInt *a);
int bn_is_neg(const BigInt *a);
/* Number of bits in |a| (0 for 0). For a >= 0 this is the minimum unsigned
 * width that holds a. */
uint64_t bn_bitlen(const BigInt *a);
/* Minimum two's-complement width that holds a (0 -> 1, -1 -> 1, 127 -> 8,
 * -128 -> 8). */
uint64_t bn_signed_width(const BigInt *a);
int bn_get_bit(const BigInt *a, uint64_t i);             /* two's complement bit i */
void bn_set_bit(BigInt *r, uint64_t i, int v);           /* two's complement bit i := v */
uint64_t bn_hash(const BigInt *a);

#endif /* TERN_BIGNUM_H */
