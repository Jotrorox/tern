/* test_base.c — unit tests for src/base and src/os.
 *
 *     cc -o build/test_base tests/unit/test_base.c && build/test_base
 *
 * A single translation unit that includes the sources under test. */
#include "../../src/os/os_posix.c"
#include "../../src/os/os_win32.c"
#include "../../src/os/os_common.c"
#include "../../src/base/common.c"
#include "../../src/base/arena.c"
#include "../../src/base/str.c"
#include "../../src/base/map.c"
#include "../../src/base/bignum.c"
#include "../../src/base/json.c"
#include "../../src/base/utf8.c"
#include "../../src/base/diag.c"
#include "../../src/base/args.c"

/* ------------------------------------------------------------ framework */

static int tb_checks, tb_fails;

static void tb_check(int ok, const char *expr, const char *file, int line)
{
    tb_checks++;
    if (!ok) {
        tb_fails++;
        if (tb_fails <= 50)
            fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    }
}

#define TB_CHECK(c) tb_check((c) ? 1 : 0, #c, __FILE__, __LINE__)
#define TB_STREQ(a, b) tb_streq((a), (b), #a, __FILE__, __LINE__)

static void tb_streq(const char *a, const char *b, const char *expr, const char *file, int line)
{
    int ok = a && b && strcmp(a, b) == 0;
    tb_checks++;
    if (!ok) {
        tb_fails++;
        if (tb_fails <= 50)
            fprintf(stderr, "%s:%d: %s\n    got:      \"%s\"\n    expected: \"%s\"\n", file, line, expr,
                    a ? a : "(null)", b ? b : "(null)");
    }
}

/* xorshift64* — deterministic pseudo-random numbers */
static uint64_t tb_rng = UINT64_C(0x9E3779B97F4A7C15);

static uint64_t tb_rand(void)
{
    tb_rng ^= tb_rng >> 12;
    tb_rng ^= tb_rng << 25;
    tb_rng ^= tb_rng >> 27;
    return tb_rng * UINT64_C(2685821657736338717);
}

/* random int64 with a random magnitude (so small values are common) */
static int64_t tb_rand_i64(int max_bits)
{
    int bits = (int)(tb_rand() % (uint64_t)max_bits) + 1;
    uint64_t m = bits >= 64 ? tb_rand() : (tb_rand() & ((UINT64_C(1) << bits) - 1));
    int64_t v;
    if (bits >= 63)
        m &= (UINT64_C(1) << 62) - 1;
    v = (int64_t)m;
    return (tb_rand() & 1) ? -v : v;
}

/* random BigInt with up to `limbs` 32-bit limbs, biased towards edge limbs */
static void tb_rand_big(BigInt *r, uint32_t limbs)
{
    static const uint32_t edge[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu};
    uint32_t n = (uint32_t)(tb_rand() % limbs) + 1, i;
    BigInt t = {0};
    for (i = 0; i < n; i++) {
        uint32_t limb = (tb_rand() % 3 == 0) ? edge[tb_rand() % TERN_ARRAY_LEN(edge)] : (uint32_t)tb_rand();
        bn_shl(&t, &t, 32);
        bn_add_i64(&t, &t, (int64_t)limb);
    }
    if (tb_rand() & 1)
        bn_neg(&t, &t);
    bn_move(r, &t);
}

static int tb_bn_eq_str(const BigInt *a, const char *dec)
{
    char *s = bn_to_str(a, 10);
    int ok = strcmp(s, dec) == 0;
    if (!ok)
        fprintf(stderr, "    bignum: got %s, expected %s\n", s, dec);
    free(s);
    return ok;
}

static int tb_bn_eq_i64(const BigInt *a, int64_t v)
{
    int64_t got;
    return bn_get_i64(a, &got) && got == v;
}

/* --------------------------------------------------------------- bignum */

static void test_bignum_parse_format(void)
{
    BigInt a = {0}, b = {0};
    char *s;
    int i;

    TB_CHECK(bn_parse_cstr(&a, "0", 10) && bn_is_zero(&a));
    TB_CHECK(bn_parse_cstr(&a, "-0", 10) && bn_is_zero(&a) && !a.neg);
    TB_CHECK(bn_parse_cstr(&a, "123_456", 10) && tb_bn_eq_i64(&a, 123456));
    TB_CHECK(bn_parse_cstr(&a, "1__2_", 10) && tb_bn_eq_i64(&a, 12));
    TB_CHECK(bn_parse_cstr(&a, "+5", 10) && tb_bn_eq_i64(&a, 5));
    TB_CHECK(bn_parse_cstr(&a, "0xFF", 0) && tb_bn_eq_i64(&a, 255));
    TB_CHECK(bn_parse_cstr(&a, "0Xff_ff", 0) && tb_bn_eq_i64(&a, 65535));
    TB_CHECK(bn_parse_cstr(&a, "0b1010", 0) && tb_bn_eq_i64(&a, 10));
    TB_CHECK(bn_parse_cstr(&a, "0o777", 0) && tb_bn_eq_i64(&a, 511));
    TB_CHECK(bn_parse_cstr(&a, "0x_1", 0) && tb_bn_eq_i64(&a, 1));
    TB_CHECK(bn_parse_cstr(&a, "777", 8) && tb_bn_eq_i64(&a, 511));
    TB_CHECK(bn_parse_cstr(&a, "deadBEEF", 16) && tb_bn_eq_i64(&a, 0xDEADBEEFLL));
    TB_CHECK(bn_parse_cstr(&a, "-0x8000_0000_0000_0000", 0) && tb_bn_eq_i64(&a, INT64_MIN));
    TB_CHECK(bn_parse_cstr(&a, "007", 10) && tb_bn_eq_i64(&a, 7));
    TB_CHECK(!bn_parse_cstr(&a, "", 10));
    TB_CHECK(!bn_parse_cstr(&a, "-", 10));
    TB_CHECK(!bn_parse_cstr(&a, "_1", 10));
    TB_CHECK(!bn_parse_cstr(&a, "12a", 10));
    TB_CHECK(!bn_parse_cstr(&a, "0x", 0));
    TB_CHECK(!bn_parse_cstr(&a, "0b2", 0));
    TB_CHECK(!bn_parse_cstr(&a, "0o8", 0));
    TB_CHECK(!bn_parse_cstr(&a, "12", 3));
    TB_CHECK(!bn_parse_cstr(&a, "1 2", 10));
    TB_CHECK(bn_is_zero(&a));

    TB_CHECK(bn_parse_cstr(&a, "340282366920938463463374607431768211456", 10));
    s = bn_to_str(&a, 16);
    TB_STREQ(s, "100000000000000000000000000000000");
    free(s);
    TB_CHECK(bn_bitlen(&a) == 129);
    s = bn_to_str(&a, 2);
    TB_CHECK(strlen(s) == 129 && s[0] == '1' && strspn(s + 1, "0") == 128);
    free(s);
    s = bn_to_str(&a, 8);
    TB_STREQ(s, "4000000000000000000000000000000000000000000");
    free(s);
    bn_neg(&a, &a);
    s = bn_to_str(&a, 10);
    TB_STREQ(s, "-340282366920938463463374607431768211456");
    free(s);
    bn_set_i64(&a, 0);
    s = bn_to_str(&a, 16);
    TB_STREQ(s, "0");
    free(s);
    /* chunk padding: 10^9 and 10^18 boundaries */
    TB_CHECK(bn_parse_cstr(&a, "1000000000000000000000000001", 10) && tb_bn_eq_str(&a, "1000000000000000000000000001"));
    TB_CHECK(bn_parse_cstr(&a, "1000000000", 10) && tb_bn_eq_str(&a, "1000000000"));

    /* random round trips in every base, cross-checked */
    for (i = 0; i < 300; i++) {
        static const int bases[] = {2, 8, 10, 16};
        char buf[400];
        int len = (int)(tb_rand() % 300) + 1, k, bi;
        int base = bases[tb_rand() % 4];
        const char *digits = "0123456789abcdef";
        buf[0] = digits[1 + tb_rand() % (unsigned)(base - 1)];
        for (k = 1; k < len; k++)
            buf[k] = digits[tb_rand() % (unsigned)base];
        buf[len] = '\0';
        TB_CHECK(bn_parse_cstr(&a, buf, base));
        s = bn_to_str(&a, base);
        TB_STREQ(s, buf);
        free(s);
        for (bi = 0; bi < 4; bi++) {
            s = bn_to_str(&a, bases[bi]);
            TB_CHECK(bn_parse_cstr(&b, s, bases[bi]) && bn_cmp(&a, &b) == 0);
            free(s);
        }
    }
    bn_free(&a);
    bn_free(&b);
}

static void test_bignum_conversions(void)
{
    BigInt a = {0};
    int64_t i64;
    uint64_t u64;

    bn_set_i64(&a, INT64_MIN);
    TB_CHECK(tb_bn_eq_str(&a, "-9223372036854775808"));
    TB_CHECK(bn_get_i64(&a, &i64) && i64 == INT64_MIN);
    TB_CHECK(!bn_get_u64(&a, &u64));
    TB_CHECK(bn_low_u64(&a) == UINT64_C(0x8000000000000000));
    bn_set_i64(&a, INT64_MAX);
    TB_CHECK(bn_get_i64(&a, &i64) && i64 == INT64_MAX);
    bn_add_i64(&a, &a, 1);
    TB_CHECK(!bn_get_i64(&a, &i64));
    TB_CHECK(bn_get_u64(&a, &u64) && u64 == UINT64_C(0x8000000000000000));
    bn_set_u64(&a, UINT64_MAX);
    TB_CHECK(tb_bn_eq_str(&a, "18446744073709551615"));
    TB_CHECK(bn_get_u64(&a, &u64) && u64 == UINT64_MAX);
    TB_CHECK(!bn_fits_i64(&a));
    bn_add_i64(&a, &a, 1);
    TB_CHECK(!bn_fits_u64(&a));
    TB_CHECK(bn_low_u64(&a) == 0);
    bn_set_i64(&a, -1);
    TB_CHECK(bn_low_u64(&a) == UINT64_MAX);
    TB_CHECK(!bn_fits_u64(&a));
    bn_set_i64(&a, -9223372036854775807LL - 1);
    bn_add_i64(&a, &a, -1);
    TB_CHECK(!bn_fits_i64(&a));
    TB_CHECK(bn_low_u64(&a) == UINT64_C(0x7FFFFFFFFFFFFFFF));
    bn_free(&a);
}

static void test_bignum_small_arith(void)
{
    BigInt a = {0}, b = {0}, r = {0}, q = {0};
    int i;
    for (i = 0; i < 20000; i++) {
        int64_t x = tb_rand_i64(62), y = tb_rand_i64(62);
        int64_t xs = tb_rand_i64(31), ys = tb_rand_i64(31);
        bn_set_i64(&a, x);
        bn_set_i64(&b, y);
        bn_add(&r, &a, &b);
        TB_CHECK(tb_bn_eq_i64(&r, x + y));
        bn_sub(&r, &a, &b);
        TB_CHECK(tb_bn_eq_i64(&r, x - y));
        TB_CHECK(bn_cmp(&a, &b) == (x < y ? -1 : x > y ? 1 : 0));
        TB_CHECK(bn_cmp_i64(&a, y) == (x < y ? -1 : x > y ? 1 : 0));
        TB_CHECK(bn_sign(&a) == (x < 0 ? -1 : x > 0 ? 1 : 0));
        if (y != 0) {
            int64_t tq = x / y, tr = x % y;
            int64_t fq = tq, fr = tr;
            if (tr != 0 && ((tr < 0) != (y < 0))) {
                fq -= 1;
                fr += y;
            }
            TB_CHECK(bn_divmod_trunc(&q, &r, &a, &b));
            TB_CHECK(tb_bn_eq_i64(&q, tq) && tb_bn_eq_i64(&r, tr));
            TB_CHECK(bn_divmod_floor(&q, &r, &a, &b));
            TB_CHECK(tb_bn_eq_i64(&q, fq) && tb_bn_eq_i64(&r, fr));
        } else {
            TB_CHECK(!bn_divmod_trunc(&q, &r, &a, &b));
        }
        /* aliasing: r = r op r */
        bn_copy(&r, &a);
        bn_add(&r, &r, &r);
        TB_CHECK(tb_bn_eq_i64(&r, 2 * x));
        bn_set_i64(&a, xs);
        bn_set_i64(&b, ys);
        bn_mul(&r, &a, &b);
        TB_CHECK(tb_bn_eq_i64(&r, xs * ys));
        bn_mul(&a, &a, &a);
        TB_CHECK(tb_bn_eq_i64(&a, xs * xs));
        /* bitwise on two's complement */
        bn_set_i64(&a, x);
        bn_set_i64(&b, y);
        bn_and(&r, &a, &b);
        TB_CHECK(tb_bn_eq_i64(&r, x & y));
        bn_or(&r, &a, &b);
        TB_CHECK(tb_bn_eq_i64(&r, x | y));
        bn_xor(&r, &a, &b);
        TB_CHECK(tb_bn_eq_i64(&r, x ^ y));
        bn_not(&r, &a);
        TB_CHECK(tb_bn_eq_i64(&r, ~x));
        {
            uint64_t bit = tb_rand() % 70;
            int expect = bit >= 63 ? (x < 0) : (int)(((uint64_t)x >> bit) & 1u);
            TB_CHECK(bn_get_bit(&a, bit) == expect);
        }
        {
            unsigned sh = (unsigned)(tb_rand() % 62);
            /* arithmetic shift right == floor division by 2^sh */
            int64_t expect = x >= 0 ? x >> sh : -(((-x) + (((int64_t)1 << sh) - 1)) >> sh);
            bn_shr(&r, &a, sh);
            TB_CHECK(tb_bn_eq_i64(&r, expect));
        }
    }
    bn_free(&a);
    bn_free(&b);
    bn_free(&r);
    bn_free(&q);
}

static void test_bignum_big_identities(void)
{
    BigInt a = {0}, b = {0}, c = {0}, q = {0}, r = {0}, t = {0}, u = {0};
    int i;
    for (i = 0; i < 4000; i++) {
        tb_rand_big(&a, 12);
        tb_rand_big(&b, (i & 1) ? 12 : 3);
        tb_rand_big(&c, 6);
        /* (a + b) - b == a */
        bn_add(&t, &a, &b);
        bn_sub(&t, &t, &b);
        TB_CHECK(bn_cmp(&t, &a) == 0);
        /* a*b == b*a; (a+b)*c == a*c + b*c */
        bn_mul(&t, &a, &b);
        bn_mul(&u, &b, &a);
        TB_CHECK(bn_cmp(&t, &u) == 0);
        bn_add(&t, &a, &b);
        bn_mul(&t, &t, &c);
        bn_mul(&u, &a, &c);
        bn_mul(&q, &b, &c);
        bn_add(&u, &u, &q);
        TB_CHECK(bn_cmp(&t, &u) == 0);
        if (!bn_is_zero(&b)) {
            /* truncating: a == q*b + r, |r| < |b|, r has a's sign */
            TB_CHECK(bn_divmod_trunc(&q, &r, &a, &b));
            bn_mul(&t, &q, &b);
            bn_add(&t, &t, &r);
            TB_CHECK(bn_cmp(&t, &a) == 0);
            TB_CHECK(bn_cmp_abs(&r, &b) < 0);
            TB_CHECK(bn_is_zero(&r) || bn_sign(&r) == bn_sign(&a));
            /* floor: r has b's sign */
            TB_CHECK(bn_divmod_floor(&q, &r, &a, &b));
            bn_mul(&t, &q, &b);
            bn_add(&t, &t, &r);
            TB_CHECK(bn_cmp(&t, &a) == 0);
            TB_CHECK(bn_cmp_abs(&r, &b) < 0);
            TB_CHECK(bn_is_zero(&r) || bn_sign(&r) == bn_sign(&b));
            /* exact division */
            bn_mul(&t, &a, &b);
            TB_CHECK(bn_divmod_trunc(&q, &r, &t, &b));
            TB_CHECK(bn_cmp(&q, &a) == 0 && bn_is_zero(&r));
            /* q alone / r alone */
            TB_CHECK(bn_divmod_trunc(&q, NULL, &t, &b) && bn_cmp(&q, &a) == 0);
            TB_CHECK(bn_divmod_floor(NULL, &r, &t, &b) && bn_is_zero(&r));
        }
        /* bitwise identities */
        bn_and(&t, &a, &b);
        bn_or(&u, &a, &b);
        bn_add(&t, &t, &u);
        bn_add(&u, &a, &b);
        TB_CHECK(bn_cmp(&t, &u) == 0); /* (a&b) + (a|b) == a + b */
        bn_xor(&t, &a, &b);
        bn_xor(&t, &t, &b);
        TB_CHECK(bn_cmp(&t, &a) == 0);
        bn_and(&t, &a, &b);
        bn_or(&u, &a, &b);
        bn_sub(&u, &u, &t);
        bn_xor(&t, &a, &b);
        TB_CHECK(bn_cmp(&t, &u) == 0); /* a^b == (a|b) - (a&b) */
        bn_not(&t, &a);
        bn_neg(&u, &a);
        bn_add_i64(&u, &u, -1);
        TB_CHECK(bn_cmp(&t, &u) == 0);
        /* shifts against multiplication / floor division by powers of two */
        {
            uint64_t n = tb_rand() % 200;
            BigInt p = {0};
            bn_set_i64(&p, 2);
            bn_pow(&p, &p, n);
            bn_shl(&t, &a, n);
            bn_mul(&u, &a, &p);
            TB_CHECK(bn_cmp(&t, &u) == 0);
            bn_shr(&t, &a, n);
            bn_divmod_floor(&u, NULL, &a, &p);
            TB_CHECK(bn_cmp(&t, &u) == 0);
            /* two's complement bit i of a == bit 0 of (a >> i) */
            TB_CHECK(bn_get_bit(&a, n) == (int)(bn_low_u64(&t) & 1u));
            bn_free(&p);
        }
    }
    bn_free(&a);
    bn_free(&b);
    bn_free(&c);
    bn_free(&q);
    bn_free(&r);
    bn_free(&t);
    bn_free(&u);
}

static void test_bignum_knuth_vectors(void)
{
    /* divisions that exercise the qhat correction and add-back paths */
    static const char *const cases[][4] = {
        /* a, b, q, r (hex) */
        {"7fffffff800000000000000000000000", "800000000000000000000001", "fffffffe", "7fffffffffffffff00000002"},
        {"800000000000000000000003", "200000000000000000000001", "3", "200000000000000000000000"},
        {"80000000000000000000000000000000", "800000000000000000000001", "ffffffff", "7fffffffffffffff00000001"},
        {"ffffffffffffffffffffffffffffffff", "ffffffffffffffff", "10000000000000001", "0"},
        {"10000000000000000", "ffffffff", "100000001", "1"},
        {"fffffffffffffffffffffffe00000001", "ffffffffffffffff", "10000000000000000", "fffffffe00000001"},
    };
    BigInt a = {0}, b = {0}, q = {0}, r = {0}, t = {0};
    size_t i;
    for (i = 0; i < TERN_ARRAY_LEN(cases); i++) {
        char *s;
        TB_CHECK(bn_parse_cstr(&a, cases[i][0], 16));
        TB_CHECK(bn_parse_cstr(&b, cases[i][1], 16));
        TB_CHECK(bn_divmod_trunc(&q, &r, &a, &b));
        /* verify by identity (the expected strings double-check the code path) */
        bn_mul(&t, &q, &b);
        bn_add(&t, &t, &r);
        TB_CHECK(bn_cmp(&t, &a) == 0 && bn_cmp_abs(&r, &b) < 0);
        s = bn_to_str(&q, 16);
        TB_STREQ(s, cases[i][2]);
        free(s);
        s = bn_to_str(&r, 16);
        TB_STREQ(s, cases[i][3]);
        free(s);
    }
    bn_free(&a);
    bn_free(&b);
    bn_free(&q);
    bn_free(&r);
    bn_free(&t);
}

static void test_bignum_bits_widths(void)
{
    BigInt a = {0}, b = {0}, m = {0}, p = {0}, r = {0};
    uint64_t w[4];
    static const uint32_t widths[] = {1, 2, 7, 8, 31, 32, 33, 63, 64, 65, 100, 128, 129, 200, 256};
    size_t wi;
    int i;

    bn_set_i64(&a, 0);
    TB_CHECK(bn_bitlen(&a) == 0 && bn_signed_width(&a) == 1);
    bn_set_i64(&a, 1);
    TB_CHECK(bn_bitlen(&a) == 1 && bn_signed_width(&a) == 2);
    bn_set_i64(&a, 255);
    TB_CHECK(bn_bitlen(&a) == 8 && bn_signed_width(&a) == 9);
    bn_set_i64(&a, -1);
    TB_CHECK(bn_bitlen(&a) == 1 && bn_signed_width(&a) == 1);
    bn_set_i64(&a, -128);
    TB_CHECK(bn_signed_width(&a) == 8);
    bn_set_i64(&a, -129);
    TB_CHECK(bn_signed_width(&a) == 9);
    bn_set_i64(&a, 127);
    TB_CHECK(bn_signed_width(&a) == 8);
    TB_CHECK(bn_fits_width(&a, 7, 0) && !bn_fits_width(&a, 6, 0) && bn_fits_width(&a, 8, 1) && !bn_fits_width(&a, 7, 1));
    bn_set_i64(&a, -128);
    TB_CHECK(!bn_fits_width(&a, 64, 0) && bn_fits_width(&a, 8, 1) && !bn_fits_width(&a, 7, 1));
    bn_set_i64(&a, 0);
    TB_CHECK(bn_fits_width(&a, 0, 0) && bn_fits_width(&a, 1, 1));
    bn_set_u64(&a, 1);
    bn_shl(&a, &a, 64);
    TB_CHECK(bn_bitlen(&a) == 65 && tb_bn_eq_str(&a, "18446744073709551616"));

    /* set/get bit */
    bn_set_i64(&a, -1);
    bn_set_bit(&a, 100, 1);
    TB_CHECK(tb_bn_eq_i64(&a, -1));
    bn_set_bit(&a, 0, 0);
    TB_CHECK(tb_bn_eq_i64(&a, -2));
    bn_set_bit(&a, 64, 0);
    TB_CHECK(bn_get_bit(&a, 64) == 0 && bn_get_bit(&a, 65) == 1 && bn_get_bit(&a, 1000) == 1);
    bn_set_i64(&a, 0);
    bn_set_bit(&a, 64, 1);
    TB_CHECK(tb_bn_eq_str(&a, "18446744073709551616"));
    bn_set_bit(&a, 64, 0);
    TB_CHECK(bn_is_zero(&a));
    bn_set_i64(&a, -8); /* ...11111000 */
    TB_CHECK(bn_get_bit(&a, 0) == 0 && bn_get_bit(&a, 2) == 0 && bn_get_bit(&a, 3) == 1 && bn_get_bit(&a, 200) == 1);

    /* limb conversions */
    bn_set_i64(&a, -1);
    memset(w, 0xAA, sizeof w);
    bn_to_limbs(&a, w, 70);
    TB_CHECK(w[0] == UINT64_MAX && w[1] == 0x3F);
    bn_from_limbs(&b, w, 70, 1);
    TB_CHECK(tb_bn_eq_i64(&b, -1));
    bn_from_limbs(&b, w, 70, 0);
    TB_CHECK(tb_bn_eq_str(&b, "1180591620717411303423")); /* 2^70 - 1 */
    w[0] = 5;
    w[1] = UINT64_MAX; /* bits above width are ignored */
    bn_from_limbs(&b, w, 64, 1);
    TB_CHECK(tb_bn_eq_i64(&b, 5));
    bn_set_u64(&a, 5);
    bn_shl(&p, &a, 70);
    bn_add(&a, &a, &p); /* 5 * 2^70 + 5 */
    bn_to_limbs(&a, w, 64);
    TB_CHECK(w[0] == 5);
    w[0] = UINT64_C(0x8000000000000000);
    bn_from_limbs(&b, w, 64, 1);
    TB_CHECK(tb_bn_eq_i64(&b, INT64_MIN));
    w[0] = 1;
    bn_from_limbs(&b, w, 1, 1);
    TB_CHECK(tb_bn_eq_i64(&b, -1));
    bn_from_limbs(&b, w, 1, 0);
    TB_CHECK(tb_bn_eq_i64(&b, 1));
    bn_from_limbs(&b, w, 0, 0);
    TB_CHECK(bn_is_zero(&b));

    for (wi = 0; wi < TERN_ARRAY_LEN(widths); wi++) {
        uint32_t width = widths[wi];
        bn_set_i64(&m, 1);
        bn_shl(&m, &m, width);      /* 2^width */
        bn_shr(&p, &m, 1);          /* 2^(width-1) */
        for (i = 0; i < 200; i++) {
            tb_rand_big(&a, 10);
            bn_to_limbs(&a, w, width);
            /* unsigned: a mod 2^width */
            bn_from_limbs(&b, w, width, 0);
            bn_divmod_floor(NULL, &r, &a, &m);
            TB_CHECK(bn_cmp(&b, &r) == 0);
            TB_CHECK(bn_fits_width(&b, width, 0));
            /* signed: wrapped into [-2^(w-1), 2^(w-1)) */
            bn_from_limbs(&b, w, width, 1);
            if (bn_cmp(&r, &p) >= 0)
                bn_sub(&r, &r, &m);
            TB_CHECK(bn_cmp(&b, &r) == 0);
            TB_CHECK(bn_fits_width(&b, width, 1));
            /* bits above width cleared */
            if (width % 64)
                TB_CHECK((w[width / 64] >> (width % 64)) == 0);
        }
    }

    /* pow */
    bn_set_i64(&a, 3);
    bn_pow(&b, &a, 40);
    TB_CHECK(tb_bn_eq_str(&b, "12157665459056928801"));
    bn_set_i64(&a, -2);
    bn_pow(&b, &a, 63);
    TB_CHECK(tb_bn_eq_i64(&b, INT64_MIN));
    bn_set_i64(&a, 0);
    bn_pow(&b, &a, 0);
    TB_CHECK(tb_bn_eq_i64(&b, 1));
    bn_set_i64(&a, 2);
    bn_pow(&b, &a, 200);
    TB_CHECK(bn_bitlen(&b) == 201);
    /* hash: equal values hash equally */
    bn_set_i64(&a, 12345);
    bn_parse_cstr(&b, "12345", 10);
    TB_CHECK(bn_hash(&a) == bn_hash(&b));

    bn_free(&a);
    bn_free(&b);
    bn_free(&m);
    bn_free(&p);
    bn_free(&r);
}

/* ------------------------------------------------------------- the rest */

static void test_arena_vec(void)
{
    Arena a = {0};
    ArenaMark mk;
    typedef VEC(int) IntVec;
    IntVec v = {0};
    int i, ok = 1;
    char *s;
    for (i = 0; i < 1000; i++) {
        void *p = arena_alloc(&a, (size_t)(i % 37) + 1);
        if ((uintptr_t)p % ARENA_ALIGN)
            ok = 0;
    }
    TB_CHECK(ok);
    s = (char *)arena_zalloc(&a, 1u << 20); /* larger than a block */
    TB_CHECK(s[0] == 0 && s[(1u << 20) - 1] == 0);
    TB_STREQ(arena_strndup(&a, "hello world", 5), "hello");
    TB_STREQ(arena_printf(&a, "%d-%s", 42, "x"), "42-x");
    mk = arena_mark(&a);
    arena_alloc(&a, 100000);
    arena_rewind(&a, mk);
    TB_CHECK(a.total == mk.total);
    {
        int *xs = ANEWN(&a, int, 10);
        TB_CHECK(xs[9] == 0);
    }
    arena_release(&a);
    TB_CHECK(a.head == NULL);

    for (i = 0; i < 1000; i++)
        vec_push(v, i);
    TB_CHECK(v.len == 1000 && v.data[999] == 999 && vec_last(v) == 999);
    TB_CHECK(vec_pop(v) == 999 && v.len == 999);
    vec_remove(v, 0);
    TB_CHECK(v.data[0] == 1 && v.len == 998);
    vec_resize(v, 2000);
    TB_CHECK(v.len == 2000 && v.data[1999] == 0 && v.data[997] == 998);
    vec_resize(v, 3);
    TB_CHECK(v.len == 3);
    vec_clear(v);
    TB_CHECK(v.len == 0);
    vec_free(v);
    TB_CHECK(v.data == NULL && v.cap == 0);
}

static void test_str(void)
{
    StrBuf sb = {0};
    const char *a, *b;
    char big[300];
    int i;
    char *t;
    a = intern("hello", 5);
    b = intern_cstr("hello");
    TB_CHECK(a == b);
    TB_CHECK(intern("hello world", 5) == a);
    TB_CHECK(intern_cstr("hellp") != a);
    TB_CHECK(intern_len(a) == 5 && a[5] == '\0');
    TB_CHECK(intern("a\0b", 3) != intern("a\0c", 3) && intern_len(intern("a\0b", 3)) == 3);
    TB_CHECK(intern("", 0) == intern_cstr("") && intern_len(intern_cstr("")) == 0);
    for (i = 0; i < 20000; i++) {
        char k[32];
        snprintf(k, sizeof k, "sym%d", i);
        if (intern_cstr(k) != intern_cstr(k))
            break;
    }
    TB_CHECK(i == 20000);
    TB_CHECK(intern_cstr("hello") == a); /* stable across table growth */

    TB_STREQ(sb_cstr(&sb), "");
    sb_putc(&sb, 'x');
    sb_puts(&sb, "yz");
    sb_append(&sb, "123", 2);
    TB_STREQ(sb.data, "xyz12");
    memset(big, 'q', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    sb_printf(&sb, "[%s|%d]", big, -7);
    TB_CHECK(sb.len == 5 + 299 + 5 && strcmp(sb.data + sb.len - 4, "|-7]") == 0);
    sb_truncate(&sb, 3);
    TB_STREQ(sb.data, "xyz");
    sb_indent(&sb, 2);
    TB_STREQ(sb.data, "xyz  ");
    sb_reset(&sb);
    TB_CHECK(sb.len == 0 && sb.data[0] == '\0');
    sb_puts(&sb, "take");
    t = sb_take(&sb);
    TB_STREQ(t, "take");
    TB_CHECK(sb.data == NULL && sb.len == 0);
    free(t);
    sb_free(&sb);

    TB_CHECK(tern_is_digit('7') && !tern_is_digit('a') && tern_is_xdigit('F') && tern_hex_value('f') == 15);
    TB_CHECK(tern_is_space('\t') && !tern_is_space('x') && tern_to_lower('Q') == 'q' && tern_is_ident('_'));
    TB_CHECK(!tern_is_alpha(-1) && !tern_is_alpha(200));
    {
        uint64_t v;
        TB_CHECK(tern_parse_u64("1_000", &v) && v == 1000);
        TB_CHECK(tern_parse_u64("0x10", &v) && v == 16);
        TB_CHECK(tern_parse_u64("18446744073709551615", &v) && v == UINT64_MAX);
        TB_CHECK(!tern_parse_u64("18446744073709551616", &v));
        TB_CHECK(!tern_parse_u64("12x", &v) && !tern_parse_u64("", &v));
    }
}

static void test_map(void)
{
    Map m = {0};
    uint64_t v, k;
    size_t it, count;
    int i, ok = 1, ins;
    for (i = 0; i < 10000; i++)
        map_put(&m, (uint64_t)i * 7919u, (uint64_t)i);
    TB_CHECK(map_len(&m) == 10000);
    for (i = 0; i < 10000; i++) {
        if (!map_get(&m, (uint64_t)i * 7919u, &v) || v != (uint64_t)i)
            ok = 0;
    }
    TB_CHECK(ok);
    TB_CHECK(!map_has(&m, 1));
    for (i = 0; i < 10000; i += 2)
        TB_CHECK(map_del(&m, (uint64_t)i * 7919u));
    TB_CHECK(!map_del(&m, 0));
    TB_CHECK(map_len(&m) == 5000);
    ok = 1;
    for (i = 0; i < 10000; i++) {
        int has = map_has(&m, (uint64_t)i * 7919u);
        if (has != (i & 1))
            ok = 0;
    }
    TB_CHECK(ok);
    /* churn through tombstones */
    for (i = 0; i < 100000; i++) {
        map_put(&m, UINT64_C(1) << 40 | (uint64_t)i, 1);
        map_del(&m, UINT64_C(1) << 40 | (uint64_t)i);
    }
    TB_CHECK(map_len(&m) == 5000);
    count = 0;
    it = 0;
    while (map_next(&m, &it, &k, &v)) {
        if (k != v * 7919u)
            ok = 0;
        count++;
    }
    TB_CHECK(ok && count == 5000);
    *map_ref(&m, 42, &ins) = 9;
    TB_CHECK(ins == 1 && map_get(&m, 42, &v) && v == 9);
    *map_ref(&m, 42, &ins) += 1;
    TB_CHECK(ins == 0 && map_get(&m, 42, &v) && v == 10);
    map_put(&m, 0, 5); /* key 0 is an ordinary key */
    TB_CHECK(map_get(&m, 0, &v) && v == 5);
    map_clear(&m);
    TB_CHECK(map_len(&m) == 0 && !map_has(&m, 42));
    map_free(&m);
    TB_CHECK(hash_bytes("abc", 3) == hash_bytes("abc", 3) && hash_bytes("abc", 3) != hash_bytes("abd", 3));
}

static void test_json(void)
{
    Arena a = {0};
    JsonError err;
    JsonValue *v, *v2;
    StrBuf s1 = {0}, s2 = {0};
    const char *doc =
        "{ \"name\": \"Top\", \"n\": -42, \"pi\": 3.25, \"big\": 12345678901234567890,\n"
        "  \"min\": -9223372036854775808, \"ok\": true, \"no\": false, \"nil\": null,\n"
        "  \"arr\": [1, [2, []], {}], \"esc\": \"a\\\"b\\\\c\\n\\t\\u00e9\\ud83d\\ude00\\/\",\n"
        "  \"exp\": 1e3, \"dup\": 1, \"dup\": 2 }";
    v = json_parse(&a, doc, strlen(doc), &err);
    TB_CHECK(v != NULL && v->kind == JSON_OBJECT);
    TB_STREQ(json_as_str(json_get(v, "name"), NULL), "Top");
    TB_CHECK(json_as_int(json_get(v, "n"), 0) == -42);
    TB_CHECK(json_as_num(json_get(v, "pi"), 0) == 3.25);
    TB_CHECK(!json_get(v, "big")->is_int && json_get(v, "big")->number > 1.2e19);
    TB_CHECK(json_get(v, "min")->is_int && json_get(v, "min")->integer == INT64_MIN);
    TB_CHECK(json_as_bool(json_get(v, "ok"), 0) == 1 && json_as_bool(json_get(v, "no"), 1) == 0);
    TB_CHECK(json_is(json_get(v, "nil"), JSON_NULL));
    TB_CHECK(json_len(json_get(v, "arr")) == 3);
    TB_CHECK(json_as_int(json_at(json_at(json_get(v, "arr"), 1), 0), 0) == 2);
    TB_CHECK(json_len(json_at(json_at(json_get(v, "arr"), 1), 1)) == 0);
    TB_STREQ(json_as_str(json_get(v, "esc"), NULL), "a\"b\\c\n\t\xc3\xa9\xf0\x9f\x98\x80/");
    TB_CHECK(json_as_int(json_get(v, "exp"), 0) == 1000 && !json_get(v, "exp")->is_int);
    TB_CHECK(json_as_int(json_get(v, "dup"), 0) == 2);
    TB_CHECK(json_get(v, "missing") == NULL && json_as_str(NULL, "d")[0] == 'd');

    /* round trip: compact and pretty serializations are stable */
    json_to_sb(&s1, v, 0);
    v2 = json_parse(&a, s1.data, s1.len, &err);
    TB_CHECK(v2 != NULL);
    json_to_sb(&s2, v2, 0);
    TB_STREQ(s1.data, s2.data);
    sb_reset(&s1);
    sb_reset(&s2);
    json_to_sb(&s1, v, 1);
    v2 = json_parse(&a, s1.data, s1.len, &err);
    TB_CHECK(v2 != NULL);
    json_to_sb(&s2, v2, 1);
    TB_STREQ(s1.data, s2.data);

    /* writer formatting */
    {
        JsonWriter w;
        StrBuf o = {0};
        jw_init(&w, &o, 1);
        jw_object_begin(&w);
        jw_kv_str(&w, "a", "x\x01y");
        jw_key(&w, "b");
        jw_array_begin(&w);
        jw_int(&w, 1);
        jw_int(&w, INT64_MIN);
        jw_array_end(&w);
        jw_key(&w, "c");
        jw_object_begin(&w);
        jw_object_end(&w);
        jw_kv_num(&w, "d", 0.1);
        jw_kv_str(&w, "e", NULL);
        jw_object_end(&w);
        TB_STREQ(o.data, "{\n  \"a\": \"x\\u0001y\",\n  \"b\": [\n    1,\n    -9223372036854775808\n  ],\n"
                         "  \"c\": {},\n  \"d\": 0.1,\n  \"e\": null\n}");
        sb_reset(&o);
        jw_init(&w, &o, 0);
        jw_array_begin(&w);
        jw_str(&w, "\xff");
        jw_bool(&w, 1);
        jw_uint(&w, UINT64_MAX);
        jw_array_end(&w);
        TB_STREQ(o.data, "[\"\\ufffd\",true,18446744073709551615]");
        sb_free(&o);
    }

    /* errors */
    TB_CHECK(json_parse(&a, "[1,]", 4, &err) == NULL && err.line == 1 && err.col == 4);
    TB_CHECK(json_parse(&a, "{\"a\" 1}", 7, &err) == NULL);
    TB_CHECK(json_parse(&a, "\n\n  tru", 7, &err) == NULL && err.line == 3);
    TB_CHECK(json_parse(&a, "01", 2, &err) == NULL);
    TB_CHECK(json_parse(&a, "\"abc", 4, &err) == NULL);
    TB_CHECK(json_parse(&a, "1 2", 3, &err) == NULL);
    TB_CHECK(json_parse(&a, "", 0, &err) == NULL);
    {
        StrBuf deep = {0};
        int i;
        for (i = 0; i < 600; i++)
            sb_putc(&deep, '[');
        for (i = 0; i < 600; i++)
            sb_putc(&deep, ']');
        TB_CHECK(json_parse(&a, deep.data, deep.len, &err) == NULL);
        sb_free(&deep);
    }
    sb_free(&s1);
    sb_free(&s2);
    arena_release(&a);
}

static void test_utf8(void)
{
    size_t adv, off;
    char buf[4];
    uint32_t cp;
    int ok = 1;
    TB_CHECK(utf8_decode("A", 1, &adv) == 'A' && adv == 1);
    TB_CHECK(utf8_decode("\xc3\xa9", 2, &adv) == 0xE9 && adv == 2);
    TB_CHECK(utf8_decode("\xe2\x82\xac", 3, &adv) == 0x20AC && adv == 3);
    TB_CHECK(utf8_decode("\xf0\x9f\x98\x80", 4, &adv) == 0x1F600 && adv == 4);
    TB_CHECK(utf8_decode("\xc0\x80", 2, &adv) == UTF8_INVALID && adv == 1);    /* overlong */
    TB_CHECK(utf8_decode("\xe0\x80\xaf", 3, &adv) == UTF8_INVALID);            /* overlong */
    TB_CHECK(utf8_decode("\xed\xa0\x80", 3, &adv) == UTF8_INVALID);            /* surrogate */
    TB_CHECK(utf8_decode("\xf4\x90\x80\x80", 4, &adv) == UTF8_INVALID);        /* > 10FFFF */
    TB_CHECK(utf8_decode("\xe2\x82", 2, &adv) == UTF8_INVALID);                /* truncated */
    TB_CHECK(utf8_decode("\x80", 1, &adv) == UTF8_INVALID && adv == 1);
    for (cp = 0; cp <= 0x10FFFF; cp += (cp < 0x1000 ? 1 : 97)) {
        int n = utf8_encode(cp, buf);
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            if (n != 0)
                ok = 0;
            continue;
        }
        if (n == 0 || utf8_decode(buf, (size_t)n, &adv) != cp || adv != (size_t)n)
            ok = 0;
    }
    TB_CHECK(ok);
    TB_CHECK(utf8_encode(0x110000, buf) == 0);
    TB_CHECK(utf8_valid("h\xc3\xa9llo", 6, NULL));
    TB_CHECK(!utf8_valid("ab\xff", 3, &off) && off == 2);
    TB_CHECK(utf8_count("h\xc3\xa9\xf0\x9f\x98\x80", 7) == 3);
    TB_CHECK(utf8_utf16_len("h\xc3\xa9\xf0\x9f\x98\x80", 7) == 4);
    TB_CHECK(utf8_offset_of_utf16("h\xc3\xa9\xf0\x9f\x98\x80x", 8, 2) == 3);
    TB_CHECK(utf8_offset_of_utf16("h\xc3\xa9\xf0\x9f\x98\x80x", 8, 4) == 7);
    TB_CHECK(utf8_offset_of_utf16("h\xc3\xa9\xf0\x9f\x98\x80x", 8, 3) == 3);
}

typedef struct TbSinkData {
    int count;
    StrBuf text;
} TbSinkData;

static void tb_sink(const Diagnostic *d, void *user)
{
    TbSinkData *s = (TbSinkData *)user;
    s->count++;
    diag_render(d, DIAG_FMT_SHORT, 0, &s->text);
}

static void test_diag(void)
{
    const char *src = "module Top {\n\tlet x = a + b\n}\n";
    uint32_t id = src_add("top.tern", src, strlen(src));
    const SourceFile *f = src_get(id);
    Span sp = span_make(id, 22, 5); /* "a + b" */
    Diagnostic d;
    DiagSub sub;
    StrBuf out = {0};
    TbSinkData sink;
    size_t len;
    const char *line;

    TB_CHECK(id >= 1 && f && f->nlines == 4 && src_find("top.tern") == id && src_get(0) == NULL);
    TB_CHECK(sp.line == 2 && sp.col == 10);
    line = src_line(f, 2, &len);
    TB_CHECK(line && len == 14 && strncmp(line, "\tlet x", 6) == 0);
    TB_CHECK(span_join(span_make(id, 0, 6), sp).len == 27);

    memset(&d, 0, sizeof d);
    d.level = DIAG_ERROR;
    d.span = sp;
    d.msg = (char *)"width mismatch: expected uint<8>, found uint<9>";
    d.path = (char *)"Top > rx_fifo: Fifo<u8, 64>";
    sub.level = DIAG_HELP;
    sub.span = span_none();
    sub.msg = (char *)"truncate explicitly with `(a + b).trunc<8>()`";
    d.nsubs = 1;
    d.subs = &sub;
    diag_render(&d, DIAG_FMT_SHORT, 0, &out);
    TB_STREQ(out.data, "top.tern:2:10: error: width mismatch: expected uint<8>, found uint<9>\n");
    sb_reset(&out);
    diag_render(&d, DIAG_FMT_HUMAN, 0, &out);
    TB_STREQ(out.data,
             "top.tern:2:10: error: width mismatch: expected uint<8>, found uint<9>\n"
             "  |\n"
             "2 |     let x = a + b\n"
             "  |             ^^^^^\n"
             "  = help: truncate explicitly with `(a + b).trunc<8>()`\n"
             "  in: Top > rx_fifo: Fifo<u8, 64>\n"
             "\n");
    sb_reset(&out);
    diag_render(&d, DIAG_FMT_JSON, 0, &out);
    TB_STREQ(out.data,
             "{\"severity\":\"error\",\"message\":\"width mismatch: expected uint<8>, found uint<9>\","
             "\"file\":\"top.tern\",\"line\":2,\"col\":10,\"offset\":22,\"length\":5,\"end_line\":2,"
             "\"end_col\":15,\"path\":\"Top > rx_fifo: Fifo<u8, 64>\",\"notes\":[{\"severity\":\"help\","
             "\"message\":\"truncate explicitly with `(a + b).trunc<8>()`\",\"file\":null}]}\n");
    sb_reset(&out);
    diag_render(&d, DIAG_FMT_HUMAN, 1, &out);
    TB_CHECK(strstr(out.data, "\x1b[1;31merror") != NULL);

    /* the global API with a sink */
    memset(&sink, 0, sizeof sink);
    diag_reset();
    diag_set_sink(tb_sink, &sink);
    diag_push_path("Top");
    diag_error(sp, "bad %d", 1);
    diag_note(span_make(id, 0, 6), "declared here");
    diag_help("fix it");
    diag_pop_path();
    diag_warning(span_none(), "unused");
    diag_set_strict(1);
    diag_warning(sp, "strict warn");
    diag_set_strict(0);
    diag_flush();
    TB_CHECK(sink.count == 3);
    TB_CHECK(diag_error_count() == 2 && diag_warning_count() == 1);
    TB_STREQ(sink.text.data, "top.tern:2:10: error: bad 1\nwarning: unused\ntop.tern:2:10: error: strict warn\n");
    diag_set_sink(NULL, NULL);
    diag_reset();
    TB_CHECK(diag_error_count() == 0 && diag_path_depth() == 0);
    sb_free(&sink.text);
    sb_free(&out);
}

static void test_args(void)
{
    char *argv[] = {(char *)"test", (char *)"--waves", (char *)"--seed", (char *)"7", (char *)"filt",
                    (char *)"--backend=verilator", (char *)"-j4", (char *)"--", (char *)"--literal", NULL};
    ArgParser ap;
    int waves = 0, npos = 0;
    uint64_t seed = 0, jobs = 0;
    const char *backend = NULL, *pos[4];
    args_init(&ap, 9, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "waves|w"))
            waves = 1;
        else if (args_opt_u64(&ap, "seed", &seed))
            ;
        else if (args_opt(&ap, "backend", &backend))
            ;
        else if (args_opt_u64(&ap, "jobs|j", &jobs))
            ;
        else if (args_positional(&ap))
            pos[npos++] = ap.arg;
        else
            args_unknown(&ap);
    }
    TB_CHECK(ap.errors == 0 && waves && seed == 7 && jobs == 4);
    TB_STREQ(backend, "verilator");
    TB_CHECK(npos == 2);
    TB_STREQ(pos[0], "filt");
    TB_STREQ(pos[1], "--literal");
    TB_CHECK(args_wants_help(2, (char *[]){(char *)"x", (char *)"-h", NULL}));
}

static void test_os(void)
{
    char *p, *tmp, *dir, *file, *data;
    size_t len;
    OsDirList list;
    OsStat st;

    p = os_path_normalize("a/./b/../c//d/");
    TB_STREQ(p, "a/c/d");
    free(p);
    p = os_path_normalize("../../x/..");
    TB_STREQ(p, "../..");
    free(p);
    p = os_path_normalize("/../a/b/../../..");
    TB_STREQ(p, "/");
    free(p);
    p = os_path_normalize("");
    TB_STREQ(p, ".");
    free(p);
    p = os_path_normalize("./");
    TB_STREQ(p, ".");
    free(p);
    p = os_path_join("a/b", "c.tern");
    TB_STREQ(p, "a/b/c.tern");
    free(p);
    p = os_path_join("a/", "/abs");
    TB_STREQ(p, "/abs");
    free(p);
    p = os_path_dirname("a/b/c.tern");
    TB_STREQ(p, "a/b");
    free(p);
    p = os_path_dirname("c.tern");
    TB_STREQ(p, ".");
    free(p);
    p = os_path_dirname("/c");
    TB_STREQ(p, "/");
    free(p);
    TB_STREQ(os_path_basename("a/b/c.tern"), "c.tern");
    TB_STREQ(os_path_ext("a/b.x/c.tern"), ".tern");
    TB_STREQ(os_path_ext("a/b.x/.hidden"), "");
    TB_STREQ(os_path_ext("noext"), "");
    p = os_path_relative("/a/b/c", "/a");
    TB_STREQ(p, "b/c");
    free(p);
    p = os_path_relative("/a/bc", "/a/b");
    TB_STREQ(p, "/a/bc");
    free(p);

    tmp = os_temp_dir();
    TB_CHECK(tmp != NULL && os_is_dir(tmp));
    {
        char name[64];
        snprintf(name, sizeof name, "tern-test-%lu", (unsigned long)(os_time_ns() % 1000000007u));
        dir = os_path_join(tmp, name);
    }
    p = os_path_join(dir, "x/y/z");
    TB_CHECK(os_mkdir_p(p) == 0 && os_is_dir(p) && os_mkdir_p(p) == 0);
    file = os_path_join(p, "f.txt");
    TB_CHECK(os_write_file(file, "hello\n", 6) == 0);
    TB_CHECK(os_read_file(file, &data, &len) == 0 && len == 6 && strcmp(data, "hello\n") == 0);
    free(data);
    TB_CHECK(os_write_file_if_changed(file, "hello\n", 6) == 0);
    TB_CHECK(os_write_file_if_changed(file, "hello!\n", 7) == 1);
    TB_CHECK(os_stat(file, &st) == 0 && st.exists && st.is_file && !st.is_dir && st.size == 7 && st.mtime_ns > 0);
    TB_CHECK(os_stat(p, &st) == 0 && st.is_dir);
    TB_CHECK(os_read_file(p, &data, &len) != 0);
    {
        char *f2 = os_path_join(p, "a.txt");
        TB_CHECK(os_copy_file(file, f2) == 0);
        TB_CHECK(os_list_dir(p, &list) == 0 && list.len == 2);
        TB_STREQ(list.items[0].name, "a.txt");
        TB_STREQ(list.items[1].name, "f.txt");
        os_dir_list_free(&list);
        free(f2);
    }
    TB_CHECK(os_remove_all(dir) == 0 && !os_exists(dir));
    TB_CHECK(os_remove_all(dir) == 0);
    TB_CHECK(os_stat(dir, &st) != 0 && !st.exists);
    free(file);
    free(p);
    free(dir);
    free(tmp);

    p = os_getcwd();
    TB_CHECK(p && os_path_is_abs(p));
    free(p);
    p = os_exe_path();
    TB_CHECK(p && os_is_file(p));
    free(p);
    TB_CHECK(os_setenv("TERN_TEST_VAR", "v1") == 0);
    p = os_getenv("TERN_TEST_VAR");
    TB_STREQ(p, "v1");
    free(p);
    TB_CHECK(os_setenv("TERN_TEST_VAR", NULL) == 0 && os_getenv("TERN_TEST_VAR") == NULL);
    TB_CHECK(os_cpu_count() >= 1);
    {
        uint64_t t0 = os_time_ns();
        os_sleep_ms(5);
        TB_CHECK(os_time_ns() - t0 >= 4000000u);
    }
    TB_CHECK(os_unix_time() > 1600000000);
}

static void test_processes(void)
{
    char *self = os_exe_path();
    char *cat_argv[3], *echo_argv[3];
#ifdef _WIN32
    char *shell_argv[] = {(char *)"cmd", (char *)"/c", (char *)"echo hi& exit /b 4", NULL};
#else
    char *shell_argv[] = {(char *)"sh", (char *)"-c", (char *)"echo hi; exit 4", NULL};
#endif
    char *bad_argv[] = {(char *)"tern-no-such-program-xyz", NULL};
    OsSpawnOpts o;
    OsRunResult res;
    OsProc *procs[4];
    char *err = NULL;
    int i, done = 0, rc;
    size_t n;

    /* children: this test binary in --cat / --echo mode (see main) */
    cat_argv[0] = echo_argv[0] = self;
    cat_argv[1] = (char *)"--cat";
    echo_argv[1] = (char *)"--echo";
    cat_argv[2] = echo_argv[2] = NULL;

    memset(&o, 0, sizeof o);
    o.capture_stdout = 1;
    o.capture_stderr = 1;
    rc = os_run(shell_argv, &o, &res); /* PATH lookup of a system program */
    TB_CHECK(rc == 4 && res.out && strncmp(res.out, "hi", 2) == 0);
    os_run_result_free(&res);

    rc = os_run(echo_argv, &o, &res);
    TB_CHECK(rc == 3);
    TB_CHECK(res.out && strncmp(res.out, "hi", 2) == 0);
    TB_CHECK(res.err && strncmp(res.err, "err", 3) == 0);
    os_run_result_free(&res);

    o.merge_stderr = 1;
    rc = os_run(echo_argv, &o, &res);
    TB_CHECK(rc == 3 && res.out && strstr(res.out, "hi") && strstr(res.out, "err"));
    os_run_result_free(&res);
    o.merge_stderr = 0;

    TB_CHECK(os_spawn(bad_argv, &o, &err) == NULL && err != NULL);
    free(err);

    /* large stdin and stdout at the same time: needs pipe pumping */
    {
        size_t big = 2u * 1024u * 1024u, k;
        char *in = (char *)xmalloc(big);
        for (k = 0; k < big; k++)
            in[k] = (k % 64 == 63) ? '\n' : (char)('a' + (char)(k % 26));
        o.stdin_data = in;
        o.stdin_len = big;
        rc = os_run(cat_argv, &o, &res);
        TB_CHECK(rc == 0);
        TB_CHECK(res.out_len == big && memcmp(res.out, in, big) == 0);
        os_run_result_free(&res);
        free(in);
        o.stdin_data = NULL;
        o.stdin_len = 0;
    }

    /* several children at once */
    for (i = 0; i < 4; i++) {
        procs[i] = os_spawn(echo_argv, &o, NULL);
        TB_CHECK(procs[i] != NULL);
    }
    while (done < 4) {
        int k = os_proc_wait_any(procs, 4, -1);
        if (k < 0)
            break;
        TB_CHECK(os_proc_done(procs[k]) && os_proc_exit_code(procs[k]) == 3);
        TB_CHECK(strncmp(os_proc_stdout(procs[k], &n), "hi", 2) == 0);
        os_proc_free(procs[k]);
        procs[k] = NULL;
        done++;
    }
    TB_CHECK(done == 4);
    TB_CHECK(os_proc_wait_any(procs, 4, 10) == -1);

    /* interactive child: request / response lines over pipes */
    {
        OsSpawnOpts io;
        OsProc *p;
        char *line, *lines_argv[3];
        lines_argv[0] = self;
        lines_argv[1] = (char *)"--lines";
        lines_argv[2] = NULL;
        memset(&io, 0, sizeof io);
        io.interactive = 1;
        io.merge_stderr = 1;
        p = os_spawn(lines_argv, &io, NULL);
        TB_CHECK(p != NULL);
        if (p) {
            TB_CHECK(os_proc_write(p, "ping\n", 5) == 0);
            line = os_proc_read_line(p, 5000);
            TB_CHECK(line && strcmp(line, "@ping") == 0);
            free(line);
            TB_CHECK(os_proc_write(p, "a b\nc\n", 6) == 0);
            line = os_proc_read_line(p, 5000);
            TB_CHECK(line && strcmp(line, "@a b") == 0);
            free(line);
            line = os_proc_read_line(p, 5000);
            TB_CHECK(line && strcmp(line, "@c") == 0);
            free(line);
            TB_CHECK(os_proc_read_line(p, 20) == NULL); /* nothing pending: timeout */
            os_proc_close_stdin(p);                     /* the child sees end of input */
            line = os_proc_read_line(p, 5000);
            TB_CHECK(line && strcmp(line, "@eof") == 0);
            free(line);
            TB_CHECK(os_proc_read_line(p, 5000) == NULL);
            TB_CHECK(os_proc_wait(p) == 0 && os_proc_exit_code(p) == 0);
            os_proc_free(p);
        }
    }

    /* cwd option */
    {
        char *tmp = os_temp_dir();
#ifdef _WIN32
        char *pwd_argv[] = {(char *)"cmd", (char *)"/c", (char *)"cd", NULL};
#else
        char *pwd_argv[] = {(char *)"pwd", NULL};
#endif
        o.cwd = tmp;
        rc = os_run(pwd_argv, &o, &res);
        TB_CHECK(rc == 0 && res.out_len > 0);
        os_run_result_free(&res);
        free(tmp);
        o.cwd = NULL;
    }
    free(self);
}

/* Child-process modes used by test_processes. */
static int tb_child_mode(const char *mode)
{
    if (strcmp(mode, "--lines") == 0) {
        char buf[1024];
        while (fgets(buf, sizeof buf, stdin)) {
            size_t n = strlen(buf);
            while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
                buf[--n] = 0;
            printf("@%s\n", buf);
            fflush(stdout);
        }
        printf("@eof\n");
        return 0;
    }
    if (strcmp(mode, "--cat") == 0) {
        char buf[65536];
        size_t n;
        os_set_binary_stdio();
        while ((n = fread(buf, 1, sizeof buf, stdin)) > 0)
            fwrite(buf, 1, n, stdout);
        return 0;
    }
    if (strcmp(mode, "--echo") == 0) {
        printf("hi\n");
        fprintf(stderr, "err\n");
        return 3;
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return tb_child_mode(argv[1]);
    test_bignum_parse_format();
    test_bignum_conversions();
    test_bignum_small_arith();
    test_bignum_big_identities();
    test_bignum_knuth_vectors();
    test_bignum_bits_widths();
    test_arena_vec();
    test_str();
    test_map();
    test_json();
    test_utf8();
    test_diag();
    test_args();
    test_os();
    test_processes();
    src_free_all();
    intern_free_all();
    if (tb_fails) {
        printf("unit tests: %d of %d checks FAILED\n", tb_fails, tb_checks);
        return 1;
    }
    printf("unit tests: all %d checks passed\n", tb_checks);
    return 0;
}
