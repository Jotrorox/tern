/* os_win32.c — platform layer for Windows (Win32 API, UTF-8 <-> UTF-16).
 *
 * Child processes write captured output to delete-on-close temporary files
 * instead of pipes: no reader threads or overlapped I/O are needed, a child
 * can never block on a full pipe, and waiting for any of several children
 * is a single WaitForMultipleObjects. */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif

#include "os.h"

#include <windows.h>
#include <io.h>
#include <fcntl.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

/* Windows FILETIME epoch (1601) to Unix epoch, in 100 ns units. */
#define OSW_EPOCH_DIFF 116444736000000000ULL

/* ------------------------------------------------------------- strings --- */

static wchar_t *osw_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) {
        w = (wchar_t *)xmalloc(sizeof(wchar_t));
        w[0] = 0;
        return w;
    }
    w = (wchar_t *)xmalloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/* Wide path with '/' turned into '\\'. */
static wchar_t *osw_path(const char *s)
{
    wchar_t *w = osw_wide(s), *p;
    for (p = w; *p; p++) {
        if (*p == L'/')
            *p = L'\\';
    }
    return w;
}

static char *osw_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 0)
        return xstrdup("");
    s = (char *)xmalloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static char *osw_utf8_slashes(const wchar_t *w)
{
    char *s = osw_utf8(w), *p;
    for (p = s; *p; p++) {
        if (*p == '\\')
            *p = '/';
    }
    return s;
}

/* ---------------------------------------------------------------- files --- */

static int osw_read_handle(HANDLE h, char **data, size_t *len)
{
    size_t cap = 4096, n = 0;
    char *buf = (char *)xmalloc(cap);
    for (;;) {
        DWORD got = 0, want;
        if (n + 1 >= cap) {
            cap *= 2;
            buf = (char *)xrealloc(buf, cap);
        }
        want = (DWORD)((cap - n - 1) > 0x40000000u ? 0x40000000u : (cap - n - 1));
        if (!ReadFile(h, buf + n, want, &got, NULL)) {
            if (GetLastError() == ERROR_BROKEN_PIPE)
                break;
            free(buf);
            return -1;
        }
        if (got == 0)
            break;
        n += got;
    }
    buf[n] = '\0';
    *data = buf;
    if (len)
        *len = n;
    return 0;
}

int os_read_file(const char *path, char **data, size_t *len)
{
    wchar_t *w = osw_path(path);
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    int rc;
    free(w);
    *data = NULL;
    if (len)
        *len = 0;
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    rc = osw_read_handle(h, data, len);
    CloseHandle(h);
    return rc;
}

static int osw_write_all(HANDLE h, const char *p, size_t n)
{
    while (n) {
        DWORD put = 0, chunk = (DWORD)(n > 0x40000000u ? 0x40000000u : n);
        if (!WriteFile(h, p, chunk, &put, NULL))
            return -1;
        p += put;
        n -= put;
    }
    return 0;
}

int os_write_file(const char *path, const void *data, size_t len)
{
    wchar_t *w = osw_path(path);
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    int rc;
    free(w);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    rc = osw_write_all(h, (const char *)data, len);
    if (!CloseHandle(h))
        rc = -1;
    return rc;
}

int os_rename(const char *from, const char *to)
{
    wchar_t *a = osw_path(from), *b = osw_path(to);
    BOOL ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    free(a);
    free(b);
    return ok ? 0 : -1;
}

int os_remove(const char *path)
{
    wchar_t *w = osw_path(path);
    DWORD attr = GetFileAttributesW(w);
    BOOL ok;
    if (attr == INVALID_FILE_ATTRIBUTES) {
        free(w);
        return -1;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        ok = RemoveDirectoryW(w);
    } else {
        if (attr & FILE_ATTRIBUTE_READONLY)
            SetFileAttributesW(w, attr & ~(DWORD)FILE_ATTRIBUTE_READONLY);
        ok = DeleteFileW(w);
    }
    free(w);
    return ok ? 0 : -1;
}

int os_mkdir(const char *path)
{
    wchar_t *w = osw_path(path);
    BOOL ok = CreateDirectoryW(w, NULL);
    DWORD err = ok ? 0 : GetLastError();
    free(w);
    if (ok)
        return 0;
    if (err == ERROR_ALREADY_EXISTS && os_is_dir(path))
        return 0;
    return -1;
}

static int64_t osw_filetime_ns(FILETIME ft)
{
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    if (t < OSW_EPOCH_DIFF)
        return 0;
    return (int64_t)((t - OSW_EPOCH_DIFF) * 100u);
}

int os_stat(const char *path, OsStat *st)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    wchar_t *w = osw_path(path);
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &fad);
    free(w);
    memset(st, 0, sizeof *st);
    if (!ok)
        return -1;
    st->exists = 1;
    st->is_dir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    st->is_file = !st->is_dir && !(fad.dwFileAttributes & FILE_ATTRIBUTE_DEVICE);
    st->is_link = (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
    st->size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    st->mtime_ns = osw_filetime_ns(fad.ftLastWriteTime);
    return 0;
}

static int osw_entry_cmp(const void *a, const void *b)
{
    return strcmp(((const OsDirEntry *)a)->name, ((const OsDirEntry *)b)->name);
}

int os_list_dir(const char *path, OsDirList *out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char *pattern = os_path_join(path, "*");
    wchar_t *w = osw_path(pattern);
    size_t cap = 0;
    free(pattern);
    out->items = NULL;
    out->len = 0;
    h = FindFirstFileW(w, &fd);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (out->len == cap) {
            cap = cap ? cap * 2 : 16;
            out->items = (OsDirEntry *)xrealloc(out->items, cap * sizeof(OsDirEntry));
        }
        out->items[out->len].name = osw_utf8(fd.cFileName);
        out->items[out->len].is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        out->items[out->len].is_link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
        out->len++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (out->len > 1)
        qsort(out->items, out->len, sizeof(OsDirEntry), osw_entry_cmp);
    return 0;
}

/* ---------------------------------------------------------------- paths --- */

char *os_getcwd(void)
{
    DWORD n = GetCurrentDirectoryW(0, NULL);
    wchar_t *w;
    char *s;
    if (n == 0)
        return NULL;
    w = (wchar_t *)xmalloc((size_t)(n + 1) * sizeof(wchar_t));
    if (GetCurrentDirectoryW(n + 1, w) == 0) {
        free(w);
        return NULL;
    }
    s = osw_utf8_slashes(w);
    free(w);
    return s;
}

int os_chdir(const char *path)
{
    wchar_t *w = osw_path(path);
    BOOL ok = SetCurrentDirectoryW(w);
    free(w);
    return ok ? 0 : -1;
}

char *os_home_dir(void)
{
    char *h = os_getenv("USERPROFILE");
    char *drive, *p, *r;
    size_t n;
    if (h && h[0]) {
        for (p = h; *p; p++) {
            if (*p == '\\')
                *p = '/';
        }
        return h;
    }
    free(h);
    drive = os_getenv("HOMEDRIVE");
    p = os_getenv("HOMEPATH");
    if (!drive || !p) {
        free(drive);
        free(p);
        return NULL;
    }
    n = strlen(drive) + strlen(p) + 1;
    r = (char *)xmalloc(n);
    snprintf(r, n, "%s%s", drive, p);
    free(drive);
    free(p);
    for (p = r; *p; p++) {
        if (*p == '\\')
            *p = '/';
    }
    return r;
}

char *os_temp_dir(void)
{
    wchar_t buf[MAX_PATH + 2];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    char *s;
    size_t len;
    if (n == 0 || n > MAX_PATH)
        return xstrdup(".");
    s = osw_utf8_slashes(buf);
    len = strlen(s);
    if (len > 3 && s[len - 1] == '/')
        s[len - 1] = '\0';
    return s;
}

char *os_exe_path(void)
{
    DWORD cap = MAX_PATH;
    for (;;) {
        wchar_t *w = (wchar_t *)xmalloc((size_t)cap * sizeof(wchar_t));
        DWORD n = GetModuleFileNameW(NULL, w, cap);
        if (n == 0) {
            free(w);
            return NULL;
        }
        if (n < cap) {
            char *s = osw_utf8_slashes(w);
            free(w);
            return s;
        }
        free(w);
        if (cap > 65536)
            return NULL;
        cap *= 2;
    }
}

/* ----------------------------------------------------------- environment --- */

char *os_getenv(const char *name)
{
    wchar_t *wn = osw_wide(name), *wv;
    DWORD n;
    char *s;
    SetLastError(0);
    n = GetEnvironmentVariableW(wn, NULL, 0);
    if (n == 0) {
        DWORD err = GetLastError();
        free(wn);
        return err == ERROR_ENVVAR_NOT_FOUND ? NULL : xstrdup("");
    }
    wv = (wchar_t *)xmalloc((size_t)n * sizeof(wchar_t));
    if (GetEnvironmentVariableW(wn, wv, n) == 0) {
        free(wn);
        free(wv);
        return NULL;
    }
    s = osw_utf8(wv);
    free(wn);
    free(wv);
    return s;
}

int os_setenv(const char *name, const char *value)
{
    wchar_t *wn = osw_wide(name);
    wchar_t *wv = value ? osw_wide(value) : NULL;
    BOOL ok = SetEnvironmentVariableW(wn, wv);
    free(wn);
    free(wv);
    return ok ? 0 : -1;
}

/* Try `base` and base + each PATHEXT extension; returns a heap path or NULL. */
static char *osw_try_exts(const char *base)
{
    char *exts, *p;
    const char *ext = os_path_ext(base);
    if (ext[0] && os_is_file(base))
        return xstrdup(base);
    exts = os_getenv("PATHEXT");
    if (!exts || !exts[0]) {
        free(exts);
        exts = xstrdup(".COM;.EXE;.BAT;.CMD");
    }
    p = exts;
    for (;;) {
        char *semi = strchr(p, ';');
        size_t n = semi ? (size_t)(semi - p) : strlen(p);
        if (n) {
            size_t bl = strlen(base);
            char *cand = (char *)xmalloc(bl + n + 1);
            memcpy(cand, base, bl);
            memcpy(cand + bl, p, n);
            cand[bl + n] = '\0';
            if (os_is_file(cand)) {
                free(exts);
                return cand;
            }
            free(cand);
        }
        if (!semi)
            break;
        p = semi + 1;
    }
    free(exts);
    return NULL;
}

char *os_find_program(const char *name)
{
    char *path, *p, *found = NULL;
    if (!name || !name[0])
        return NULL;
    if (strchr(name, '/') || strchr(name, '\\') || (name[0] && name[1] == ':'))
        return osw_try_exts(name);
    path = os_getenv("PATH");
    if (!path)
        return NULL;
    p = path;
    for (;;) {
        char *semi = strchr(p, ';');
        size_t n = semi ? (size_t)(semi - p) : strlen(p);
        if (n) {
            char *dir = xstrndup(p, n);
            char *full;
            if (dir[0] == '"' && n > 1 && dir[n - 1] == '"') {
                memmove(dir, dir + 1, n - 2);
                dir[n - 2] = '\0';
            }
            full = os_path_join(dir, name);
            found = osw_try_exts(full);
            free(full);
            free(dir);
            if (found)
                break;
        }
        if (!semi)
            break;
        p = semi + 1;
    }
    free(path);
    return found;
}

/* ------------------------------------------------------------ time, tty --- */

uint64_t os_time_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    uint64_t q, r, f;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    f = (uint64_t)freq.QuadPart;
    q = (uint64_t)c.QuadPart / f;
    r = (uint64_t)c.QuadPart % f;
    return q * 1000000000u + r * 1000000000u / f;
}

int64_t os_unix_time(void)
{
    FILETIME ft;
    uint64_t t;
    GetSystemTimeAsFileTime(&ft);
    t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)((t - OSW_EPOCH_DIFF) / 10000000u);
}

void os_sleep_ms(uint32_t ms)
{
    Sleep(ms);
}

static HANDLE osw_std_handle(int fd)
{
    return GetStdHandle(fd == 0 ? STD_INPUT_HANDLE : fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
}

int os_isatty(int fd)
{
    DWORD mode;
    HANDLE h = osw_std_handle(fd);
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return 0;
    return GetConsoleMode(h, &mode) ? 1 : 0;
}

int os_color_enabled(int fd)
{
    char *nc = os_getenv("NO_COLOR");
    char *term = os_getenv("TERM");
    int off = (nc && nc[0]) || (term && strcmp(term, "dumb") == 0);
    DWORD mode;
    HANDLE h = osw_std_handle(fd);
    free(nc);
    free(term);
    if (off || h == NULL || h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode))
        return 0;
    if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)
        return 1;
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) ? 1 : 0;
}

int os_cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if (si.dwNumberOfProcessors < 1)
        return 1;
    if (si.dwNumberOfProcessors > 1024)
        return 1024;
    return (int)si.dwNumberOfProcessors;
}

void os_set_binary_stdio(void)
{
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
}

/* ------------------------------------------------------------ processes --- */

typedef struct OswBuf {
    char *data;
    size_t len, cap;
} OswBuf;

static void osw_buf_append(OswBuf *b, const char *p, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->len + n + 1)
            nc *= 2;
        b->data = (char *)xrealloc(b->data, nc);
        b->cap = nc;
    }
    if (n)
        memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void osw_buf_puts(OswBuf *b, const char *s)
{
    osw_buf_append(b, s, strlen(s));
}

struct OsProc {
    HANDLE hproc;
    int done;
    int exit_code;
    HANDLE hout;          /* temp file receiving stdout (and stderr if merged), or NULL */
    HANDLE herr;          /* temp file receiving stderr, or NULL */
    char *out;
    size_t out_len;
    char *err;
    size_t err_len;
};

/* A delete-on-close temporary file, not inheritable. */
static HANDLE osw_temp_file(void)
{
    wchar_t dir[MAX_PATH + 2], name[MAX_PATH + 2];
    DWORD n = GetTempPathW(MAX_PATH + 1, dir);
    if (n == 0 || n > MAX_PATH) {
        dir[0] = L'.';
        dir[1] = 0;
    }
    if (!GetTempFileNameW(dir, L"trn", 0, name))
        return INVALID_HANDLE_VALUE;
    return CreateFileW(name, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
}

/* Append argv element `arg` to a command line using the quoting rules of
 * CommandLineToArgvW / the Microsoft C runtime. */
static void osw_quote_arg(OswBuf *cmd, const char *arg)
{
    const char *p;
    if (arg[0] && !strpbrk(arg, " \t\n\v\"")) {
        osw_buf_puts(cmd, arg);
        return;
    }
    osw_buf_append(cmd, "\"", 1);
    for (p = arg;; p++) {
        size_t nbs = 0, i;
        while (*p == '\\') {
            p++;
            nbs++;
        }
        if (*p == '\0') {
            for (i = 0; i < nbs * 2; i++)
                osw_buf_append(cmd, "\\", 1);
            break;
        }
        if (*p == '"') {
            for (i = 0; i < nbs * 2 + 1; i++)
                osw_buf_append(cmd, "\\", 1);
            osw_buf_append(cmd, "\"", 1);
        } else {
            for (i = 0; i < nbs; i++)
                osw_buf_append(cmd, "\\", 1);
            osw_buf_append(cmd, p, 1);
        }
    }
    osw_buf_append(cmd, "\"", 1);
}

static HANDLE osw_dup_inheritable(HANDLE h)
{
    HANDLE d = NULL;
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, TRUE, DUPLICATE_SAME_ACCESS))
        return NULL;
    return d;
}

static char *osw_errmsg(const char *what, const char *arg)
{
    DWORD err = GetLastError();
    char sys[256];
    size_t n;
    char *m;
    DWORD k = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0,
                             sys, (DWORD)sizeof sys, NULL);
    if (k == 0)
        snprintf(sys, sizeof sys, "error %lu", (unsigned long)err);
    while (k > 0 && (sys[k - 1] == '\n' || sys[k - 1] == '\r' || sys[k - 1] == '.'))
        sys[--k] = '\0';
    n = strlen(what) + strlen(arg) + strlen(sys) + 8;
    m = (char *)xmalloc(n);
    snprintf(m, n, "%s %s: %s", what, arg, sys);
    return m;
}

OsProc *os_spawn(char *const argv[], const OsSpawnOpts *opts, char **err_out)
{
    OsSpawnOpts dflt;
    OswBuf cmd = {0};
    char *resolved;
    wchar_t *wcmd, *wcwd = NULL;
    HANDLE hin = NULL, hout = NULL, herr = NULL;          /* given to the child */
    HANDLE own_in = NULL, own_out = NULL, own_err = NULL; /* temp files we keep */
    HANDLE dup_in = NULL, dup_out = NULL, dup_err = NULL; /* duplicated std handles */
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    BOOL ok;
    OsProc *p;
    int i;
    if (err_out)
        *err_out = NULL;
    if (!opts) {
        memset(&dflt, 0, sizeof dflt);
        opts = &dflt;
    }
    if (!argv || !argv[0]) {
        if (err_out)
            *err_out = xstrdup("no program given");
        return NULL;
    }

    /* command line: resolved program path, then the arguments */
    resolved = os_find_program(argv[0]);
    osw_quote_arg(&cmd, resolved ? resolved : argv[0]);
    free(resolved);
    for (i = 1; argv[i]; i++) {
        osw_buf_append(&cmd, " ", 1);
        osw_quote_arg(&cmd, argv[i]);
    }

    /* stdin */
    if (opts->stdin_data) {
        own_in = osw_temp_file();
        if (own_in != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER zero;
            const char *d = opts->stdin_data;
            size_t left = opts->stdin_len;
            while (left) {
                DWORD put = 0, chunk = (DWORD)(left > 0x40000000u ? 0x40000000u : left);
                if (!WriteFile(own_in, d, chunk, &put, NULL))
                    break;
                d += put;
                left -= put;
            }
            zero.QuadPart = 0;
            SetFilePointerEx(own_in, zero, NULL, FILE_BEGIN);
        }
        hin = own_in;
    } else if (opts->stdin_null) {
        own_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, 0, NULL);
        hin = own_in;
    } else {
        hin = dup_in = osw_dup_inheritable(GetStdHandle(STD_INPUT_HANDLE));
    }
    /* stdout */
    if (opts->capture_stdout) {
        own_out = osw_temp_file();
        hout = own_out;
    } else {
        hout = dup_out = osw_dup_inheritable(GetStdHandle(STD_OUTPUT_HANDLE));
    }
    /* stderr */
    if (opts->merge_stderr) {
        herr = hout;
    } else if (opts->capture_stderr) {
        own_err = osw_temp_file();
        herr = own_err;
    } else {
        herr = dup_err = osw_dup_inheritable(GetStdHandle(STD_ERROR_HANDLE));
    }
    if (own_in == INVALID_HANDLE_VALUE || own_out == INVALID_HANDLE_VALUE ||
        own_err == INVALID_HANDLE_VALUE) {
        if (err_out)
            *err_out = osw_errmsg("cannot create temporary file for", argv[0]);
        ok = FALSE;
        goto cleanup_fail;
    }
    /* make our own handles inheritable only for the duration of the call
     * (single-threaded, so no other child can pick them up) */
    if (own_in)
        SetHandleInformation(own_in, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (own_out)
        SetHandleInformation(own_out, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    if (own_err)
        SetHandleInformation(own_err, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hin;
    si.hStdOutput = hout;
    si.hStdError = herr;
    memset(&pi, 0, sizeof pi);
    wcmd = osw_wide(cmd.data ? cmd.data : "");
    if (opts->cwd)
        wcwd = osw_path(opts->cwd);
    fflush(stdout);
    fflush(stderr);
    ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, wcwd, &si, &pi);
    if (!ok && err_out)
        *err_out = osw_errmsg("cannot run", argv[0]);
    free(wcmd);
    free(wcwd);
    if (own_in)
        SetHandleInformation(own_in, HANDLE_FLAG_INHERIT, 0);
    if (own_out)
        SetHandleInformation(own_out, HANDLE_FLAG_INHERIT, 0);
    if (own_err)
        SetHandleInformation(own_err, HANDLE_FLAG_INHERIT, 0);

cleanup_fail:
    free(cmd.data);
    if (dup_in)
        CloseHandle(dup_in);
    if (dup_out)
        CloseHandle(dup_out);
    if (dup_err)
        CloseHandle(dup_err);
    if (own_in && own_in != INVALID_HANDLE_VALUE)
        CloseHandle(own_in); /* the child has its own copy */
    if (!ok) {
        if (own_out && own_out != INVALID_HANDLE_VALUE)
            CloseHandle(own_out);
        if (own_err && own_err != INVALID_HANDLE_VALUE)
            CloseHandle(own_err);
        return NULL;
    }
    CloseHandle(pi.hThread);
    p = (OsProc *)xcalloc(1, sizeof(OsProc));
    p->hproc = pi.hProcess;
    p->hout = own_out;
    p->herr = own_err;
    return p;
}

static void osw_slurp(HANDLE *h, char **data, size_t *len)
{
    LARGE_INTEGER zero;
    if (!*h)
        return;
    zero.QuadPart = 0;
    SetFilePointerEx(*h, zero, NULL, FILE_BEGIN);
    if (osw_read_handle(*h, data, len) != 0) {
        *data = NULL;
        *len = 0;
    }
    CloseHandle(*h);
    *h = NULL;
}

static void osw_finish(OsProc *p)
{
    DWORD code = 255;
    if (p->done)
        return;
    GetExitCodeProcess(p->hproc, &code);
    p->exit_code = (int)code;
    osw_slurp(&p->hout, &p->out, &p->out_len);
    osw_slurp(&p->herr, &p->err, &p->err_len);
    p->done = 1;
}

int os_proc_wait_any(OsProc **procs, size_t n, int timeout_ms)
{
    HANDLE hs[MAXIMUM_WAIT_OBJECTS];
    size_t idx[MAXIMUM_WAIT_OBJECTS];
    uint64_t deadline = timeout_ms >= 0 ? os_time_ns() + (uint64_t)timeout_ms * 1000000u : 0;
    for (;;) {
        size_t i, alive = 0, start;
        for (i = 0; i < n; i++) {
            if (procs[i] && procs[i]->done)
                return (int)i;
            if (procs[i])
                alive++;
        }
        if (!alive)
            return -1;
        /* WaitForMultipleObjects takes at most 64 handles: with more
         * children, poll the groups with short waits. */
        for (start = 0; start < n;) {
            DWORD cnt = 0, wait_ms, r;
            while (start < n && cnt < MAXIMUM_WAIT_OBJECTS) {
                if (procs[start]) {
                    hs[cnt] = procs[start]->hproc;
                    idx[cnt] = start;
                    cnt++;
                }
                start++;
            }
            if (!cnt)
                break;
            if (alive <= MAXIMUM_WAIT_OBJECTS) {
                if (timeout_ms < 0) {
                    wait_ms = INFINITE;
                } else {
                    uint64_t now = os_time_ns();
                    wait_ms = now >= deadline ? 0 : (DWORD)((deadline - now) / 1000000u) + 1;
                }
            } else {
                wait_ms = 2;
            }
            r = WaitForMultipleObjects(cnt, hs, FALSE, wait_ms);
            if (r < WAIT_OBJECT_0 + cnt) {
                OsProc *p = procs[idx[r - WAIT_OBJECT_0]];
                osw_finish(p);
                return (int)idx[r - WAIT_OBJECT_0];
            }
            if (r == WAIT_FAILED)
                return -1;
        }
        if (timeout_ms >= 0 && os_time_ns() >= deadline)
            return -1;
    }
}

int os_proc_done(const OsProc *p)
{
    return p->done;
}

int os_proc_exit_code(const OsProc *p)
{
    return p->exit_code;
}

const char *os_proc_stdout(const OsProc *p, size_t *len)
{
    if (len)
        *len = p->out_len;
    return p->out ? p->out : "";
}

const char *os_proc_stderr(const OsProc *p, size_t *len)
{
    if (len)
        *len = p->err_len;
    return p->err ? p->err : "";
}

void os_proc_kill(OsProc *p)
{
    if (!p->done)
        TerminateProcess(p->hproc, 137);
}

void os_proc_free(OsProc *p)
{
    if (!p)
        return;
    if (!p->done) {
        TerminateProcess(p->hproc, 137);
        WaitForSingleObject(p->hproc, INFINITE);
    }
    if (p->hout)
        CloseHandle(p->hout);
    if (p->herr)
        CloseHandle(p->herr);
    CloseHandle(p->hproc);
    free(p->out);
    free(p->err);
    free(p);
}

#undef OSW_EPOCH_DIFF

/* windows.h defines macros with ordinary names. In the unity build every
 * later file would see them, so remove the ones likely to collide. */
#undef near
#undef far
#undef IN
#undef OUT
#undef OPTIONAL
#undef CONST
#undef ERROR
#undef DELETE
#undef interface
#undef small
#undef hyper
#undef Yield
#undef ABSOLUTE
#undef RELATIVE
#undef TRANSPARENT
#undef OPAQUE

#else
/* keep the translation unit non-empty on other platforms */
typedef int os_win32_empty_tu;
#endif /* _WIN32 */
