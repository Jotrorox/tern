/* lsp.h — `tern lsp`, the Language Server Protocol server (spec §13.2).
 *
 * Transport (rpc.c): JSON-RPC 2.0 messages framed by `Content-Length`
 * headers over an LspIo byte stream — stdin/stdout for `tern lsp`
 * (lsp_stdio_io), a memory buffer in the unit tests.
 *
 *     LspIo io = lsp_stdio_io();
 *     LspOptions o = {0};
 *     o.debounce_ms = 150;
 *     int rc = lsp_run(&io, &o);       // returns at `exit` / end of input
 *
 * Server (lsp.c): open documents overlay the files on disk (driver.h's
 * DriverOptions.overlay). Each document belongs to an analysis group: the
 * project of the nearest tern.toml when the file lies in its src/ or tests/,
 * otherwise the file on its own (in its project's context when there is
 * one). After a change the group's whole front end runs again (parse,
 * resolve, elaborate, IR checks — `tern check`), after `debounce_ms` without
 * further input; requests that need fresh results run it at once. Each
 * analysis owns its driver and IR arena and frees the previous one.
 *
 * Supported: initialize/initialized/shutdown/exit, didOpen/didChange
 * (incremental)/didClose/didSave, publishDiagnostics, definition, hover,
 * documentSymbol, formatting, completion, references, documentHighlight,
 * workspace/symbol and the custom request `tern/instanceTree`.
 */
#ifndef TERN_LSP_H
#define TERN_LSP_H

#include "../base/common.h"
#include "../base/str.h"
#include "../base/json.h"

/* ----------------------------------------------------------- transport */

typedef struct LspIo {
    /* Reads up to n bytes: > 0 bytes read, 0 at end of input, < 0 error. */
    long (*read)(void *user, char *buf, size_t n);
    /* 1 when input is available (or at its end), 0 after timeout_ms. */
    int (*wait)(void *user, int timeout_ms);
    /* Writes all n bytes. */
    void (*write)(void *user, const char *data, size_t n);
    void *user;
} LspIo;

/* stdin/stdout through os.h (binary mode on Windows). */
LspIo lsp_stdio_io(void);

typedef struct RpcReader {
    const LspIo *io;
    char *buf;
    size_t len, cap, pos;     /* unread bytes are buf[pos..len) */
    int eof;
} RpcReader;

void rpc_reader_init(RpcReader *r, const LspIo *io);
void rpc_reader_free(RpcReader *r);
/* Reads one framed message into `body` (replacing its contents). Returns 1
 * for a message, 0 at end of input, -1 for a malformed header (the bad
 * header is skipped; the caller may continue). */
int rpc_read(RpcReader *r, StrBuf *body);
/* 1 when unread bytes are buffered or the stream has input within
 * timeout_ms. */
int rpc_input_pending(RpcReader *r, int timeout_ms);

/* Writes one message (the header is added). */
void rpc_write(const LspIo *io, const char *json, size_t len);
/* `result_json` is an encoded JSON value ("null" when NULL). `id` may be
 * NULL (written as null). */
void rpc_send_result(const LspIo *io, const JsonValue *id, const char *result_json);
void rpc_send_error(const LspIo *io, const JsonValue *id, int code, const char *message);
void rpc_send_notification(const LspIo *io, const char *method, const char *params_json);

/* JSON-RPC / LSP error codes */
#define RPC_PARSE_ERROR -32700
#define RPC_INVALID_REQUEST -32600
#define RPC_METHOD_NOT_FOUND -32601
#define RPC_INVALID_PARAMS -32602
#define RPC_INTERNAL_ERROR -32603
#define RPC_SERVER_NOT_INITIALIZED -32002
#define RPC_REQUEST_CANCELLED -32800

/* -------------------------------------------------------------- server */

typedef struct LspOptions {
    int debounce_ms;          /* re-analysis delay after a change; 0: analyse at once */
    FILE *log;                /* message / timing log (--log file), or NULL */
} LspOptions;

/* Serves until `exit` or the end of input. Returns the process exit code:
 * 0 after shutdown + exit, 1 when the input ended or `exit` came without
 * `shutdown`. */
int lsp_run(const LspIo *io, const LspOptions *opt);

/* Helpers shared with the tests. */
char *lsp_uri_to_path(const char *uri);   /* heap; NULL for non-file URIs */
char *lsp_path_to_uri(const char *path);  /* heap */

#endif /* TERN_LSP_H */
