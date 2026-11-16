/* bignum.c — arbitrary-precision signed integers (sign-magnitude, 32-bit limbs). */
#include "bignum.h"
#include "str.h"
#include "map.h"

/* ------------------------------------------------------------ helpers --- */

static void bn_reserve(BigInt *r, uint32_t n)
{
    if (n > r->cap) {
        uint32_t nc = r->cap ? r->cap : 4;
        while (nc < n) {
            if (nc > UINT32_MAX / 2)
                tern_fatal("integer too large");
            nc *= 2;
        }
        r->d = (uint32_t *)xrealloc(r->d, (size_t)nc * sizeof(uint32_t));
        r->cap = nc;
    }
}

static void bn_trim(BigInt *r)
{
    while (r->len && r->d[r->len - 1] == 0)
        r->len--;
    if (!r->len)
        r->neg = 0;
}

static uint32_t bn_clz32(uint32_t x)
{
    uint32_t n = 0;
    if (!x)
        return 32;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
}

/* Set the magnitude of r to the given limbs (r must not alias src). */
static void bn_set_mag(BigInt *r, const uint32_t *src, uint32_t n, int neg)
{
    bn_reserve(r, n);
    if (n)
        memcpy(r->d, src, (size_t)n * sizeof(uint32_t));
    r->len = n;
    r->neg = neg;
    bn_trim(r);
}

static int bn_mag_cmp(const uint32_t *a, uint32_t alen, const uint32_t *b, uint32_t blen)
{
    uint32_t i;
    if (alen != blen)
        return alen < blen ? -1 : 1;
    for (i = alen; i-- > 0;) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

/* |r| = |r| * m + add, in place. */
static void bn_mag_muladd_small(BigInt *r, uint32_t m, uint32_t add)
{
    uint64_t carry = add;
    uint32_t i;
    for (i = 0; i < r->len; i++) {
        uint64_t cur = (uint64_t)r->d[i] * m + carry;
        r->d[i] = (uint32_t)cur;
        carry = cur >> 32;
    }
    if (carry) {
        bn_reserve(r, r->len + 1);
        r->d[r->len++] = (uint32_t)carry;
    }
}

/* In-place a[0..len) /= d; returns the remainder. */
static uint32_t bn_mag_divmod_small(uint32_t *a, uint32_t len, uint32_t d)
{
    uint64_t rem = 0;
    uint32_t i;
    for (i = len; i-- > 0;) {
        uint64_t cur = (rem << 32) | a[i];
        a[i] = (uint32_t)(cur / d);
        rem = cur % d;
    }
    return (uint32_t)rem;
}

/* Magnitude division (Knuth, TAOCP vol. 2, 4.3.1, algorithm D).
 * q = |a| / |b|, rem = |a| % |b|, both non-negative; q/rem may be NULL;
 * |b| must be non-zero; q and rem must not alias a or b. */
static void bn_mag_divmod(BigInt *q, BigInt *rem, const BigInt *a, const BigInt *b)
{
    uint32_t n = b->len, m, s, i, j;
    uint32_t *un, *vn;
    if (bn_mag_cmp(a->d, a->len, b->d, b->len) < 0) {
        if (q) {
            q->len = 0;
            q->neg = 0;
        }
        if (rem)
            bn_set_mag(rem, a->d, a->len, 0);
        return;
    }
    m = a->len - n;
    if (n == 1) {
        uint32_t *tmp = (uint32_t *)xmalloc((size_t)a->len * sizeof(uint32_t));
        uint32_t r1;
        memcpy(tmp, a->d, (size_t)a->len * sizeof(uint32_t));
        r1 = bn_mag_divmod_small(tmp, a->len, b->d[0]);
        if (q)
            bn_set_mag(q, tmp, a->len, 0);
        if (rem)
            bn_set_mag(rem, &r1, 1, 0);
        free(tmp);
        return;
    }
    s = bn_clz32(b->d[n - 1]);
    vn = (uint32_t *)xmalloc((size_t)n * sizeof(uint32_t));
    un = (uint32_t *)xmalloc((size_t)(a->len + 1) * sizeof(uint32_t));
    for (i = n - 1; i > 0; i--)
        vn[i] = (b->d[i] << s) | (s ? b->d[i - 1] >> (32 - s) : 0);
    vn[0] = b->d[0] << s;
    un[a->len] = s ? a->d[a->len - 1] >> (32 - s) : 0;
    for (i = a->len - 1; i > 0; i--)
        un[i] = (a->d[i] << s) | (s ? a->d[i - 1] >> (32 - s) : 0);
    un[0] = a->d[0] << s;
    if (q) {
        bn_reserve(q, m + 1);
        q->len = m + 1;
        q->neg = 0;
    }
    for (j = m + 1; j-- > 0;) {
        const uint64_t B = UINT64_C(1) << 32;
        uint64_t num = ((uint64_t)un[j + n] << 32) | un[j + n - 1];
        uint64_t qhat = num / vn[n - 1];
        uint64_t rhat = num % vn[n - 1];
        uint64_t carry = 0, borrow = 0, sub;
        while (qhat >= B || qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
            qhat--;
            rhat += vn[n - 1];
            if (rhat >= B)
                break;
        }
        /* un[j..j+n] -= qhat * vn */
        for (i = 0; i < n; i++) {
            uint64_t p = qhat * vn[i] + carry;
            carry = p >> 32;
            sub = (uint64_t)un[i + j] - (uint32_t)p - borrow;
            un[i + j] = (uint32_t)sub;
            borrow = sub >> 63;
        }
        sub = (uint64_t)un[j + n] - carry - borrow;
        un[j + n] = (uint32_t)sub;
        if (sub >> 63) {
            /* qhat was one too large: add vn back */
            uint64_t c = 0;
            qhat--;
            for (i = 0; i < n; i++) {
                uint64_t t = (uint64_t)un[i + j] + vn[i] + c;
                un[i + j] = (uint32_t)t;
                c = t >> 32;
            }
            un[j + n] = (uint32_t)(un[j + n] + c);
        }
        if (q)
            q->d[j] = (uint32_t)qhat;
    }
    if (q)
        bn_trim(q);
    if (rem) {
        bn_reserve(rem, n);
        for (i = 0; i < n; i++)
            rem->d[i] = (un[i] >> s) | (s ? un[i + 1] << (32 - s) : 0);
        rem->len = n;
        rem->neg = 0;
        bn_trim(rem);
    }
    free(vn);
    free(un);
}

/* --------------------------------------------------------- lifecycle ---- */

void bn_init(BigInt *r)
{
    r->d = NULL;
    r->len = r->cap = 0;
    r->neg = 0;
}

void bn_free(BigInt *r)
{
    free(r->d);
    bn_init(r);
}

void bn_copy(BigInt *r, const BigInt *a)
{
    if (r == a)
        return;
    bn_set_mag(r, a->d, a->len, a->neg);
}

void bn_swap(BigInt *a, BigInt *b)
{
    BigInt t = *a;
    *a = *b;
    *b = t;
}

void bn_move(BigInt *r, BigInt *a)
{
    if (r == a)
        return;
    free(r->d);
    *r = *a;
    bn_init(a);
}

/* ------------------------------------------------------- conversions ---- */

void bn_set_u64(BigInt *r, uint64_t v)
{
    bn_reserve(r, 2);
    r->d[0] = (uint32_t)v;
    r->d[1] = (uint32_t)(v >> 32);
    r->len = 2;
    r->neg = 0;
    bn_trim(r);
}

void bn_set_i64(BigInt *r, int64_t v)
{
    if (v < 0) {
        bn_set_u64(r, (uint64_t)(-(v + 1)) + 1u);
        r->neg = 1;
    } else {
        bn_set_u64(r, (uint64_t)v);
    }
}

static uint64_t bn_mag_low64(const BigInt *a)
{
    uint64_t v = 0;
    if (a->len > 0)
        v = a->d[0];
    if (a->len > 1)
        v |= (uint64_t)a->d[1] << 32;
    return v;
}

int bn_get_u64(const BigInt *a, uint64_t *out)
{
    if (a->neg || a->len > 2)
        return 0;
    if (out)
        *out = bn_mag_low64(a);
    return 1;
}

int bn_get_i64(const BigInt *a, int64_t *out)
{
    uint64_t m;
    if (a->len > 2)
        return 0;
    m = bn_mag_low64(a);
    if (!a->neg) {
        if (m > (uint64_t)INT64_MAX)
            return 0;
        if (out)
            *out = (int64_t)m;
        return 1;
    }
    if (m > (uint64_t)INT64_MAX + 1u)
        return 0;
    if (out)
        *out = m == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)m;
    return 1;
}

int bn_fits_i64(const BigInt *a) { return bn_get_i64(a, NULL); }
int bn_fits_u64(const BigInt *a) { return bn_get_u64(a, NULL); }

uint64_t bn_low_u64(const BigInt *a)
{
    uint64_t w;
    bn_to_limbs(a, &w, 64);
    return w;
}

void bn_from_limbs(BigInt *r, const uint64_t *w, uint32_t width, int is_signed)
{
    uint32_t n64 = (uint32_t)(((uint64_t)width + 63u) / 64u), i;
    int negative = 0;
    BigInt t = {0};
    if (width == 0) {
        r->len = 0;
        r->neg = 0;
        return;
    }
    bn_reserve(&t, n64 * 2);
    for (i = 0; i < n64; i++) {
        uint64_t v = w[i];
        if (i == n64 - 1 && width % 64)
            v &= (UINT64_C(1) << (width % 64)) - 1u;
        t.d[2 * i] = (uint32_t)v;
        t.d[2 * i + 1] = (uint32_t)(v >> 32);
    }
    t.len = n64 * 2;
    t.neg = 0;
    if (is_signed)
        negative = (int)((w[(width - 1) / 64] >> ((width - 1) % 64)) & 1u);
    bn_trim(&t);
    if (negative) {
        BigInt p = {0};
        bn_set_u64(&p, 1);
        bn_shl(&p, &p, width);
        bn_sub(&t, &t, &p);
        bn_free(&p);
    }
    bn_move(r, &t);
}

/* Two's complement form of a in L limbs (L > a->len), heap-allocated. */
static uint32_t *bn_to_tc(const BigInt *a, uint32_t L)
{
    uint32_t *t = (uint32_t *)xcalloc(L, sizeof(uint32_t));
    uint32_t i;
    if (a->len)
        memcpy(t, a->d, (size_t)a->len * sizeof(uint32_t));
    if (a->neg) {
        /* ~(|a| - 1) */
        for (i = 0; i < L; i++) {
            if (t[i]--)
                break;
        }
        for (i = 0; i < L; i++)
            t[i] = ~t[i];
    }
    return t;
}

/* r = value of the L-limb two's complement number t (t is consumed). */
static void bn_from_tc(BigInt *r, uint32_t *t, uint32_t L)
{
    int neg = L && (t[L - 1] & 0x80000000u);
    uint32_t i;
    if (neg) {
        for (i = 0; i < L; i++)
            t[i] = ~t[i];
        for (i = 0; i < L; i++) {
            if (++t[i])
                break;
        }
    }
    free(r->d);
    r->d = t;
    r->len = L;
    r->cap = L;
    r->neg = neg;
    bn_trim(r);
}

void bn_to_limbs(const BigInt *a, uint64_t *w, uint32_t width)
{
    uint32_t n64 = (uint32_t)(((uint64_t)width + 63u) / 64u), i;
    uint32_t L;
    uint32_t *t;
    if (width == 0)
        return;
    L = (a->len > 2 * n64 ? a->len : 2 * n64) + 1;
    t = bn_to_tc(a, L);
    for (i = 0; i < n64; i++)
        w[i] = (uint64_t)t[2 * i] | ((uint64_t)t[2 * i + 1] << 32);
    if (width % 64)
        w[n64 - 1] &= (UINT64_C(1) << (width % 64)) - 1u;
    free(t);
}

int bn_fits_width(const BigInt *a, uint32_t width, int is_signed)
{
    if (a->len == 0)
        return 1;
    if (width == 0)
        return 0;
    if (!is_signed)
        return !a->neg && bn_bitlen(a) <= width;
    return bn_signed_width(a) <= width;
}

static uint32_t bn_chunk_digits(int base, uint32_t *pow_out)
{
    /* largest k with base^k < 2^32 that we use per chunk */
    uint32_t k, p = 1, i;
    switch (base) {
    case 2: k = 31; break;
    case 8: k = 10; break;
    case 10: k = 9; break;
    default: k = 7; break; /* 16 */
    }
    for (i = 0; i < k; i++)
        p *= (uint32_t)base;
    if (pow_out)
        *pow_out = p;
    return k;
}

int bn_parse(BigInt *r, const char *s, size_t n, int base)
{
    size_t i = 0;
    int neg = 0, prefixed = 0, ndig = 0;
    uint32_t chunk = 0, cnt = 0, k, cpow = 1;
    r->len = 0;
    r->neg = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        i++;
    }
    if (base == 0) {
        base = 10;
        if (i + 1 < n && s[i] == '0') {
            int c = tern_to_lower((unsigned char)s[i + 1]);
            if (c == 'x')
                base = 16;
            else if (c == 'o')
                base = 8;
            else if (c == 'b')
                base = 2;
            if (base != 10) {
                i += 2;
                prefixed = 1;
            }
        }
    } else if (base != 2 && base != 8 && base != 10 && base != 16) {
        return 0;
    }
    if (!prefixed && i < n && s[i] == '_')
        return 0;
    k = bn_chunk_digits(base, NULL);
    for (; i < n; i++) {
        int d;
        if (s[i] == '_')
            continue;
        d = tern_hex_value((unsigned char)s[i]);
        if (d < 0 || d >= base) {
            r->len = 0;
            return 0;
        }
        chunk = chunk * (uint32_t)base + (uint32_t)d;
        cpow *= (uint32_t)base;
        ndig++;
        if (++cnt == k) {
            bn_mag_muladd_small(r, cpow, chunk);
            chunk = 0;
            cnt = 0;
            cpow = 1;
        }
    }
    if (cnt)
        bn_mag_muladd_small(r, cpow, chunk);
    if (!ndig) {
        r->len = 0;
        return 0;
    }
    r->neg = neg;
    bn_trim(r);
    return 1;
}

int bn_parse_cstr(BigInt *r, const char *s, int base)
{
    return bn_parse(r, s, strlen(s), base);
}

void bn_to_sb(StrBuf *out, const BigInt *a, int base)
{
    static const char digits[] = "0123456789abcdef";
    uint32_t *tmp, *chunks;
    uint32_t len, nchunks = 0, k, cpow, i, j;
    char buf[40];
    if (base != 2 && base != 8 && base != 16)
        base = 10;
    if (a->len == 0) {
        sb_putc(out, '0');
        return;
    }
    if (a->neg)
        sb_putc(out, '-');
    k = bn_chunk_digits(base, &cpow);
    len = a->len;
    tmp = (uint32_t *)xmalloc((size_t)len * sizeof(uint32_t));
    memcpy(tmp, a->d, (size_t)len * sizeof(uint32_t));
    /* each chunk is < cpow and holds k digits; at most 32*len/log2(base)/k + 1 */
    chunks = (uint32_t *)xmalloc(((size_t)len * 32u + 1u) * sizeof(uint32_t));
    while (len) {
        chunks[nchunks++] = bn_mag_divmod_small(tmp, len, cpow);
        while (len && tmp[len - 1] == 0)
            len--;
    }
    for (i = nchunks; i-- > 0;) {
        uint32_t v = chunks[i];
        uint32_t nd = 0;
        do {
            buf[nd++] = digits[v % (uint32_t)base];
            v /= (uint32_t)base;
        } while (v);
        if (i != nchunks - 1) {
            while (nd < k)
                buf[nd++] = '0';
        }
        for (j = nd; j-- > 0;)
            sb_putc(out, buf[j]);
    }
    free(tmp);
    free(chunks);
}

char *bn_to_str(const BigInt *a, int base)
{
    StrBuf sb = {0};
    bn_to_sb(&sb, a, base);
    return sb_take(&sb);
}

/* -------------------------------------------------------- arithmetic ---- */

void bn_neg(BigInt *r, const BigInt *a)
{
    bn_copy(r, a);
    if (r->len)
        r->neg = !r->neg;
}

void bn_abs(BigInt *r, const BigInt *a)
{
    bn_copy(r, a);
    r->neg = 0;
}

/* r = a + (b_neg_flip ? -b : b) */
static void bn_addsub(BigInt *r, const BigInt *a, const BigInt *b, int flip)
{
    BigInt t = {0};
    int bneg = b->len ? (b->neg ^ flip) : 0;
    const BigInt *x = a, *y = b;
    uint32_t i;
    if (a->neg == bneg) {
        uint32_t n = a->len > b->len ? a->len : b->len;
        uint64_t carry = 0;
        bn_reserve(&t, n + 1);
        for (i = 0; i < n; i++) {
            uint64_t s = carry;
            if (i < a->len)
                s += a->d[i];
            if (i < b->len)
                s += b->d[i];
            t.d[i] = (uint32_t)s;
            carry = s >> 32;
        }
        t.d[n] = (uint32_t)carry;
        t.len = n + 1;
        t.neg = a->neg;
    } else {
        int c = bn_mag_cmp(a->d, a->len, b->d, b->len);
        uint64_t borrow = 0;
        int sign;
        if (c == 0) {
            bn_free(&t);
            r->len = 0;
            r->neg = 0;
            return;
        }
        if (c < 0) {
            x = b;
            y = a;
            sign = bneg;
        } else {
            sign = a->neg;
        }
        bn_reserve(&t, x->len);
        for (i = 0; i < x->len; i++) {
            uint64_t s = (uint64_t)x->d[i] - (i < y->len ? y->d[i] : 0u) - borrow;
            t.d[i] = (uint32_t)s;
            borrow = s >> 63;
        }
        t.len = x->len;
        t.neg = sign;
    }
    bn_trim(&t);
    bn_move(r, &t);
}

void bn_add(BigInt *r, const BigInt *a, const BigInt *b) { bn_addsub(r, a, b, 0); }
void bn_sub(BigInt *r, const BigInt *a, const BigInt *b) { bn_addsub(r, a, b, 1); }

void bn_mul(BigInt *r, const BigInt *a, const BigInt *b)
{
    BigInt t = {0};
    uint32_t i, j;
    if (!a->len || !b->len) {
        r->len = 0;
        r->neg = 0;
        return;
    }
    if ((uint64_t)a->len + b->len > UINT32_MAX / 2)
        tern_fatal("integer too large");
    bn_reserve(&t, a->len + b->len);
    memset(t.d, 0, (size_t)(a->len + b->len) * sizeof(uint32_t));
    for (i = 0; i < a->len; i++) {
        uint64_t carry = 0;
        for (j = 0; j < b->len; j++) {
            uint64_t cur = (uint64_t)a->d[i] * b->d[j] + t.d[i + j] + carry;
            t.d[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        t.d[i + b->len] = (uint32_t)carry;
    }
    t.len = a->len + b->len;
    t.neg = a->neg ^ b->neg;
    bn_trim(&t);
    bn_move(r, &t);
}

void bn_add_i64(BigInt *r, const BigInt *a, int64_t b)
{
    BigInt t = {0};
    bn_set_i64(&t, b);
    bn_add(r, a, &t);
    bn_free(&t);
}

void bn_mul_i64(BigInt *r, const BigInt *a, int64_t b)
{
    BigInt t = {0};
    bn_set_i64(&t, b);
    bn_mul(r, a, &t);
    bn_free(&t);
}

static int bn_divmod_impl(BigInt *q, BigInt *rem, const BigInt *a, const BigInt *b, int floor_div)
{
    BigInt tq = {0}, tr = {0};
    if (!b->len)
        return 0;
    bn_mag_divmod(&tq, &tr, a, b);
    if (tq.len)
        tq.neg = a->neg ^ b->neg;
    if (tr.len)
        tr.neg = a->neg;
    if (floor_div && tr.len && a->neg != b->neg) {
        BigInt one = {0};
        bn_set_u64(&one, 1);
        bn_sub(&tq, &tq, &one);
        bn_add(&tr, &tr, b);
        bn_free(&one);
    }
    if (q)
        bn_move(q, &tq);
    else
        bn_free(&tq);
    if (rem)
        bn_move(rem, &tr);
    else
        bn_free(&tr);
    return 1;
}

int bn_divmod_trunc(BigInt *q, BigInt *rem, const BigInt *a, const BigInt *b)
{
    return bn_divmod_impl(q, rem, a, b, 0);
}

int bn_divmod_floor(BigInt *q, BigInt *rem, const BigInt *a, const BigInt *b)
{
    return bn_divmod_impl(q, rem, a, b, 1);
}

void bn_pow(BigInt *r, const BigInt *a, uint64_t e)
{
    BigInt res = {0}, base = {0};
    bn_set_u64(&res, 1);
    bn_copy(&base, a);
    while (e) {
        if (e & 1u)
            bn_mul(&res, &res, &base);
        e >>= 1;
        if (e)
            bn_mul(&base, &base, &base);
    }
    bn_free(&base);
    bn_move(r, &res);
}

void bn_shl(BigInt *r, const BigInt *a, uint64_t n)
{
    BigInt t = {0};
    uint64_t words64 = n / 32;
    uint32_t words, bits = (uint32_t)(n % 32), i;
    if (!a->len) {
        r->len = 0;
        r->neg = 0;
        return;
    }
    if (words64 + a->len + 1 > UINT32_MAX / 2)
        tern_fatal("integer too large (shift by %lu)", (unsigned long)n);
    words = (uint32_t)words64;
    bn_reserve(&t, a->len + words + 1);
    memset(t.d, 0, (size_t)(a->len + words + 1) * sizeof(uint32_t));
    for (i = 0; i < a->len; i++) {
        uint64_t v = (uint64_t)a->d[i] << bits;
        t.d[i + words] |= (uint32_t)v;
        t.d[i + words + 1] |= (uint32_t)(v >> 32);
    }
    t.len = a->len + words + 1;
    t.neg = a->neg;
    bn_trim(&t);
    bn_move(r, &t);
}

void bn_shr(BigInt *r, const BigInt *a, uint64_t n)
{
    BigInt t = {0};
    uint64_t words64 = n / 32;
    uint32_t words, bits = (uint32_t)(n % 32), i, len;
    int lost = 0;
    if (!a->len) {
        r->len = 0;
        r->neg = 0;
        return;
    }
    if (words64 >= a->len) {
        if (a->neg)
            bn_set_i64(r, -1);
        else {
            r->len = 0;
            r->neg = 0;
        }
        return;
    }
    words = (uint32_t)words64;
    for (i = 0; i < words; i++) {
        if (a->d[i]) {
            lost = 1;
            break;
        }
    }
    if (bits && (a->d[words] & ((1u << bits) - 1u)))
        lost = 1;
    len = a->len - words;
    bn_reserve(&t, len + 1);
    for (i = 0; i < len; i++) {
        uint32_t lo = a->d[i + words] >> bits;
        uint32_t hi = 0;
        if (bits && i + words + 1 < a->len)
            hi = a->d[i + words + 1] << (32 - bits);
        t.d[i] = lo | hi;
    }
    t.len = len;
    t.neg = 0;
    bn_trim(&t);
    if (a->neg) {
        if (lost) {
            /* floor: magnitude + 1 */
            t.d[t.len] = 0;
            for (i = 0; i <= t.len; i++) {
                if (++t.d[i])
                    break;
            }
            if (t.d[t.len])
                t.len++;
        }
        if (t.len)
            t.neg = 1;
    }
    bn_trim(&t);
    bn_move(r, &t);
}

enum { BN_OP_AND, BN_OP_OR, BN_OP_XOR };

static void bn_bitop(BigInt *r, const BigInt *a, const BigInt *b, int op)
{
    uint32_t L = (a->len > b->len ? a->len : b->len) + 1, i;
    uint32_t *x = bn_to_tc(a, L);
    uint32_t *y = bn_to_tc(b, L);
    for (i = 0; i < L; i++) {
        if (op == BN_OP_AND)
            x[i] &= y[i];
        else if (op == BN_OP_OR)
            x[i] |= y[i];
        else
            x[i] ^= y[i];
    }
    free(y);
    bn_from_tc(r, x, L);
}

void bn_and(BigInt *r, const BigInt *a, const BigInt *b) { bn_bitop(r, a, b, BN_OP_AND); }
void bn_or(BigInt *r, const BigInt *a, const BigInt *b) { bn_bitop(r, a, b, BN_OP_OR); }
void bn_xor(BigInt *r, const BigInt *a, const BigInt *b) { bn_bitop(r, a, b, BN_OP_XOR); }

void bn_not(BigInt *r, const BigInt *a)
{
    BigInt one = {0};
    bn_set_u64(&one, 1);
    bn_neg(r, a);
    bn_sub(r, r, &one);
    bn_free(&one);
}

/* ------------------------------------------------------------ queries --- */

int bn_cmp_abs(const BigInt *a, const BigInt *b)
{
    return bn_mag_cmp(a->d, a->len, b->d, b->len);
}

int bn_cmp(const BigInt *a, const BigInt *b)
{
    int sa = bn_sign(a), sb = bn_sign(b), c;
    if (sa != sb)
        return sa < sb ? -1 : 1;
    c = bn_mag_cmp(a->d, a->len, b->d, b->len);
    return a->neg ? -c : c;
}

int bn_cmp_i64(const BigInt *a, int64_t b)
{
    BigInt t = {0};
    int c;
    bn_set_i64(&t, b);
    c = bn_cmp(a, &t);
    bn_free(&t);
    return c;
}

int bn_sign(const BigInt *a)
{
    if (!a->len)
        return 0;
    return a->neg ? -1 : 1;
}

int bn_is_zero(const BigInt *a) { return a->len == 0; }
int bn_is_neg(const BigInt *a) { return a->neg && a->len; }

uint64_t bn_bitlen(const BigInt *a)
{
    if (!a->len)
        return 0;
    return (uint64_t)(a->len - 1) * 32u + (32u - bn_clz32(a->d[a->len - 1]));
}

uint64_t bn_signed_width(const BigInt *a)
{
    uint64_t w;
    if (!a->neg)
        return bn_bitlen(a) + 1;
    {
        BigInt t = {0};
        bn_not(&t, a); /* -a - 1 >= 0 */
        w = bn_bitlen(&t) + 1;
        bn_free(&t);
    }
    return w;
}

static int bn_mag_bit(const BigInt *a, uint64_t i)
{
    uint64_t limb = i / 32;
    if (limb >= a->len)
        return 0;
    return (int)((a->d[limb] >> (i % 32)) & 1u);
}

int bn_get_bit(const BigInt *a, uint64_t i)
{
    uint64_t k;
    if (!a->neg)
        return bn_mag_bit(a, i);
    /* two's complement of -m: bits below the lowest set bit k of m are 0,
     * bit k is 1, bits above k are the complement of m's bits. */
    k = 0;
    while (!bn_mag_bit(a, k))
        k++;
    if (i < k)
        return 0;
    if (i == k)
        return 1;
    return !bn_mag_bit(a, i);
}

void bn_set_bit(BigInt *r, uint64_t i, int v)
{
    BigInt m = {0};
    bn_set_u64(&m, 1);
    bn_shl(&m, &m, i);
    if (v) {
        bn_or(r, r, &m);
    } else {
        bn_not(&m, &m);
        bn_and(r, r, &m);
    }
    bn_free(&m);
}

uint64_t bn_hash(const BigInt *a)
{
    uint64_t h = hash_bytes(a->d ? (const void *)a->d : (const void *)"", (size_t)a->len * sizeof(uint32_t));
    return hash_combine(h, (uint64_t)a->neg);
}
