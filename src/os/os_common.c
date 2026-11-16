/* os_common.c — the portable half of the platform layer: path manipulation
 * and helpers built on the primitives in os_posix.c / os_win32.c. Includes
 * no OS headers. */
#include "os.h"

#ifdef _WIN32
#define OS_IS_SEP(c) ((c) == '/' || (c) == '\\')
#else
#define OS_IS_SEP(c) ((c) == '/')
#endif

/* ---------------------------------------------------------------- files --- */

int os_write_file_if_changed(const char *path, const void *data, size_t len)
{
    char *old = NULL;
    size_t old_len = 0;
    if (os_read_file(path, &old, &old_len) == 0) {
        int same = old_len == len && (len == 0 || memcmp(old, data, len) == 0);
        free(old);
        if (same)
            return 0;
    }
    return os_write_file(path, data, len) == 0 ? 1 : -1;
}

int os_copy_file(const char *from, const char *to)
{
    char *data;
    size_t len;
    int rc;
    if (os_read_file(from, &data, &len) != 0)
        return -1;
    rc = os_write_file(to, data, len);
    free(data);
    return rc;
}

int os_exists(const char *path)
{
    OsStat st;
    return os_stat(path, &st) == 0;
}

int os_is_dir(const char *path)
{
    OsStat st;
    return os_stat(path, &st) == 0 && st.is_dir;
}

int os_is_file(const char *path)
{
    OsStat st;
    return os_stat(path, &st) == 0 && st.is_file;
}

int os_remove_all(const char *path)
{
    OsStat st;
    OsDirList list;
    size_t i;
    int rc = 0;
    if (os_stat(path, &st) != 0) {
        os_remove(path); /* absent, or a dangling symbolic link */
        return 0;
    }
    if (!st.is_dir || st.is_link)
        return os_remove(path);
    if (os_list_dir(path, &list) != 0)
        return -1;
    for (i = 0; i < list.len; i++) {
        char *child = os_path_join(path, list.items[i].name);
        if (list.items[i].is_dir && !list.items[i].is_link)
            rc |= os_remove_all(child);
        else
            rc |= os_remove(child);
        free(child);
    }
    os_dir_list_free(&list);
    if (os_remove(path) != 0)
        rc = -1;
    return rc ? -1 : 0;
}

int os_mkdir_p(const char *path)
{
    char *p = os_path_normalize(path);
    size_t i, start = 0;
    int rc = 0;
    if (os_is_dir(p)) {
        free(p);
        return 0;
    }
    /* skip the root / drive / UNC prefix */
    if (p[0] == '/')
        start = 1;
#ifdef _WIN32
    if (p[0] && p[1] == ':')
        start = (p[2] == '/') ? 3 : 2;
    if (p[0] == '/' && p[1] == '/') {
        /* //server/share/... : skip server and share */
        int seps = 0;
        for (start = 2; p[start]; start++) {
            if (p[start] == '/' && ++seps == 2)
                break;
        }
    }
#endif
    for (i = start; ; i++) {
        if (p[i] == '/' || p[i] == '\0') {
            char saved = p[i];
            p[i] = '\0';
            if (i > start && os_mkdir(p) != 0)
                rc = -1;
            p[i] = saved;
            if (saved == '\0')
                break;
            if (rc)
                break;
        }
    }
    free(p);
    return rc;
}

void os_dir_list_free(OsDirList *list)
{
    size_t i;
    for (i = 0; i < list->len; i++)
        free(list->items[i].name);
    free(list->items);
    list->items = NULL;
    list->len = 0;
}

/* ---------------------------------------------------------------- paths --- */

int os_path_is_abs(const char *path)
{
    if (!path || !path[0])
        return 0;
    if (OS_IS_SEP(path[0]))
        return 1;
#ifdef _WIN32
    if (tern_is_alpha((unsigned char)path[0]) && path[1] == ':' && OS_IS_SEP(path[2]))
        return 1;
#endif
    return 0;
}

char *os_path_join(const char *a, const char *b)
{
    size_t la, lb;
    char *r;
    int need_sep;
    if (!a || !a[0] || os_path_is_abs(b))
        return xstrdup(b);
    la = strlen(a);
    lb = strlen(b);
    need_sep = !OS_IS_SEP(a[la - 1]);
    r = (char *)xmalloc(la + lb + 2);
    memcpy(r, a, la);
    if (need_sep)
        r[la++] = '/';
    memcpy(r + la, b, lb + 1);
    return r;
}

char *os_path_normalize(const char *path)
{
    size_t n = strlen(path), i, plen = 0, ncomp = 0, out = 0;
    char *buf = xstrdup(path);
    char *res;
    char **comps;
    int absolute = 0;
    for (i = 0; i < n; i++) {
        if (buf[i] == '\\')
            buf[i] = '/';
    }
    /* prefix: "/" , "C:" , "C:/" , "//" (UNC, Windows only) */
#ifdef _WIN32
    if (tern_is_alpha((unsigned char)buf[0]) && buf[1] == ':') {
        plen = 2;
        if (buf[2] == '/') {
            plen = 3;
            absolute = 1;
        }
    } else if (buf[0] == '/' && buf[1] == '/' && buf[2] != '/') {
        plen = 2;
        absolute = 1;
    } else
#endif
    if (buf[0] == '/') {
        plen = 1;
        absolute = 1;
    }
    comps = (char **)xmalloc((n / 2 + 2) * sizeof(char *));
    i = plen;
    while (i < n) {
        size_t s;
        while (i < n && buf[i] == '/')
            i++;
        if (i >= n)
            break;
        s = i;
        while (i < n && buf[i] != '/')
            i++;
        buf[i] = '\0';
        i++;
        if (strcmp(buf + s, ".") == 0)
            continue;
        if (strcmp(buf + s, "..") == 0) {
            if (ncomp > 0 && strcmp(comps[ncomp - 1], "..") != 0) {
                ncomp--;
                continue;
            }
            if (absolute)
                continue;
        }
        comps[ncomp++] = buf + s;
    }
    res = (char *)xmalloc(n + 3);
    memcpy(res, path, plen);
    for (i = 0; i < plen; i++) {
        if (res[i] == '\\')
            res[i] = '/';
    }
    out = plen;
    for (i = 0; i < ncomp; i++) {
        size_t l = strlen(comps[i]);
        if (i > 0)
            res[out++] = '/';
        memcpy(res + out, comps[i], l);
        out += l;
    }
    if (out == 0)
        res[out++] = '.';
    res[out] = '\0';
    free(comps);
    free(buf);
    return res;
}

char *os_path_abs(const char *path)
{
    char *joined, *res;
    if (os_path_is_abs(path))
        return os_path_normalize(path);
    {
        char *cwd = os_getcwd();
        if (!cwd)
            return os_path_normalize(path);
        joined = os_path_join(cwd, path);
        free(cwd);
    }
    res = os_path_normalize(joined);
    free(joined);
    return res;
}

const char *os_path_basename(const char *path)
{
    const char *b = path, *p;
    for (p = path; *p; p++) {
        if (OS_IS_SEP(*p))
            b = p + 1;
    }
    return b;
}

char *os_path_dirname(const char *path)
{
    size_t n = strlen(path);
    while (n > 1 && OS_IS_SEP(path[n - 1]))
        n--; /* trailing separators */
    while (n > 0 && !OS_IS_SEP(path[n - 1]))
        n--; /* last component */
    if (n == 0)
        return xstrdup(".");
    while (n > 1 && OS_IS_SEP(path[n - 1]))
        n--; /* separators before it */
#ifdef _WIN32
    if (n == 2 && path[1] == ':')
        n = 3; /* keep "C:/" */
#endif
    return xstrndup(path, n);
}

const char *os_path_ext(const char *path)
{
    const char *base = os_path_basename(path);
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base)
        return base + strlen(base);
    return dot;
}

char *os_path_relative(const char *path, const char *base)
{
    char *p = os_path_abs(path);
    char *b = os_path_abs(base);
    size_t bl = strlen(b);
    char *res;
    if (strcmp(p, b) == 0) {
        res = xstrdup(".");
    } else if (strncmp(p, b, bl) == 0 && (p[bl] == '/' || (bl > 0 && b[bl - 1] == '/'))) {
        res = xstrdup(p + bl + (p[bl] == '/' ? 1 : 0));
    } else {
        res = xstrdup(p);
    }
    free(p);
    free(b);
    return res;
}

/* ------------------------------------------------------------ processes --- */

static char *os_argv0_saved;

void os_set_argv0(const char *argv0)
{
    free(os_argv0_saved);
    os_argv0_saved = argv0 ? xstrdup(argv0) : NULL;
}

const char *os_get_argv0(void)
{
    return os_argv0_saved;
}

int os_proc_wait(OsProc *p)
{
    OsProc *one[1];
    one[0] = p;
    while (!os_proc_done(p)) {
        if (os_proc_wait_any(one, 1, -1) < 0 && !os_proc_done(p))
            return -1;
    }
    return 0;
}

int os_run(char *const argv[], const OsSpawnOpts *opts, OsRunResult *res)
{
    OsProc *p;
    const char *s;
    size_t n;
    memset(res, 0, sizeof *res);
    p = os_spawn(argv, opts, NULL);
    if (!p) {
        res->exit_code = -1;
        return -1;
    }
    os_proc_wait(p);
    res->exit_code = os_proc_exit_code(p);
    if (opts && opts->capture_stdout) {
        s = os_proc_stdout(p, &n);
        res->out = xstrndup(s, n);
        res->out_len = n;
    }
    if (opts && opts->capture_stderr && !opts->merge_stderr) {
        s = os_proc_stderr(p, &n);
        res->err = xstrndup(s, n);
        res->err_len = n;
    }
    os_proc_free(p);
    return res->exit_code;
}

void os_run_result_free(OsRunResult *res)
{
    free(res->out);
    free(res->err);
    res->out = res->err = NULL;
    res->out_len = res->err_len = 0;
}

#undef OS_IS_SEP
