/* embed.c — generate src/std_embed.c from the standard library sources.
 *
 *     cc -o tools/embed tools/embed.c        (standalone; no other inputs)
 *     tools/embed [<std-dir> [<output.c>]]   (defaults: std src/std_embed.c)
 *
 * Walks <std-dir> recursively, collects every *.tern file (sorted by path),
 * and writes a C file holding each file's contents as an array of string
 * literal chunks. Each chunk holds at most EMBED_CHUNK bytes, which keeps
 * every literal far below both C99's 4095-character minimum limit and
 * MSVC's 16 KB limit (non-ASCII bytes are escaped as 4-character hex
 * escapes, so even the source form stays under 16 KB). A hex escape
 * followed by a hex digit closes the literal and opens the next one
 * (`"\xe2" "a"`): hex escapes are greedy, and MSVC warns about octal
 * escapes followed by a digit (C4125). '\r' before '\n'
 * is dropped so the output does not depend on the checkout's line endings.
 * The output is only rewritten when its contents change.
 */
#include "../src/os/os_posix.c"
#include "../src/os/os_win32.c"
#include "../src/os/os_common.c"
#include "../src/base/common.c"

#define EMBED_CHUNK 4000

typedef struct EmbedBuf {
    char *data;
    size_t len, cap;
} EmbedBuf;

static void embed_put(EmbedBuf *b, const char *p, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 65536;
        while (nc < b->len + n + 1)
            nc *= 2;
        b->data = (char *)xrealloc(b->data, nc);
        b->cap = nc;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void embed_puts(EmbedBuf *b, const char *s)
{
    embed_put(b, s, strlen(s));
}

static void embed_printf(EmbedBuf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    embed_put(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

typedef struct EmbedList {
    char **items;
    size_t len, cap;
} EmbedList;

static void embed_walk(const char *dir, EmbedList *out)
{
    OsDirList list;
    size_t i;
    if (os_list_dir(dir, &list) != 0) {
        fprintf(stderr, "embed: cannot read directory %s\n", dir);
        exit(1);
    }
    for (i = 0; i < list.len; i++) {
        char *p = os_path_join(dir, list.items[i].name);
        if (list.items[i].name[0] == '.') {
            free(p);
            continue;
        }
        if (list.items[i].is_dir) {
            embed_walk(p, out);
            free(p);
        } else if (strcmp(os_path_ext(p), ".tern") == 0) {
            if (out->len == out->cap) {
                out->cap = out->cap ? out->cap * 2 : 64;
                out->items = (char **)xrealloc(out->items, out->cap * sizeof(char *));
            }
            out->items[out->len++] = p;
        } else {
            free(p);
        }
    }
    os_dir_list_free(&list);
}

static int embed_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* Append byte c to the current literal; *after_hex says the previous
 * byte was written as a hex escape (and is updated). */
static void embed_byte(EmbedBuf *o, unsigned char c, int *after_hex)
{
    char esc[8];
    int hex = *after_hex;
    *after_hex = 0;
    switch (c) {
    case '"': embed_puts(o, "\\\""); return;
    case '\\': embed_puts(o, "\\\\"); return;
    case '?': embed_puts(o, "\\?"); return; /* no trigraphs, ever */
    case '\t': embed_puts(o, "\\t"); return;
    case '\n': embed_puts(o, "\\n"); return;
    default: break;
    }
    if (c >= 0x20 && c < 0x7F) {
        if (hex && tern_is_xdigit(c))
            embed_puts(o, "\" \"");
        esc[0] = (char)c;
        embed_put(o, esc, 1);
    } else {
        snprintf(esc, sizeof esc, "\\x%02x", (unsigned)c);
        embed_puts(o, esc);
        *after_hex = 1;
    }
}

/* Emit the chunk array for one file; returns the number of chunks. */
static size_t embed_file(EmbedBuf *o, size_t idx, const char *text, size_t len)
{
    size_t pos = 0, nchunks = 0;
    embed_printf(o, "static const char *const tern_std_chunks_%lu[] = {\n", (unsigned long)idx);
    if (len == 0) {
        embed_puts(o, "    \"\"\n};\n\n");
        return 1;
    }
    while (pos < len) {
        /* a chunk: whole lines while they fit, splitting only overlong lines */
        size_t end = pos, limit = pos + EMBED_CHUNK < len ? pos + EMBED_CHUNK : len;
        size_t i;
        int after_hex = 0;
        while (end < limit) {
            const char *nl = (const char *)memchr(text + end, '\n', len - end);
            size_t line_end = nl ? (size_t)(nl - text) + 1 : len;
            if (line_end > limit) {
                if (end == pos)
                    end = limit; /* line longer than a chunk: split it */
                break;
            }
            end = line_end;
        }
        embed_puts(o, nchunks ? ",\n" : "");
        embed_puts(o, "    \"");
        for (i = pos; i < end; i++) {
            embed_byte(o, (unsigned char)text[i], &after_hex);
            if (text[i] == '\n' && i + 1 < end)
                embed_puts(o, "\"\n    \"");
        }
        embed_puts(o, "\"");
        nchunks++;
        pos = end;
    }
    embed_puts(o, "\n};\n\n");
    return nchunks;
}

int main(int argc, char **argv)
{
    const char *std_dir = argc > 1 ? argv[1] : "std";
    const char *out_path = argc > 2 ? argv[2] : "src/std_embed.c";
    char *root = os_path_normalize(std_dir);
    EmbedList files = {0};
    EmbedBuf o = {0};
    size_t i, *nchunks, *lens;
    int rc;

    if (os_is_dir(root))
        embed_walk(root, &files);
    if (files.len > 1)
        qsort(files.items, files.len, sizeof(char *), embed_cmp);
    nchunks = (size_t *)xcalloc(files.len + 1, sizeof(size_t));
    lens = (size_t *)xcalloc(files.len + 1, sizeof(size_t));

    embed_puts(&o,
               "/* Generated by tools/embed.c from std/ -- do not edit.\n"
               " * Regenerate with `make` (or `cc -o tools/embed tools/embed.c && tools/embed`). */\n"
               "#include \"std_embed.h\"\n\n");
    for (i = 0; i < files.len; i++) {
        char *text;
        size_t len, r, w;
        if (os_read_file(files.items[i], &text, &len) != 0) {
            fprintf(stderr, "embed: cannot read %s\n", files.items[i]);
            return 1;
        }
        if (memchr(text, '\0', len)) {
            fprintf(stderr, "embed: %s contains a NUL byte\n", files.items[i]);
            return 1;
        }
        for (r = w = 0; r < len; r++) {
            if (text[r] == '\r' && r + 1 < len && text[r + 1] == '\n')
                continue;
            text[w++] = text[r];
        }
        lens[i] = w;
        nchunks[i] = embed_file(&o, i, text, w);
        free(text);
    }
    if (files.len == 0) {
        embed_puts(&o, "const StdEmbedFile tern_std_files[1] = {{NULL, NULL, 0, 0}};\n");
    } else {
        embed_puts(&o, "const StdEmbedFile tern_std_files[] = {\n");
        for (i = 0; i < files.len; i++) {
            const char *p = files.items[i];
            char *q;
            for (q = files.items[i]; *q; q++) {
                if (*q == '\\')
                    *q = '/';
            }
            embed_printf(&o, "    {\"%s\", tern_std_chunks_%lu, %lu, %lu},\n", p, (unsigned long)i,
                         (unsigned long)nchunks[i], (unsigned long)lens[i]);
        }
        embed_puts(&o, "};\n");
    }
    embed_printf(&o, "const size_t tern_std_file_count = %lu;\n\n", (unsigned long)files.len);
    embed_printf(&o,
                 "const char *tern_std_text(size_t i)\n"
                 "{\n"
                 "    static char *cache[%lu];\n"
                 "    const StdEmbedFile *f;\n"
                 "    size_t k, off = 0;\n"
                 "    if (i >= tern_std_file_count)\n"
                 "        return NULL;\n"
                 "    if (cache[i])\n"
                 "        return cache[i];\n"
                 "    f = &tern_std_files[i];\n"
                 "    cache[i] = (char *)xmalloc(f->len + 1);\n"
                 "    for (k = 0; k < f->nchunks; k++) {\n"
                 "        size_t n = strlen(f->chunks[k]);\n"
                 "        memcpy(cache[i] + off, f->chunks[k], n);\n"
                 "        off += n;\n"
                 "    }\n"
                 "    cache[i][off] = '\\0';\n"
                 "    return cache[i];\n"
                 "}\n",
                 (unsigned long)(files.len ? files.len : 1));

    rc = os_write_file_if_changed(out_path, o.data, o.len);
    if (rc < 0) {
        fprintf(stderr, "embed: cannot write %s\n", out_path);
        return 1;
    }
    if (rc > 0)
        printf("embed: wrote %s (%lu files)\n", out_path, (unsigned long)files.len);
    for (i = 0; i < files.len; i++)
        free(files.items[i]);
    free(files.items);
    free(nchunks);
    free(lens);
    free(o.data);
    free(root);
    return 0;
}

#undef EMBED_CHUNK
