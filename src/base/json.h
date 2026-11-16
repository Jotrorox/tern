/* json.h — JSON DOM reader (arena-allocated) and streaming writer.
 *
 * Reading:
 *     JsonError err;
 *     JsonValue *v = json_parse(&arena, text, len, &err);
 *     if (!v) report(err.line, err.col, err.msg);
 *     const char *m = json_as_str(json_get(v, "method"), "");
 *
 * Writing (commas and indentation are handled automatically):
 *     JsonWriter w;  jw_init(&w, &sb, 1);
 *     jw_object_begin(&w);
 *       jw_kv_str(&w, "name", "Top");
 *       jw_key(&w, "ports"); jw_array_begin(&w); jw_int(&w, 3); jw_array_end(&w);
 *     jw_object_end(&w);
 */
#ifndef TERN_JSON_H
#define TERN_JSON_H

#include "common.h"
#include "arena.h"
#include "str.h"

typedef enum JsonKind {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonKind;

typedef struct JsonValue JsonValue;

typedef struct JsonMember {
    const char *key;     /* NUL-terminated (may contain embedded NULs: see key_len) */
    size_t key_len;
    JsonValue *value;
} JsonMember;

struct JsonValue {
    JsonKind kind;
    int boolean;         /* BOOL */
    int is_int;          /* NUMBER: literal was an integer that fits in int64 */
    int64_t integer;     /* NUMBER: valid when is_int */
    double number;       /* NUMBER: always set */
    const char *str;     /* STRING: decoded UTF-8, NUL-terminated */
    size_t len;          /* STRING: byte length */
    size_t n;            /* ARRAY: item count; OBJECT: member count */
    JsonValue **items;   /* ARRAY */
    JsonMember *members; /* OBJECT, in document order */
};

typedef struct JsonError {
    size_t offset;       /* byte offset of the error */
    uint32_t line, col;  /* 1-based */
    char msg[96];
} JsonError;

/* Parse exactly one JSON value (surrounding whitespace allowed). Returns
 * NULL on error and fills *err (if non-NULL). Nesting depth is limited to
 * JSON_MAX_DEPTH. */
#define JSON_MAX_DEPTH 512
JsonValue *json_parse(Arena *a, const char *text, size_t len, JsonError *err);

/* Accessors: all accept NULL and return the default for a missing value
 * or a value of the wrong kind. */
JsonValue *json_get(const JsonValue *obj, const char *key);   /* last member wins */
JsonValue *json_at(const JsonValue *arr, size_t i);
size_t json_len(const JsonValue *v);                          /* items/members, else 0 */
int json_is(const JsonValue *v, JsonKind kind);
const char *json_as_str(const JsonValue *v, const char *dflt);
int64_t json_as_int(const JsonValue *v, int64_t dflt);
double json_as_num(const JsonValue *v, double dflt);
int json_as_bool(const JsonValue *v, int dflt);

/* Constructors (for building documents to write back out). */
JsonValue *json_new(Arena *a, JsonKind kind);
JsonValue *json_new_str(Arena *a, const char *s, size_t len);
JsonValue *json_new_int(Arena *a, int64_t v);

/* ------------------------------------------------------------- writer --- */

typedef struct JsonWriter {
    StrBuf *out;
    size_t start;   /* out->len when the writer was initialized */
    int depth;
    int pretty;     /* 2-space indentation and newlines */
} JsonWriter;

void jw_init(JsonWriter *w, StrBuf *out, int pretty);
void jw_object_begin(JsonWriter *w);
void jw_object_end(JsonWriter *w);
void jw_array_begin(JsonWriter *w);
void jw_array_end(JsonWriter *w);
void jw_key(JsonWriter *w, const char *key);
void jw_str(JsonWriter *w, const char *s);            /* NULL writes null */
void jw_strn(JsonWriter *w, const char *s, size_t n);
void jw_int(JsonWriter *w, int64_t v);
void jw_uint(JsonWriter *w, uint64_t v);
void jw_num(JsonWriter *w, double v);                 /* NaN/Inf write null */
void jw_bool(JsonWriter *w, int v);
void jw_null(JsonWriter *w);
void jw_raw(JsonWriter *w, const char *json);         /* pre-encoded value */
void jw_value(JsonWriter *w, const JsonValue *v);     /* write a DOM value */

void jw_kv_str(JsonWriter *w, const char *key, const char *s);
void jw_kv_int(JsonWriter *w, const char *key, int64_t v);
void jw_kv_uint(JsonWriter *w, const char *key, uint64_t v);
void jw_kv_bool(JsonWriter *w, const char *key, int v);
void jw_kv_num(JsonWriter *w, const char *key, double v);

/* Append s[0..n) as a quoted JSON string literal (invalid UTF-8 becomes
 * U+FFFD). */
void json_escape(StrBuf *out, const char *s, size_t n);

/* Serialize a DOM value. */
void json_to_sb(StrBuf *out, const JsonValue *v, int pretty);

#endif /* TERN_JSON_H */
