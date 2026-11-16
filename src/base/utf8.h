/* utf8.h — UTF-8 decoding, encoding and validation (RFC 3629: no overlong
 * forms, no surrogates, nothing above U+10FFFF). */
#ifndef TERN_UTF8_H
#define TERN_UTF8_H

#include "common.h"

#define UTF8_INVALID 0xFFFFFFFFu

/* Decode one code point from s[0..len). Sets *adv to the number of bytes
 * consumed (always >= 1 when len > 0, so callers can skip bad bytes) and
 * returns the code point, or UTF8_INVALID for a malformed sequence. */
uint32_t utf8_decode(const char *s, size_t len, size_t *adv);

/* Encode cp into out (up to 4 bytes); returns the byte count, 0 if cp is
 * not a valid scalar value. */
int utf8_encode(uint32_t cp, char out[4]);

/* 1 if s[0..len) is valid UTF-8; otherwise 0 and *bad_off (if non-NULL)
 * receives the offset of the first malformed byte. */
int utf8_valid(const char *s, size_t len, size_t *bad_off);

/* Number of code points in s[0..len) (malformed bytes count as one each). */
size_t utf8_count(const char *s, size_t len);

/* Number of UTF-16 code units in s[0..len) (LSP positions). */
size_t utf8_utf16_len(const char *s, size_t len);

/* Byte offset within s[0..len) of UTF-16 unit offset `units` (clamped). */
size_t utf8_offset_of_utf16(const char *s, size_t len, size_t units);

#endif /* TERN_UTF8_H */
