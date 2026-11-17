/* test_lsp.c — tests for `tern lsp` (src/lsp/).
 *
 *     cc -o build/test_lsp tests/unit/test_lsp.c && build/test_lsp   (from the repository root)
 *
 * The server runs in-process over a memory stream (LspIo): each test
 * builds a framed client session, runs lsp_run() to the end and inspects
 * the framed messages the server wrote. Documents are editor buffers
 * (they need not exist on disk), except in the project test, which writes
 * a small project into a temporary directory.
 *
 * Scripted sessions: every tests/lsp/<name>.session is replayed. Lines
 * starting with `-->` are sent, lines starting with `<--` must match the
 * server's messages in order (a subset match: every key of an expected
 * object must be present and equal, arrays must have the same length,
 * the string "$ANY" matches anything). `${URI:file}` expands to the file
 * URI of tests/lsp/<file>. Other lines are comments. */
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
#include "../../src/syntax/lexer.c"
#include "../../src/syntax/parser.c"
#include "../../src/syntax/ast_dump.c"
#include "../../src/ir/ir_build.c"
#include "../../src/ir/ir_json.c"
#include "../../src/sema/resolve.c"
#include "../../src/sema/types.c"
#include "../../src/sema/info.c"
#include "../../src/elab/value.c"
#include "../../src/elab/consteval.c"
#include "../../src/elab/mono.c"
#include "../../src/elab/regfile.c"
#include "../../src/elab/elab.c"
#include "../../src/check/check.c"
#include "../../src/check/drivers.c"
#include "../../src/check/assign.c"
#include "../../src/check/cdc.c"
#include "../../src/check/unused.c"
#include "../../src/pkg/toml.c"
#include "../../src/pkg/manifest.c"
#include "../../src/pkg/lock.c"
#include "../../src/pkg/git.c"
#include "../../src/lsp/rpc.c"
#include "../../src/lsp/lsp.c"
#include "../../src/lsp/lsp_nav.c"
#include "../../src/tools/fmt.c"
#include "../../src/std_embed.c"
#include "../../src/driver.c"

int cmd_print_usage(const char *name)
{
    TERN_UNUSED(name);
    return 0;
}

static int tl_checks, tl_fails;

#define TL_CHECK(c) tl_check((c) ? 1 : 0, #c, __FILE__, __LINE__)

static void tl_check(int ok, const char *expr, const char *file, int line)
{
    tl_checks++;
    if (!ok) {
        tl_fails++;
        fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
    }
}

/* ------------------------------------------------------------ memory io */

typedef struct TlIo {
    const char *in;
    size_t len, pos;
    size_t chunk;             /* bytes per read (exercises reassembly) */
    StrBuf out;
} TlIo;

static long tl_read(void *user, char *buf, size_t n)
{
    TlIo *io = (TlIo *)user;
    size_t k = io->len - io->pos;
    if (k > n)
        k = n;
    if (io->chunk && k > io->chunk)
        k = io->chunk;
    memcpy(buf, io->in + io->pos, k);
    io->pos += k;
    return (long)k;
}

static int tl_wait(void *user, int timeout_ms)
{
    TERN_UNUSED(user);
    TERN_UNUSED(timeout_ms);
    return 1;
}

static void tl_write(void *user, const char *data, size_t n)
{
    sb_append(&((TlIo *)user)->out, data, n);
}

/* A session: framed client messages, then the server's replies. */
typedef struct TlSession {
    StrBuf in;
    TlIo io;
    Arena arena;
    VEC(JsonValue *) msgs;
    int rc;
} TlSession;

static void tl_send(TlSession *s, const char *json)
{
    sb_printf(&s->in, "Content-Length: %u\r\n\r\n%s", (unsigned)strlen(json), json);
}

static void tl_sendf(TlSession *s, const char *fmt, ...) TERN_PRINTF(2, 3);
static void tl_sendf(TlSession *s, const char *fmt, ...)
{
    StrBuf b = {0};
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(&b, fmt, ap);
    va_end(ap);
    tl_send(s, sb_cstr(&b));
    sb_free(&b);
}

/* didOpen with `text` (JSON-escaped here) */
static void tl_open(TlSession *s, const char *uri, const char *text)
{
    StrBuf b = {0};
    sb_printf(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\","
                  "\"languageId\":\"tern\",\"version\":1,\"text\":",
              uri);
    json_escape(&b, text, strlen(text));
    sb_puts(&b, "}}}");
    tl_send(s, sb_cstr(&b));
    sb_free(&b);
}

static void tl_init(TlSession *s)
{
    memset(s, 0, sizeof *s);
    tl_send(s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"capabilities\":{}}}");
    tl_send(s, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
}

static void tl_shutdown(TlSession *s)
{
    tl_send(s, "{\"jsonrpc\":\"2.0\",\"id\":9999,\"method\":\"shutdown\"}");
    tl_send(s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
}

static void tl_run(TlSession *s, size_t chunk, int debounce)
{
    LspIo io;
    LspOptions o;
    const char *p, *end;
    memset(&o, 0, sizeof o);
    o.debounce_ms = debounce;
    s->io.in = s->in.data ? s->in.data : "";
    s->io.len = s->in.len;
    s->io.chunk = chunk;
    io.read = tl_read;
    io.wait = tl_wait;
    io.write = tl_write;
    io.user = &s->io;
    s->rc = lsp_run(&io, &o);
    /* split the replies */
    p = s->io.out.data ? s->io.out.data : "";
    end = p + s->io.out.len;
    while (p < end) {
        const char *hdr_end = strstr(p, "\r\n\r\n");
        unsigned long n;
        JsonValue *v;
        TL_CHECK(hdr_end != NULL && strncmp(p, "Content-Length: ", 16) == 0);
        if (!hdr_end)
            break;
        n = strtoul(p + 16, NULL, 10);
        p = hdr_end + 4;
        TL_CHECK(p + n <= end);
        if (p + n > end)
            break;
        v = json_parse(&s->arena, p, n, NULL);
        TL_CHECK(v != NULL);
        if (v)
            vec_push(s->msgs, v);
        p += n;
    }
}

static void tl_free(TlSession *s)
{
    sb_free(&s->in);
    sb_free(&s->io.out);
    vec_free(s->msgs);
    arena_release(&s->arena);
}

static JsonValue *tl_response(TlSession *s, int64_t id)
{
    size_t i;
    for (i = 0; i < s->msgs.len; i++)
        if (json_get(s->msgs.data[i], "id") && json_as_int(json_get(s->msgs.data[i], "id"), -1) == id &&
            !json_get(s->msgs.data[i], "method"))
            return s->msgs.data[i];
    return NULL;
}

static JsonValue *tl_result(TlSession *s, int64_t id)
{
    return json_get(tl_response(s, id), "result");
}

/* The nth (0-based; -1: last) publishDiagnostics for `uri`. */
static JsonValue *tl_diags(TlSession *s, const char *uri, int nth, int *count)
{
    size_t i;
    JsonValue *last = NULL;
    int n = 0;
    for (i = 0; i < s->msgs.len; i++) {
        JsonValue *m = s->msgs.data[i];
        if (strcmp(json_as_str(json_get(m, "method"), ""), "textDocument/publishDiagnostics") != 0)
            continue;
        if (strcmp(json_as_str(json_get(json_get(m, "params"), "uri"), ""), uri) != 0)
            continue;
        if (n == nth)
            last = json_get(json_get(m, "params"), "diagnostics");
        if (nth < 0)
            last = json_get(json_get(m, "params"), "diagnostics");
        n++;
    }
    if (count)
        *count = n;
    return last;
}

static int tl_has_str(const char *hay, const char *needle)
{
    return hay && strstr(hay, needle) != NULL;
}

static int64_t tl_line(const JsonValue *range, const char *which)
{
    return json_as_int(json_get(json_get(range, which), "line"), -1);
}

static int64_t tl_char(const JsonValue *range, const char *which)
{
    return json_as_int(json_get(json_get(range, which), "character"), -1);
}

/* the file URI of tests/lsp/<name> (need not exist) */
static char *tl_uri(const char *name)
{
    char *dir = os_path_abs("tests/lsp");
    char *p = os_path_join(dir, name);
    char *u = lsp_path_to_uri(p);
    free(dir);
    free(p);
    return u;
}

/* ------------------------------------------------------------------ tests */

static void test_uri(void)
{
    char *p, *u;
    p = lsp_uri_to_path("file:///home/u/a%20b/x.tern");
    TL_CHECK(p && strcmp(p, "/home/u/a b/x.tern") == 0);
    free(p);
    p = lsp_uri_to_path("file:///c%3A/Users/x.tern");
    TL_CHECK(p && strcmp(p, "C:/Users/x.tern") == 0);
    free(p);
    p = lsp_uri_to_path("file://server/share/x.tern");
    TL_CHECK(tl_has_str(p, "server/share/x.tern"));
    free(p);
    TL_CHECK(lsp_uri_to_path("untitled:Untitled-1") == NULL);
    u = lsp_path_to_uri("/home/u/a b/x#1.tern");
    TL_CHECK(strcmp(u, "file:///home/u/a%20b/x%231.tern") == 0);
    free(u);
    u = lsp_path_to_uri("C:/x/y.tern");
    TL_CHECK(strcmp(u, "file:///C:/x/y.tern") == 0);
    free(u);
}

static void test_framing(void)
{
    TlSession s;
    JsonValue *r;
    memset(&s, 0, sizeof s);
    /* a request before initialize, a malformed header, malformed JSON,
     * then a normal session read three bytes at a time */
    tl_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/hover\",\"params\":{}}");
    sb_puts(&s.in, "X-Junk: 1\r\n\r\n");
    tl_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":");
    sb_puts(&s.in, "content-length:   2\n\n{}");
    tl_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
    tl_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tern/noSuchMethod\",\"params\":{}}");
    tl_send(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\",\"params\":{\"id\":2}}");
    tl_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":\"s\",\"method\":\"shutdown\"}");
    tl_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\",\"params\":{}}");
    tl_send(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
    tl_run(&s, 3, 0);
    TL_CHECK(s.rc == 0);
    r = tl_response(&s, 7);
    TL_CHECK(json_as_int(json_get(json_get(r, "error"), "code"), 0) == RPC_SERVER_NOT_INITIALIZED);
    TL_CHECK(s.msgs.len >= 2 &&
             json_as_int(json_get(json_get(s.msgs.data[1], "error"), "code"), 0) == RPC_PARSE_ERROR);
    TL_CHECK(json_is(json_get(s.msgs.data[1], "id"), JSON_NULL));
    TL_CHECK(json_as_int(json_get(json_get(s.msgs.data[2], "error"), "code"), 0) == RPC_INVALID_REQUEST);
    r = tl_result(&s, 1);
    TL_CHECK(json_as_bool(json_get(json_get(r, "capabilities"), "hoverProvider"), 0));
    TL_CHECK(json_as_int(json_get(json_get(json_get(r, "capabilities"), "textDocumentSync"), "change"), 0) == 2);
    TL_CHECK(tl_has_str(json_as_str(json_get(json_get(r, "serverInfo"), "name"), NULL), "tern"));
    TL_CHECK(json_as_int(json_get(json_get(tl_response(&s, 2), "error"), "code"), 0) == RPC_METHOD_NOT_FOUND);
    TL_CHECK(json_as_int(json_get(json_get(tl_response(&s, 3), "error"), "code"), 0) == RPC_INVALID_REQUEST);
    {
        size_t i;
        int found = 0;
        for (i = 0; i < s.msgs.len; i++)
            if (strcmp(json_as_str(json_get(s.msgs.data[i], "id"), ""), "s") == 0 &&
                json_is(json_get(s.msgs.data[i], "result"), JSON_NULL))
                found = 1;
        TL_CHECK(found);
    }
    tl_free(&s);

    /* exit without shutdown, and the end of input */
    tl_init(&s);
    tl_send(&s, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
    tl_run(&s, 0, 0);
    TL_CHECK(s.rc == 1);
    tl_free(&s);
    tl_init(&s);
    tl_run(&s, 0, 0);
    TL_CHECK(s.rc == 1);
    tl_free(&s);
}

static const char tl_src_main[] =
    "/// A counter with a FIFO.\n"
    "module Top {\n"
    "    in clk: clock\n"
    "    in rst: reset\n"
    "    in push: Stream<u8>\n"
    "    out pop: Stream<u8>\n"
    "    /// The number of beats seen.\n"
    "    reg count: u8 = 0\n"
    "    out level: u8\n"
    "    let f = Buf<u8>(push: push)\n"
    "    pop = f.pop\n"
    "    level = count\n"
    "    on clk {\n"
    "        if push.fire() {\n"
    "            count = count +% 1\n"
    "        }\n"
    "    }\n"
    "}\n"
    "\n"
    "module Buf<T: type> {\n"
    "    in clk: clock\n"
    "    in rst: reset\n"
    "    in push: Stream<T>\n"
    "    out pop: Stream<T>\n"
    "    pop = push\n"
    "}\n";

static void test_diagnostics(void)
{
    TlSession s;
    char *uri = tl_uri("diag.tern"), *uri2 = tl_uri("broken.tern");
    JsonValue *d;
    int n;
    tl_init(&s);
    /* a resolution error after a non-ASCII block comment on the same line */
    tl_open(&s, uri,
            "module Top {\n"
            "    out y: u8\n"
            "    /* h\xc3\xa9llo \xf0\x9f\x98\x80 */ y = nope\n"
            "}\n");
    /* a file that fails to parse */
    tl_open(&s, uri2, "module Top {\n    out y: u8\n    y = \n}\n");
    /* fix the first one with an incremental edit: `nope` -> `0` */
    tl_sendf(&s,
             "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"%s\","
             "\"version\":2},\"contentChanges\":[{\"range\":{\"start\":{\"line\":2,\"character\":23},"
             "\"end\":{\"line\":2,\"character\":27}},\"text\":\"0\"}]}}",
             uri);
    tl_shutdown(&s);
    tl_run(&s, 0, 0);
    TL_CHECK(s.rc == 0);
    d = tl_diags(&s, uri, 0, &n);
    TL_CHECK(n == 2);
    TL_CHECK(json_len(d) == 1);
    TL_CHECK(tl_has_str(json_as_str(json_get(json_at(d, 0), "message"), NULL), "nope"));
    TL_CHECK(json_as_int(json_get(json_at(d, 0), "severity"), 0) == 1);
    TL_CHECK(tl_line(json_get(json_at(d, 0), "range"), "start") == 2);
    /* 4 spaces, the comment opener and a space (3), "héllo" (5), a space,
     * the emoji (2 UTF-16 units), a space, the closer, " y = " (8): 23 */
    TL_CHECK(tl_char(json_get(json_at(d, 0), "range"), "start") == 23);
    TL_CHECK(tl_char(json_get(json_at(d, 0), "range"), "end") == 27);
    d = tl_diags(&s, uri, 1, NULL);
    TL_CHECK(d && json_len(d) == 0);
    d = tl_diags(&s, uri2, -1, &n);
    TL_CHECK(n == 1 && json_len(d) >= 1);
    TL_CHECK(tl_line(json_get(json_at(d, 0), "range"), "start") >= 2);
    tl_free(&s);
    free(uri);
    free(uri2);
}

static void test_navigation(void)
{
    TlSession s;
    char *uri = tl_uri("nav.tern");
    JsonValue *r;
    const char *md;
    size_t i;
    tl_init(&s);
    tl_open(&s, uri, tl_src_main);
#define TL_POS_REQ(id, method, line, ch)                                                                              \
    tl_sendf(&s,                                                                                                      \
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},"         \
             "\"position\":{\"line\":%d,\"character\":%d},\"context\":{\"includeDeclaration\":true}}}",              \
             id, method, uri, line, ch)
    TL_POS_REQ(10, "textDocument/definition", 11, 13);   /* level = count */
    TL_POS_REQ(11, "textDocument/hover", 11, 13);
    TL_POS_REQ(12, "textDocument/definition", 4, 15);    /* Stream (std prelude) */
    TL_POS_REQ(13, "textDocument/definition", 9, 13);    /* Buf */
    TL_POS_REQ(14, "textDocument/hover", 9, 13);
    TL_POS_REQ(15, "textDocument/references", 7, 9);     /* count */
    TL_POS_REQ(16, "textDocument/completion", 10, 12);   /* f.| */
    TL_POS_REQ(17, "textDocument/completion", 11, 13);   /* level = c| */
    TL_POS_REQ(18, "textDocument/hover", 10, 13);        /* f.pop */
    TL_POS_REQ(19, "textDocument/definition", 22, 15);   /* Stream inside the generic Buf */
    TL_POS_REQ(20, "textDocument/documentHighlight", 7, 9);
    TL_POS_REQ(21, "textDocument/definition", 9, 21);    /* Buf<u8>(push: ...) */
    TL_POS_REQ(22, "textDocument/hover", 9, 21);
    tl_sendf(&s,
             "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"textDocument/documentSymbol\",\"params\":{\"textDocument\":"
             "{\"uri\":\"%s\"}}}",
             uri);
    tl_sendf(&s, "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"tern/instanceTree\",\"params\":{\"textDocument\":"
                 "{\"uri\":\"%s\"}}}",
             uri);
    tl_send(&s, "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"workspace/symbol\",\"params\":{\"query\":\"bu\"}}");
#undef TL_POS_REQ
    tl_shutdown(&s);
    tl_run(&s, 0, 0);
    TL_CHECK(s.rc == 0);
    TL_CHECK(tl_diags(&s, uri, -1, NULL) && json_len(tl_diags(&s, uri, -1, NULL)) == 0);

    r = tl_result(&s, 10);
    TL_CHECK(strcmp(json_as_str(json_get(r, "uri"), ""), uri) == 0);
    TL_CHECK(tl_line(json_get(r, "range"), "start") == 7 && tl_char(json_get(r, "range"), "start") == 8);

    md = json_as_str(json_get(json_get(tl_result(&s, 11), "contents"), "value"), "");
    TL_CHECK(tl_has_str(md, "count: uint<8> @ clk"));
    TL_CHECK(tl_has_str(md, "8 bits"));
    TL_CHECK(tl_has_str(md, "driven by `on clk`"));
    TL_CHECK(tl_has_str(md, "The number of beats seen."));
    TL_CHECK(strcmp(json_as_str(json_get(json_get(tl_result(&s, 11), "contents"), "kind"), ""), "markdown") == 0);

    r = tl_result(&s, 12);
    TL_CHECK(tl_has_str(json_as_str(json_get(r, "uri"), NULL), "std/prelude/prelude.tern"));
    r = tl_result(&s, 13);
    TL_CHECK(tl_line(json_get(r, "range"), "start") == 19);
    md = json_as_str(json_get(json_get(tl_result(&s, 14), "contents"), "value"), "");
    TL_CHECK(tl_has_str(md, "module Buf<T: type>"));
    TL_CHECK(tl_has_str(md, "out pop: Stream<T>"));

    r = tl_result(&s, 15);
    TL_CHECK(json_len(r) == 4); /* declaration, level = count, count = count +% 1 (twice) */

    r = json_get(tl_result(&s, 16), "items");
    {
        int has_pop = 0, has_push = 0;
        for (i = 0; i < json_len(r); i++) {
            const char *l = json_as_str(json_get(json_at(r, i), "label"), "");
            has_pop |= strcmp(l, "pop") == 0;
            has_push |= strcmp(l, "push") == 0;
        }
        TL_CHECK(has_pop && has_push);
    }
    r = json_get(tl_result(&s, 17), "items");
    {
        int has_count = 0, has_clock = 0, has_kw = 0;
        for (i = 0; i < json_len(r); i++) {
            const char *l = json_as_str(json_get(json_at(r, i), "label"), "");
            has_count |= strcmp(l, "count") == 0;
            has_clock |= strcmp(l, "clock") == 0;
            has_kw |= strcmp(l, "comb") == 0;
        }
        TL_CHECK(has_count && has_clock && has_kw);
    }
    md = json_as_str(json_get(json_get(tl_result(&s, 18), "contents"), "value"), "");
    TL_CHECK(tl_has_str(md, "out pop: Stream<uint<8>>"));
    r = tl_result(&s, 19);
    TL_CHECK(tl_has_str(json_as_str(json_get(r, "uri"), NULL), "std/prelude/prelude.tern"));
    TL_CHECK(json_len(tl_result(&s, 20)) == 4);
    r = tl_result(&s, 21);
    TL_CHECK(tl_line(json_get(r, "range"), "start") == 22 && tl_char(json_get(r, "range"), "start") == 7);
    md = json_as_str(json_get(json_get(tl_result(&s, 22), "contents"), "value"), "");
    TL_CHECK(tl_has_str(md, "in push: Stream<uint<8>>"));

    r = tl_result(&s, 30);
    TL_CHECK(json_len(r) == 2);
    TL_CHECK(strcmp(json_as_str(json_get(json_at(r, 0), "name"), ""), "Top") == 0);
    TL_CHECK(json_as_int(json_get(json_at(r, 0), "kind"), 0) == 2);
    {
        JsonValue *ch = json_get(json_at(r, 0), "children");
        int inst = 0, reg = 0, port = 0;
        for (i = 0; i < json_len(ch); i++) {
            const char *n = json_as_str(json_get(json_at(ch, i), "name"), "");
            int64_t k = json_as_int(json_get(json_at(ch, i), "kind"), 0);
            if (strcmp(n, "f") == 0 && k == 19)
                inst = 1;
            if (strcmp(n, "count") == 0 && k == 13)
                reg = 1;
            if (strcmp(n, "push") == 0 && k == 7 &&
                strcmp(json_as_str(json_get(json_at(ch, i), "detail"), ""), "in Stream<u8>") == 0)
                port = 1;
        }
        TL_CHECK(inst && reg && port);
    }

    r = tl_result(&s, 31);
    TL_CHECK(json_len(r) == 1);
    TL_CHECK(strcmp(json_as_str(json_get(json_at(r, 0), "module"), ""), "Top") == 0);
    r = json_get(json_at(r, 0), "children");
    TL_CHECK(json_len(r) == 1);
    TL_CHECK(strcmp(json_as_str(json_get(json_at(r, 0), "name"), ""), "f") == 0);
    TL_CHECK(strcmp(json_as_str(json_get(json_at(r, 0), "display"), ""), "Buf<uint<8>>") == 0);
    TL_CHECK(tl_line(json_get(json_get(json_at(r, 0), "location"), "range"), "start") == 9);
    TL_CHECK(tl_line(json_get(json_get(json_at(r, 0), "definition"), "range"), "start") == 19);

    r = tl_result(&s, 32);
    TL_CHECK(json_len(r) == 1 && strcmp(json_as_str(json_get(json_at(r, 0), "name"), ""), "Buf") == 0);
    tl_free(&s);
    free(uri);
}

static void test_formatting(void)
{
    TlSession s;
    char *uri = tl_uri("fmt.tern"), *bad = tl_uri("fmt_bad.tern");
    JsonValue *r;
    tl_init(&s);
    tl_open(&s, uri, "module   Top{\n  out y :u8\n  y=1\n}\n");
    tl_open(&s, bad, "module Top {\n  out y: u8\n  y = (\n}\n");
    tl_sendf(&s,
             "{\"jsonrpc\":\"2.0\",\"id\":40,\"method\":\"textDocument/formatting\",\"params\":{\"textDocument\":{\"uri\":"
             "\"%s\"},\"options\":{\"tabSize\":4,\"insertSpaces\":true}}}",
             uri);
    tl_sendf(&s,
             "{\"jsonrpc\":\"2.0\",\"id\":41,\"method\":\"textDocument/formatting\",\"params\":{\"textDocument\":{\"uri\":"
             "\"%s\"},\"options\":{\"tabSize\":4,\"insertSpaces\":true}}}",
             bad);
    tl_shutdown(&s);
    tl_run(&s, 0, 0);
    r = tl_result(&s, 40);
    TL_CHECK(json_len(r) == 1);
    TL_CHECK(strcmp(json_as_str(json_get(json_at(r, 0), "newText"), ""), "module Top {\n    out y: u8\n    y = 1\n}\n") ==
             0);
    TL_CHECK(tl_line(json_get(json_at(r, 0), "range"), "end") == 4);
    r = tl_result(&s, 41);
    TL_CHECK(json_is(r, JSON_ARRAY) && json_len(r) == 0);
    tl_free(&s);
    free(uri);
    free(bad);
}

/* Many edits: the source registry must not grow, analyses are released. */
static void test_bounded(void)
{
    TlSession s;
    char *uri = tl_uri("many.tern");
    int i, n;
    uint32_t before;
    tl_init(&s);
    tl_open(&s, uri, tl_src_main);
    for (i = 0; i < 20; i++)
        tl_sendf(&s,
                 "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":"
                 "\"%s\",\"version\":%d},\"contentChanges\":[{\"text\":\"module Top {\\n    out y: u8\\n    y = %s\\n}\\n\"}]}}",
                 uri, i + 2, i % 2 ? "(" : "nope");
    tl_shutdown(&s);
    /* one warm-up session registers std; the second must add nothing */
    tl_run(&s, 0, 0);
    tl_free(&s);
    before = src_count();
    tl_init(&s);
    tl_open(&s, uri, tl_src_main);
    for (i = 0; i < 30; i++)
        tl_sendf(&s,
                 "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":"
                 "\"%s\",\"version\":%d},\"contentChanges\":[{\"text\":\"module Top {\\n    out y: u%d\\n    y = %d\\n}\\n\"}]}}",
                 uri, i + 2, 8 + i, i);
    tl_shutdown(&s);
    tl_run(&s, 0, 0);
    TL_CHECK(s.rc == 0);
    TL_CHECK(src_count() == before);
    tl_diags(&s, uri, -1, &n);
    TL_CHECK(n == 31);
    tl_free(&s);

    /* debounce: edits followed by a request are analysed once */
    tl_init(&s);
    tl_open(&s, uri, "module Top {\n    out y: u8\n    y = nope\n}\n");
    for (i = 0; i < 5; i++)
        tl_sendf(&s,
                 "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":"
                 "\"%s\",\"version\":%d},\"contentChanges\":[{\"text\":\"module Top {\\n    out y: u8\\n    y = %d\\n}\\n\"}]}}",
                 uri, i + 2, i);
    tl_sendf(&s,
             "{\"jsonrpc\":\"2.0\",\"id\":50,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":"
             "\"%s\"},\"position\":{\"line\":2,\"character\":4}}}",
             uri);
    tl_shutdown(&s);
    tl_run(&s, 0, 200);
    tl_diags(&s, uri, -1, &n);
    TL_CHECK(n == 1);
    TL_CHECK(json_len(tl_diags(&s, uri, -1, NULL)) == 0);
    TL_CHECK(tl_has_str(json_as_str(json_get(json_get(tl_result(&s, 50), "contents"), "value"), NULL), "y: uint<8>"));
    tl_free(&s);
    free(uri);
}

/* A project on disk: diagnostics in a file that is not open, cleared once
 * its buffer is fixed; definitions across files. */
static void test_project(void)
{
    char *tmp = os_temp_dir();
    char *root, *src, *toml, *a, *b, *ua, *ub;
    StrBuf name = {0};
    TlSession s;
    JsonValue *r;
    int n;
    sb_printf(&name, "tern-lsp-test-%lu", (unsigned long)(os_time_ns() % 1000000007u));
    root = os_path_join(tmp, sb_cstr(&name));
    src = os_path_join(root, "src");
    toml = os_path_join(root, "tern.toml");
    a = os_path_join(src, "a.tern");
    b = os_path_join(src, "b.tern");
    TL_CHECK(os_mkdir_p(src) == 0);
    {
        static const char m[] = "[package]\nname = \"lsptest\"\nversion = \"0.1.0\"\n";
        static const char ta[] = "#[top]\nmodule Top {\n    out y: u8\n    let h = Helper()\n    y = h.v\n}\n";
        static const char tb[] = "module Helper {\n    out v: u8\n    v = missing\n}\n";
        os_write_file(toml, m, sizeof m - 1);
        os_write_file(a, ta, sizeof ta - 1);
        os_write_file(b, tb, sizeof tb - 1);
    }
    ua = lsp_path_to_uri(a);
    ub = lsp_path_to_uri(b);
    tl_init(&s);
    {
        char *text = NULL;
        size_t len;
        os_read_file(a, &text, &len);
        tl_open(&s, ua, text);
        free(text);
    }
    tl_sendf(&s,
             "{\"jsonrpc\":\"2.0\",\"id\":60,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":"
             "\"%s\"},\"position\":{\"line\":3,\"character\":13}}}",
             ua);
    tl_open(&s, ub, "module Helper {\n    out v: u8\n    v = 3\n}\n");
    tl_shutdown(&s);
    tl_run(&s, 0, 0);
    TL_CHECK(s.rc == 0);
    r = tl_diags(&s, ub, 0, &n);
    TL_CHECK(n == 2);
    TL_CHECK(json_len(r) == 1 && tl_has_str(json_as_str(json_get(json_at(r, 0), "message"), NULL), "missing"));
    TL_CHECK(json_len(tl_diags(&s, ub, 1, NULL)) == 0);
    r = tl_result(&s, 60);
    TL_CHECK(strcmp(json_as_str(json_get(r, "uri"), ""), ub) == 0);
    TL_CHECK(tl_line(json_get(r, "range"), "start") == 0);
    tl_free(&s);
    os_remove(a);
    os_remove(b);
    os_remove(toml);
    os_remove(src);
    os_remove(root);
    free(tmp);
    free(root);
    free(src);
    free(toml);
    free(a);
    free(b);
    free(ua);
    free(ub);
    sb_free(&name);
}

/* ------------------------------------------------------ scripted sessions */

static int tl_match(const JsonValue *want, const JsonValue *got)
{
    size_t i;
    if (!want)
        return 1;
    if (want->kind == JSON_STRING && strcmp(want->str, "$ANY") == 0)
        return got != NULL;
    if (!got || want->kind != got->kind)
        return 0;
    switch (want->kind) {
    case JSON_NULL: return 1;
    case JSON_BOOL: return want->boolean == got->boolean;
    case JSON_NUMBER: return want->number == got->number;
    case JSON_STRING: return want->len == got->len && memcmp(want->str, got->str, want->len) == 0;
    case JSON_ARRAY:
        if (want->n != got->n)
            return 0;
        for (i = 0; i < want->n; i++)
            if (!tl_match(want->items[i], got->items[i]))
                return 0;
        return 1;
    case JSON_OBJECT:
        for (i = 0; i < want->n; i++)
            if (!tl_match(want->members[i].value, json_get(got, want->members[i].key)))
                return 0;
        return 1;
    }
    return 0;
}

static void tl_expand(StrBuf *out, const char *line, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (i + 6 < n && strncmp(line + i, "${URI:", 6) == 0) {
            size_t e = i + 6;
            char *nm, *u;
            while (e < n && line[e] != '}')
                e++;
            nm = xstrndup(line + i + 6, e - i - 6);
            u = tl_uri(nm);
            sb_puts(out, u);
            free(u);
            free(nm);
            i = e + 1;
        } else {
            sb_putc(out, line[i++]);
        }
    }
}

static void test_sessions(void)
{
    OsDirList list;
    size_t f;
    int nsessions = 0;
    if (os_list_dir("tests/lsp", &list) != 0)
        return;
    for (f = 0; f < list.len; f++) {
        char *path;
        char *text = NULL;
        size_t len, i = 0, next = 0;
        TlSession s;
        VEC(JsonValue *) want = {0};
        Arena wa = {0};
        if (strcmp(os_path_ext(list.items[f].name), ".session") != 0)
            continue;
        path = os_path_join("tests/lsp", list.items[f].name);
        if (os_read_file(path, &text, &len) != 0) {
            free(path);
            continue;
        }
        nsessions++;
        memset(&s, 0, sizeof s);
        while (i < len) {
            size_t ls = i, le;
            StrBuf line = {0};
            while (i < len && text[i] != '\n')
                i++;
            le = i;
            if (i < len)
                i++;
            if (le > ls && text[le - 1] == '\r')
                le--;
            if (le - ls < 4 || (strncmp(text + ls, "--> ", 4) != 0 && strncmp(text + ls, "<-- ", 4) != 0))
                continue;
            tl_expand(&line, text + ls + 4, le - ls - 4);
            if (text[ls] == '-') {
                tl_send(&s, sb_cstr(&line));
            } else {
                JsonError err;
                JsonValue *v = json_parse(&wa, line.data, line.len, &err);
                if (!v)
                    fprintf(stderr, "%s: bad expectation: %s\n", path, err.msg);
                TL_CHECK(v != NULL);
                if (v)
                    vec_push(want, v);
            }
            sb_free(&line);
        }
        tl_run(&s, 0, 0);
        for (next = 0; next < want.len; next++) {
            int ok = next < s.msgs.len && tl_match(want.data[next], s.msgs.data[next]);
            if (!ok) {
                StrBuf a = {0}, b = {0};
                json_to_sb(&a, want.data[next], 0);
                if (next < s.msgs.len)
                    json_to_sb(&b, s.msgs.data[next], 0);
                fprintf(stderr, "%s: message %u\n  expected: %s\n  got:      %s\n", path, (unsigned)next + 1,
                        sb_cstr(&a), next < s.msgs.len ? sb_cstr(&b) : "(nothing)");
                sb_free(&a);
                sb_free(&b);
            }
            TL_CHECK(ok);
        }
        TL_CHECK(s.msgs.len == want.len);
        tl_free(&s);
        vec_free(want);
        arena_release(&wa);
        free(text);
        free(path);
    }
    os_dir_list_free(&list);
    TL_CHECK(nsessions > 0);
}

int main(void)
{
    test_uri();
    test_framing();
    test_diagnostics();
    test_navigation();
    test_formatting();
    test_bounded();
    test_project();
    test_sessions();
    diag_flush();
    printf("test_lsp: %d checks, %d failed\n", tl_checks, tl_fails);
    return tl_fails ? 1 : 0;
}
