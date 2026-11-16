/* bitvec.c — arbitrary-width two-state bit-vector arithmetic on uint64_t
 * limb arrays (the simulator's wide path; see sim.h for the contract). */
#include "sim.h"
#include "../base/str.h"

#define BV_LIMBS(w) (((w) + 63u) / 64u)

static uint64_t bv_top_mask(uint32_t w)
{
    return (w % 64) ? ((UINT64_C(1) << (w % 64)) - 1u) : ~UINT64_C(0);
}

static void bv_fix(uint64_t *d, uint32_t w)
{
    if (w)
        d[BV_LIMBS(w) - 1] &= bv_top_mask(w);
}

void bv_zero(uint64_t *d, uint32_t w)
{
    uint32_t i, n = BV_LIMBS(w);
    for (i = 0; i < n; i++)
        d[i] = 0;
}

void bv_copy(uint64_t *d, const uint64_t *a, uint32_t w)
{
    if (d != a)
        memmove(d, a, sizeof(uint64_t) * BV_LIMBS(w));
}

int bv_is_zero(const uint64_t *a, uint32_t w)
{
    uint32_t i, n = BV_LIMBS(w);
    for (i = 0; i < n; i++)
        if (a[i])
            return 0;
    return 1;
}

int bv_eq(const uint64_t *a, const uint64_t *b, uint32_t w)
{
    uint32_t i, n = BV_LIMBS(w);
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

void bv_not(uint64_t *d, const uint64_t *a, uint32_t w)
{
    uint32_t i, n = BV_LIMBS(w);
    for (i = 0; i < n; i++)
        d[i] = ~a[i];
    bv_fix(d, w);
}

void bv_neg(uint64_t *d, const uint64_t *a, uint32_t w)
{
    uint32_t i, n = BV_LIMBS(w);
    uint64_t borrow = 0;
    for (i = 0; i < n; i++) {
        uint64_t x = a[i];
        uint64_t r = 0 - x - borrow;
        borrow = (x != 0 || borrow) ? 1u : 0u;
        d[i] = r;
    }
    bv_fix(d, w);
}

void bv_and(uint64_t *d, const uint64_t *a, const uint64_t *b, uint32_t w)
{
    uint32_t i, n = BV_LIMBS(w);
    for (i = 0; i < n; i++)
        d[i] = a[i] & b[i];
}

void bv_or(uint64_t *d, const uint64_t *a, const uint64_t *b, uint32_t w)
{
    uint32_t i, n = BV_LIMBS(w);
    for (i = 0; i < n; i++)
        d[i] = a[i] | b[i];
}

void bv_xor(uint64_t *d, const uint64_t *a, const uint64_t *b, uint32_t w)
{
    uint32_t i, n = BV_LIMBS(w);
    for (i = 0; i < n; i++)
        d[i] = a[i] ^ b[i];
}

void bv_add(uint64_t *d, const uint64_t *a, const uint64_t *b, uint32_t w)
{
    uint32_t i, n = BV_LIMBS(w);
    uint64_t carry = 0;
    for (i = 0; i < n; i++) {
        uint64_t x = a[i], y = b[i];
        uint64_t s = x + y;
        uint64_t c1 = s < x;
        uint64_t s2 = s + carry;
        uint64_t c2 = s2 < s;
        d[i] = s2;
        carry = c1 | c2;
    }
    bv_fix(d, w);
}

void bv_sub(uint64_t *d, const uint64_t *a, const uint64_t *b, uint32_t w)
{
    uint32_t i, n = BV_LIMBS(w);
    uint64_t borrow = 0;
    for (i = 0; i < n; i++) {
        uint64_t x = a[i], y = b[i];
        uint64_t r = x - y;
        uint64_t b1 = x < y;
        uint64_t r2 = r - borrow;
        uint64_t b2 = r < borrow;
        d[i] = r2;
        borrow = b1 | b2;
    }
    bv_fix(d, w);
}

/* 64x64 -> 128 multiply */
static uint64_t bv_mul64(uint64_t a, uint64_t b, uint64_t *hi)
{
    uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return (mid << 32) | (p00 & 0xFFFFFFFFu);
}

void bv_mul(uint64_t *d, const uint64_t *a, const uint64_t *b, uint32_t w, uint64_t *scratch)
{
    uint32_t n = BV_LIMBS(w), i, j;
    uint64_t *s = scratch;
    for (i = 0; i < n; i++)
        s[i] = 0;
    for (i = 0; i < n; i++) {
        uint64_t carry = 0;
        if (!a[i])
            continue;
        for (j = 0; i + j < n; j++) {
            uint64_t hi, lo = bv_mul64(a[i], b[j], &hi);
            uint64_t t = s[i + j] + lo;
            hi += t < lo;
            t += carry;
            hi += t < carry;
            s[i + j] = t;
            carry = hi;
        }
    }
    for (i = 0; i < n; i++)
        d[i] = s[i];
    bv_fix(d, w);
}

int bv_get_bit(const uint64_t *a, uint32_t bit)
{
    return (int)((a[bit / 64] >> (bit % 64)) & 1u);
}

static int bv_cmp_u(const uint64_t *a, const uint64_t *b, uint32_t n)
{
    uint32_t i;
    for (i = n; i-- > 0;)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

int bv_cmp(const uint64_t *a, const uint64_t *b, uint32_t w, int is_signed)
{
    if (w == 0)
        return 0;
    if (is_signed) {
        int sa = bv_get_bit(a, w - 1), sb = bv_get_bit(b, w - 1);
        if (sa != sb)
            return sa ? -1 : 1;
    }
    return bv_cmp_u(a, b, BV_LIMBS(w));
}

/* unsigned q = a / b, r = a % b over n limbs of width w; q, r distinct from a, b */
static void bv_udivmod(uint64_t *q, uint64_t *r, const uint64_t *a, const uint64_t *b, uint32_t w)
{
    uint32_t n = BV_LIMBS(w), i, k;
    for (i = 0; i < n; i++)
        q[i] = r[i] = 0;
    for (i = w; i-- > 0;) {
        uint64_t carry = r[n - 1] >> 63;
        int ge;
        for (k = n; k-- > 1;)
            r[k] = (r[k] << 1) | (r[k - 1] >> 63);
        r[0] = (r[0] << 1) | (uint64_t)bv_get_bit(a, i);
        if (w % 64) {
            carry = (r[n - 1] >> (w % 64)) & 1u;
            r[n - 1] &= bv_top_mask(w);
        }
        ge = carry || bv_cmp_u(r, b, n) >= 0;
        if (ge) {
            uint64_t borrow = 0;
            for (k = 0; k < n; k++) {
                uint64_t x = r[k], y = b[k];
                uint64_t t = x - y;
                uint64_t b1 = x < y;
                uint64_t t2 = t - borrow;
                uint64_t b2 = t < borrow;
                r[k] = t2;
                borrow = b1 | b2;
            }
            r[n - 1] &= bv_top_mask(w);
            q[i / 64] |= UINT64_C(1) << (i % 64);
        }
    }
}

void bv_divmod(uint64_t *q, uint64_t *r, const uint64_t *a, const uint64_t *b, uint32_t w,
               int is_signed, uint64_t *scratch)
{
    uint32_t n = BV_LIMBS(w), i;
    uint64_t *ua = scratch, *ub = scratch + n, *uq = scratch + 2 * n, *ur = scratch + 3 * n;
    int na = 0, nb = 0;
    if (w == 0)
        return;
    if (bv_is_zero(b, w)) {
        if (r)
            bv_copy(r, a, w);
        if (q)
            bv_zero(q, w);
        return;
    }
    if (is_signed) {
        na = bv_get_bit(a, w - 1);
        nb = bv_get_bit(b, w - 1);
    }
    if (na)
        bv_neg(ua, a, w);
    else
        for (i = 0; i < n; i++)
            ua[i] = a[i];
    if (nb)
        bv_neg(ub, b, w);
    else
        for (i = 0; i < n; i++)
            ub[i] = b[i];
    if (w <= 64) {
        uq[0] = ua[0] / ub[0];
        ur[0] = ua[0] % ub[0];
    } else {
        bv_udivmod(uq, ur, ua, ub, w);
    }
    if (na != nb)
        bv_neg(uq, uq, w);
    if (na)
        bv_neg(ur, ur, w);
    if (q)
        for (i = 0; i < n; i++)
            q[i] = uq[i];
    if (r)
        for (i = 0; i < n; i++)
            r[i] = ur[i];
}

void bv_shl(uint64_t *d, const uint64_t *a, uint32_t w, uint64_t amt)
{
    uint32_t n = BV_LIMBS(w), ls, bs, i;
    if (amt >= w) {
        bv_zero(d, w);
        return;
    }
    ls = (uint32_t)(amt / 64);
    bs = (uint32_t)(amt % 64);
    for (i = n; i-- > 0;) {
        uint64_t v = 0;
        if (i >= ls) {
            v = a[i - ls] << bs;
            if (bs && i > ls)
                v |= a[i - ls - 1] >> (64 - bs);
        }
        d[i] = v;
    }
    bv_fix(d, w);
}

static void bv_shr_fill(uint64_t *d, const uint64_t *a, uint32_t w, uint64_t amt, int fill)
{
    uint32_t n = BV_LIMBS(w), ls, bs, i;
    uint64_t f = fill ? ~UINT64_C(0) : 0;
    uint64_t top;
    if (w == 0)
        return;
    if (amt >= w) {
        for (i = 0; i < n; i++)
            d[i] = f;
        bv_fix(d, w);
        return;
    }
    ls = (uint32_t)(amt / 64);
    bs = (uint32_t)(amt % 64);
    /* view a as sign-filled above w: limb n-1 with fill bits above w%64 */
    top = a[n - 1];
    if (fill && (w % 64))
        top |= ~bv_top_mask(w);
    for (i = 0; i < n; i++) {
        uint32_t s = i + ls;
        uint64_t lo = s < n ? (s == n - 1 ? top : a[s]) : f;
        uint64_t hi = s + 1 < n ? (s + 1 == n - 1 ? top : a[s + 1]) : f;
        d[i] = bs ? ((lo >> bs) | (hi << (64 - bs))) : lo;
    }
    bv_fix(d, w);
}

void bv_shr(uint64_t *d, const uint64_t *a, uint32_t w, uint64_t amt)
{
    bv_shr_fill(d, a, w, amt, 0);
}

void bv_ashr(uint64_t *d, const uint64_t *a, uint32_t w, uint64_t amt)
{
    bv_shr_fill(d, a, w, amt, w ? bv_get_bit(a, w - 1) : 0);
}

int bv_red_and(const uint64_t *a, uint32_t w)
{
    uint32_t i, n = BV_LIMBS(w);
    for (i = 0; i + 1 < n; i++)
        if (a[i] != ~UINT64_C(0))
            return 0;
    return n ? a[n - 1] == bv_top_mask(w) : 1;
}

int bv_red_or(const uint64_t *a, uint32_t w)
{
    return !bv_is_zero(a, w);
}

static int bv_parity64(uint64_t x)
{
    x ^= x >> 32;
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return (int)(x & 1u);
}

int bv_red_xor(const uint64_t *a, uint32_t w)
{
    uint32_t i, n = BV_LIMBS(w);
    uint64_t x = 0;
    for (i = 0; i < n; i++)
        x ^= a[i];
    return bv_parity64(x);
}

uint64_t bv_get64(const uint64_t *a, uint32_t aw, uint64_t lo, uint32_t n)
{
    uint32_t limb, off, na = BV_LIMBS(aw);
    uint64_t v;
    if (lo >= aw || n == 0)
        return 0;
    limb = (uint32_t)(lo / 64);
    off = (uint32_t)(lo % 64);
    v = a[limb] >> off;
    if (off && limb + 1 < na)
        v |= a[limb + 1] << (64 - off);
    if (n < 64)
        v &= (UINT64_C(1) << n) - 1u;
    return v;
}

void bv_extract(uint64_t *d, uint32_t dw, const uint64_t *a, uint32_t aw, uint64_t lo)
{
    uint32_t i, n = BV_LIMBS(dw);
    for (i = 0; i < n; i++) {
        uint32_t bits = dw - 64 * i;
        d[i] = bv_get64(a, aw, lo + 64u * i, bits > 64 ? 64 : bits);
    }
}

/* d[p +: nbits] = v (nbits <= 64), clipped to dw */
static void bv_put_bits(uint64_t *d, uint32_t dw, uint64_t p, uint64_t v, uint32_t nbits)
{
    uint32_t limb, off;
    uint64_t m;
    if (p >= dw || nbits == 0)
        return;
    if (p + nbits > dw)
        nbits = (uint32_t)(dw - p);
    m = nbits >= 64 ? ~UINT64_C(0) : ((UINT64_C(1) << nbits) - 1u);
    v &= m;
    limb = (uint32_t)(p / 64);
    off = (uint32_t)(p % 64);
    d[limb] = (d[limb] & ~(m << off)) | (v << off);
    if (off && off + nbits > 64) {
        uint32_t rest = off + nbits - 64;
        uint64_t m2 = (UINT64_C(1) << rest) - 1u;
        d[limb + 1] = (d[limb + 1] & ~m2) | (v >> (64 - off));
    }
}

void bv_deposit(uint64_t *d, uint32_t dw, uint64_t lo, const uint64_t *a, uint32_t aw)
{
    uint32_t i, n = BV_LIMBS(aw);
    for (i = 0; i < n; i++) {
        uint32_t bits = aw - 64 * i;
        uint64_t p = lo + 64u * i;
        if (p >= dw)
            break;
        bv_put_bits(d, dw, p, a[i], bits > 64 ? 64 : bits);
    }
}

void bv_resize(uint64_t *d, uint32_t dw, const uint64_t *a, uint32_t aw, int sign_extend)
{
    uint32_t nd = BV_LIMBS(dw), na = BV_LIMBS(aw), i;
    int sign = sign_extend && aw && bv_get_bit(a, aw - 1);
    uint64_t f = sign ? ~UINT64_C(0) : 0;
    if (dw <= aw) {
        for (i = 0; i < nd; i++)
            d[i] = a[i];
        bv_fix(d, dw);
        return;
    }
    for (i = 0; i < na; i++)
        d[i] = a[i];
    if (sign && (aw % 64))
        d[na - 1] |= ~bv_top_mask(aw);
    for (i = na; i < nd; i++)
        d[i] = f;
    bv_fix(d, dw);
}

uint64_t bv_to_u64_sat(const uint64_t *a, uint32_t w)
{
    uint32_t i, n = BV_LIMBS(w);
    for (i = 1; i < n; i++)
        if (a[i])
            return ~UINT64_C(0);
    return n ? a[0] : 0;
}

void bv_print_bin(StrBuf *out, const uint64_t *a, uint32_t w, int strip_zeros)
{
    uint32_t i;
    int started = !strip_zeros;
    for (i = w; i-- > 0;) {
        int b = bv_get_bit(a, i);
        if (!started && !b && i != 0)
            continue;
        started = 1;
        sb_putc(out, b ? '1' : '0');
    }
}

void bv_print_hex(StrBuf *out, const uint64_t *a, uint32_t w)
{
    uint32_t nd = (w + 3) / 4, i;
    int started = 0;
    if (nd == 0) {
        sb_putc(out, '0');
        return;
    }
    for (i = nd; i-- > 0;) {
        unsigned nib = (unsigned)bv_get64(a, w, (uint64_t)i * 4, 4);
        if (!started && nib == 0 && i != 0)
            continue;
        started = 1;
        sb_putc(out, "0123456789abcdef"[nib]);
    }
}

#undef BV_LIMBS
