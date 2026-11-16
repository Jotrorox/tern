/* utf8.c — UTF-8 helpers. */
#include "utf8.h"

uint32_t utf8_decode(const char *s, size_t len, size_t *adv)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t cp, min;
    size_t n, i;
    if (len == 0) {
        *adv = 0;
        return UTF8_INVALID;
    }
    *adv = 1;
    if (p[0] < 0x80)
        return p[0];
    if ((p[0] & 0xE0) == 0xC0) {
        n = 2;
        cp = p[0] & 0x1Fu;
        min = 0x80;
    } else if ((p[0] & 0xF0) == 0xE0) {
        n = 3;
        cp = p[0] & 0x0Fu;
        min = 0x800;
    } else if ((p[0] & 0xF8) == 0xF0) {
        n = 4;
        cp = p[0] & 0x07u;
        min = 0x10000;
    } else {
        return UTF8_INVALID;
    }
    if (len < n)
        return UTF8_INVALID;
    for (i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80)
            return UTF8_INVALID;
        cp = (cp << 6) | (p[i] & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return UTF8_INVALID;
    *adv = n;
    return cp;
}

int utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0;
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

int utf8_valid(const char *s, size_t len, size_t *bad_off)
{
    size_t i = 0, adv;
    while (i < len) {
        if ((unsigned char)s[i] < 0x80) {
            i++;
            continue;
        }
        if (utf8_decode(s + i, len - i, &adv) == UTF8_INVALID) {
            if (bad_off)
                *bad_off = i;
            return 0;
        }
        i += adv;
    }
    return 1;
}

size_t utf8_count(const char *s, size_t len)
{
    size_t i = 0, n = 0, adv;
    while (i < len) {
        utf8_decode(s + i, len - i, &adv);
        i += adv;
        n++;
    }
    return n;
}

size_t utf8_utf16_len(const char *s, size_t len)
{
    size_t i = 0, n = 0, adv;
    while (i < len) {
        uint32_t cp = utf8_decode(s + i, len - i, &adv);
        i += adv;
        n += (cp != UTF8_INVALID && cp >= 0x10000) ? 2 : 1;
    }
    return n;
}

size_t utf8_offset_of_utf16(const char *s, size_t len, size_t units)
{
    size_t i = 0, n = 0, adv;
    while (i < len && n < units) {
        uint32_t cp = utf8_decode(s + i, len - i, &adv);
        n += (cp != UTF8_INVALID && cp >= 0x10000) ? 2 : 1;
        if (n > units)
            break; /* inside a surrogate pair: stay before it */
        i += adv;
    }
    return i;
}
