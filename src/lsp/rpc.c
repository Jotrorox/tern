/* rpc.c — Content-Length framed JSON-RPC 2.0 transport (see lsp.h). */
#include "lsp.h"
#include "../os/os.h"

#define RPC_MAX_HEADER 65536u
#define RPC_MAX_BODY (512u * 1024u * 1024u)

/* ------------------------------------------------------------ stdio io --- */

static long rpc_stdio_read(void *user, char *buf, size_t n)
{
    TERN_UNUSED(user);
    return os_read_stdin(buf, n);
}

static int rpc_stdio_wait(void *user, int timeout_ms)
{
    TERN_UNUSED(user);
    return os_stdin_wait(timeout_ms) != 0;
}

static void rpc_stdio_write(void *user, const char *data, size_t n)
{
    TERN_UNUSED(user);
    fwrite(data, 1, n, stdout);
    fflush(stdout);
}

LspIo lsp_stdio_io(void)
{
    LspIo io;
    os_set_binary_stdio();
    io.read = rpc_stdio_read;
    io.wait = rpc_stdio_wait;
    io.write = rpc_stdio_write;
    io.user = NULL;
    return io;
}

/* -------------------------------------------------------------- reading --- */

void rpc_reader_init(RpcReader *r, const LspIo *io)
{
    memset(r, 0, sizeof *r);
    r->io = io;
}

void rpc_reader_free(RpcReader *r)
{
    free(r->buf);
    memset(r, 0, sizeof *r);
}

/* Reads more input; 0 at the end of input. */
static int rpc_fill(RpcReader *r)
{
    long n;
    if (r->eof)
        return 0;
    if (r->pos > 0 && r->pos == r->len) {
        r->pos = r->len = 0;
    } else if (r->pos > 0 && r->pos >= r->cap / 2) {
        memmove(r->buf, r->buf + r->pos, r->len - r->pos);
        r->len -= r->pos;
        r->pos = 0;
    }
    if (r->cap - r->len < 4096) {
        r->cap = r->cap ? r->cap * 2 : 16384;
        r->buf = (char *)xrealloc(r->buf, r->cap);
    }
    n = r->io->read(r->io->user, r->buf + r->len, r->cap - r->len);
    if (n <= 0) {
        r->eof = 1;
        return 0;
    }
    r->len += (size_t)n;
    return 1;
}

/* End of the header block in buf[pos..len): offset just past the blank
 * line, or 0 when it is not complete yet. */
static size_t rpc_header_end(const RpcReader *r)
{
    size_t i;
    for (i = r->pos; i < r->len; i++) {
        if (r->buf[i] != '\n')
            continue;
        if (i + 1 < r->len && r->buf[i + 1] == '\n')
            return i + 2;
        if (i + 2 < r->len && r->buf[i + 1] == '\r' && r->buf[i + 2] == '\n')
            return i + 3;
    }
    return 0;
}

/* Content-Length of the header block buf[from..to), or -1. */
static long rpc_content_length(const char *p, size_t n)
{
    static const char key[] = "content-length";
    size_t i = 0;
    while (i < n) {
        size_t ls = i, le, k;
        while (i < n && p[i] != '\n')
            i++;
        le = i;
        i++;
        if (le - ls > sizeof key - 1 && p[ls + sizeof key - 1] == ':') {
            for (k = 0; k < sizeof key - 1; k++)
                if (tern_to_lower((unsigned char)p[ls + k]) != key[k])
                    break;
            if (k == sizeof key - 1) {
                size_t j = ls + sizeof key;
                unsigned long v = 0;
                int digits = 0;
                while (j < le && (p[j] == ' ' || p[j] == '\t'))
                    j++;
                while (j < le && tern_is_digit((unsigned char)p[j]) && v <= RPC_MAX_BODY) {
                    v = v * 10 + (unsigned long)(p[j] - '0');
                    digits++;
                    j++;
                }
                if (!digits || v > RPC_MAX_BODY)
                    return -1;
                return (long)v;
            }
        }
    }
    return -1;
}

int rpc_read(RpcReader *r, StrBuf *body)
{
    sb_reset(body);
    for (;;) {
        size_t he;
        long clen;
        /* blank lines between messages are tolerated */
        while (r->pos < r->len && (r->buf[r->pos] == '\r' || r->buf[r->pos] == '\n'))
            r->pos++;
        he = rpc_header_end(r);
        if (!he) {
            if (r->len - r->pos > RPC_MAX_HEADER) {
                r->pos = r->len; /* garbage: drop it */
                return -1;
            }
            if (!rpc_fill(r))
                return 0;
            continue;
        }
        clen = rpc_content_length(r->buf + r->pos, he - r->pos);
        r->pos = he;
        if (clen < 0)
            return -1;
        while (r->len - r->pos < (size_t)clen)
            if (!rpc_fill(r))
                return 0;
        sb_append(body, r->buf + r->pos, (size_t)clen);
        r->pos += (size_t)clen;
        return 1;
    }
}

int rpc_input_pending(RpcReader *r, int timeout_ms)
{
    size_t i;
    for (i = r->pos; i < r->len; i++)
        if (r->buf[i] != '\r' && r->buf[i] != '\n')
            return 1;
    if (r->eof)
        return 0;
    return r->io->wait(r->io->user, timeout_ms) ? 1 : 0;
}

/* -------------------------------------------------------------- writing --- */

void rpc_write(const LspIo *io, const char *json, size_t len)
{
    char hdr[64];
    int n = snprintf(hdr, sizeof hdr, "Content-Length: %lu\r\n\r\n", (unsigned long)len);
    StrBuf sb = {0};
    sb_append(&sb, hdr, (size_t)n);
    sb_append(&sb, json, len);
    io->write(io->user, sb.data, sb.len);
    sb_free(&sb);
}

static void rpc_write_id(JsonWriter *w, const JsonValue *id)
{
    jw_key(w, "id");
    if (id && (id->kind == JSON_NUMBER || id->kind == JSON_STRING))
        jw_value(w, id);
    else
        jw_null(w);
}

void rpc_send_result(const LspIo *io, const JsonValue *id, const char *result_json)
{
    StrBuf sb = {0};
    JsonWriter w;
    jw_init(&w, &sb, 0);
    jw_object_begin(&w);
    jw_kv_str(&w, "jsonrpc", "2.0");
    rpc_write_id(&w, id);
    jw_key(&w, "result");
    jw_raw(&w, result_json ? result_json : "null");
    jw_object_end(&w);
    rpc_write(io, sb.data, sb.len);
    sb_free(&sb);
}

void rpc_send_error(const LspIo *io, const JsonValue *id, int code, const char *message)
{
    StrBuf sb = {0};
    JsonWriter w;
    jw_init(&w, &sb, 0);
    jw_object_begin(&w);
    jw_kv_str(&w, "jsonrpc", "2.0");
    rpc_write_id(&w, id);
    jw_key(&w, "error");
    jw_object_begin(&w);
    jw_kv_int(&w, "code", code);
    jw_kv_str(&w, "message", message);
    jw_object_end(&w);
    jw_object_end(&w);
    rpc_write(io, sb.data, sb.len);
    sb_free(&sb);
}

void rpc_send_notification(const LspIo *io, const char *method, const char *params_json)
{
    StrBuf sb = {0};
    JsonWriter w;
    jw_init(&w, &sb, 0);
    jw_object_begin(&w);
    jw_kv_str(&w, "jsonrpc", "2.0");
    jw_kv_str(&w, "method", method);
    jw_key(&w, "params");
    jw_raw(&w, params_json ? params_json : "null");
    jw_object_end(&w);
    rpc_write(io, sb.data, sb.len);
    sb_free(&sb);
}

#undef RPC_MAX_HEADER
#undef RPC_MAX_BODY
