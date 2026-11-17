/* os_posix.c — platform layer for Linux, macOS and other POSIX systems.
 *
 * The feature-test macros must precede every system header, so this file
 * is the first one included by src/unity.c. */
#ifndef _WIN32

#if defined(__APPLE__)
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#else
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#endif

#include "os.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

/* ---------------------------------------------------------------- files --- */

int os_read_file(const char *path, char **data, size_t *len)
{
    int fd;
    char *buf;
    size_t cap = 0, n = 0;
    struct stat st;
    *data = NULL;
    if (len)
        *len = 0;
    do {
        fd = open(path, O_RDONLY);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        close(fd);
        errno = EISDIR;
        return -1;
    }
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
        cap = (size_t)st.st_size + 1;
    if (cap < 4096)
        cap = 4096;
    buf = (char *)xmalloc(cap);
    for (;;) {
        ssize_t r;
        if (n + 1 >= cap) {
            cap *= 2;
            buf = (char *)xrealloc(buf, cap);
        }
        r = read(fd, buf + n, cap - n - 1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return -1;
        }
        if (r == 0)
            break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = '\0';
    *data = buf;
    if (len)
        *len = n;
    return 0;
}

static int osp_write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int os_write_file(const char *path, const void *data, size_t len)
{
    int fd, rc;
    do {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return -1;
    rc = osp_write_all(fd, (const char *)data, len);
    if (close(fd) != 0)
        rc = -1;
    return rc;
}

int os_rename(const char *from, const char *to)
{
    return rename(from, to) == 0 ? 0 : -1;
}

int os_remove(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    if (S_ISDIR(st.st_mode))
        return rmdir(path) == 0 ? 0 : -1;
    return unlink(path) == 0 ? 0 : -1;
}

int os_mkdir(const char *path)
{
    if (mkdir(path, 0777) == 0)
        return 0;
    if (errno == EEXIST && os_is_dir(path))
        return 0;
    return -1;
}

static int64_t osp_mtime_ns(const struct stat *st)
{
#if defined(__APPLE__)
    return (int64_t)st->st_mtimespec.tv_sec * 1000000000 + (int64_t)st->st_mtimespec.tv_nsec;
#else
    return (int64_t)st->st_mtim.tv_sec * 1000000000 + (int64_t)st->st_mtim.tv_nsec;
#endif
}

int os_stat(const char *path, OsStat *out)
{
    struct stat st, lst;
    memset(out, 0, sizeof *out);
    if (lstat(path, &lst) != 0)
        return -1;
    if (S_ISLNK(lst.st_mode)) {
        out->is_link = 1;
        if (stat(path, &st) != 0)
            return -1; /* dangling */
    } else {
        st = lst;
    }
    out->exists = 1;
    out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out->is_file = S_ISREG(st.st_mode) ? 1 : 0;
    out->size = (uint64_t)st.st_size;
    out->mtime_ns = osp_mtime_ns(&st);
    return 0;
}

static int osp_entry_cmp(const void *a, const void *b)
{
    return strcmp(((const OsDirEntry *)a)->name, ((const OsDirEntry *)b)->name);
}

int os_list_dir(const char *path, OsDirList *out)
{
    DIR *d;
    struct dirent *e;
    size_t cap = 0;
    out->items = NULL;
    out->len = 0;
    d = opendir(path);
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        char *full;
        OsStat st;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (out->len == cap) {
            cap = cap ? cap * 2 : 16;
            out->items = (OsDirEntry *)xrealloc(out->items, cap * sizeof(OsDirEntry));
        }
        full = os_path_join(path, e->d_name);
        os_stat(full, &st);
        if (!st.exists) {
            /* dangling link: still list it */
            struct stat lst;
            st.is_link = lstat(full, &lst) == 0 && S_ISLNK(lst.st_mode);
        }
        free(full);
        out->items[out->len].name = xstrdup(e->d_name);
        out->items[out->len].is_dir = st.is_dir;
        out->items[out->len].is_link = st.is_link;
        out->len++;
    }
    closedir(d);
    if (out->len > 1)
        qsort(out->items, out->len, sizeof(OsDirEntry), osp_entry_cmp);
    return 0;
}

/* ---------------------------------------------------------------- paths --- */

char *os_getcwd(void)
{
    size_t cap = 256;
    for (;;) {
        char *buf = (char *)xmalloc(cap);
        if (getcwd(buf, cap))
            return buf;
        free(buf);
        if (errno != ERANGE)
            return NULL;
        cap *= 2;
    }
}

int os_chdir(const char *path)
{
    return chdir(path) == 0 ? 0 : -1;
}

char *os_home_dir(void)
{
    return os_getenv("HOME");
}

char *os_temp_dir(void)
{
    char *t = os_getenv("TMPDIR");
    if (t && t[0])
        return t;
    free(t);
    return xstrdup("/tmp");
}

static char *osp_realpath(const char *p)
{
    char *r = realpath(p, NULL);
    char *res;
    if (!r)
        return NULL;
    res = xstrdup(r);
    free(r);
    return res;
}

char *os_exe_path(void)
{
    const char *argv0;
#if defined(__APPLE__)
    {
        uint32_t size = 0;
        char *buf, *r;
        _NSGetExecutablePath(NULL, &size);
        buf = (char *)xmalloc(size + 1);
        if (_NSGetExecutablePath(buf, &size) == 0) {
            r = osp_realpath(buf);
            if (r) {
                free(buf);
                return r;
            }
            return buf;
        }
        free(buf);
    }
#elif defined(__linux__)
    {
        size_t cap = 256;
        for (;;) {
            char *buf = (char *)xmalloc(cap);
            ssize_t n = readlink("/proc/self/exe", buf, cap);
            if (n < 0) {
                free(buf);
                break;
            }
            if ((size_t)n < cap) {
                buf[n] = '\0';
                return buf;
            }
            free(buf);
            cap *= 2;
        }
    }
#endif
    argv0 = os_get_argv0();
    if (!argv0)
        return NULL;
    if (strchr(argv0, '/'))
        return osp_realpath(argv0);
    {
        char *found = os_find_program(argv0);
        char *r;
        if (!found)
            return NULL;
        r = osp_realpath(found);
        free(found);
        return r;
    }
}

/* ----------------------------------------------------------- environment --- */

char *os_getenv(const char *name)
{
    const char *v = getenv(name);
    return v ? xstrdup(v) : NULL;
}

int os_setenv(const char *name, const char *value)
{
    if (!value)
        return unsetenv(name) == 0 ? 0 : -1;
    return setenv(name, value, 1) == 0 ? 0 : -1;
}

char *os_find_program(const char *name)
{
    const char *path, *p;
    if (!name || !name[0])
        return NULL;
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0 && os_is_file(name))
            return xstrdup(name);
        return NULL;
    }
    path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    p = path;
    for (;;) {
        const char *colon = strchr(p, ':');
        size_t n = colon ? (size_t)(colon - p) : strlen(p);
        char *dir = n ? xstrndup(p, n) : xstrdup(".");
        char *full = os_path_join(dir, name);
        free(dir);
        if (access(full, X_OK) == 0 && os_is_file(full))
            return full;
        free(full);
        if (!colon)
            break;
        p = colon + 1;
    }
    return NULL;
}

/* ------------------------------------------------------------ time, tty --- */

uint64_t os_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

int64_t os_unix_time(void)
{
    return (int64_t)time(NULL);
}

void os_sleep_ms(uint32_t ms)
{
    struct timespec ts, rem;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &rem) != 0 && errno == EINTR)
        ts = rem;
}

int os_isatty(int fd)
{
    return isatty(fd) ? 1 : 0;
}

int os_color_enabled(int fd)
{
    const char *term = getenv("TERM");
    const char *nc = getenv("NO_COLOR");
    if (nc && nc[0])
        return 0;
    if (term && strcmp(term, "dumb") == 0)
        return 0;
    return os_isatty(fd);
}

int os_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1)
        return 1;
    if (n > 1024)
        return 1024;
    return (int)n;
}

void os_set_binary_stdio(void)
{
}

long os_read_stdin(char *buf, size_t n)
{
    ssize_t r;
    do {
        r = read(0, buf, n);
    } while (r < 0 && errno == EINTR);
    return r < 0 ? -1 : (long)r;
}

int os_stdin_wait(int timeout_ms)
{
    struct pollfd pfd;
    int r;
    pfd.fd = 0;
    pfd.events = POLLIN;
    pfd.revents = 0;
    do {
        r = poll(&pfd, 1, timeout_ms < 0 ? -1 : timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return -1;
    return r > 0 ? 1 : 0;
}

/* ------------------------------------------------------------ processes --- */

typedef struct OspBuf {
    char *data;
    size_t len, cap;
} OspBuf;

static void osp_buf_append(OspBuf *b, const char *p, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
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

struct OsProc {
    pid_t pid;
    int done;
    int exit_code;
    int fd_in, fd_out, fd_err;   /* parent-side pipe ends, -1 if closed/unused */
    char *in_data;
    size_t in_len, in_off;
    OspBuf out, err;
    int interactive;             /* fd_in / fd_out are driven by os_proc_write / read_line */
    size_t out_off;              /* interactive: consumed bytes of out */
};

static void osp_close(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void osp_cloexec(int fd)
{
    int fl = fcntl(fd, F_GETFD);
    if (fl >= 0)
        fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

static int osp_pipe(int p[2])
{
    if (pipe(p) != 0) {
        p[0] = p[1] = -1;
        return -1;
    }
    osp_cloexec(p[0]);
    osp_cloexec(p[1]);
    return 0;
}

static void osp_set_status(OsProc *p, int status)
{
    p->done = 1;
    if (WIFEXITED(status))
        p->exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        p->exit_code = 128 + WTERMSIG(status);
    else
        p->exit_code = 255;
}

/* Non-blocking reap. Returns 1 if the process has finished. */
static int osp_try_reap(OsProc *p)
{
    int status;
    pid_t r;
    if (p->done)
        return 1;
    do {
        r = waitpid(p->pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r == p->pid) {
        osp_set_status(p, status);
        return 1;
    }
    if (r < 0) {
        /* not our child any more (should not happen): treat as finished */
        p->done = 1;
        p->exit_code = 255;
        return 1;
    }
    return 0;
}

static void osp_child_fail(int fd, int stage)
{
    int msg[2];
    msg[0] = stage;
    msg[1] = errno;
    if (write(fd, msg, sizeof msg) < 0) {
        /* nothing more we can do */
    }
    _exit(127);
}

static char *osp_errmsg(const char *what, const char *arg, int err)
{
    const char *es = strerror(err);
    size_t n = strlen(what) + strlen(arg) + strlen(es) + 8;
    char *m = (char *)xmalloc(n);
    snprintf(m, n, "%s %s: %s", what, arg, es);
    return m;
}

OsProc *os_spawn(char *const argv[], const OsSpawnOpts *opts, char **err_out)
{
    static int sigpipe_ignored;
    OsSpawnOpts dflt;
    int pin[2] = {-1, -1}, pout[2] = {-1, -1}, perr[2] = {-1, -1}, pexec[2] = {-1, -1};
    int msg[2];
    ssize_t got;
    pid_t pid;
    OsProc *p;
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
    if (!sigpipe_ignored) {
        /* a child closing its stdin early must not kill us */
        signal(SIGPIPE, SIG_IGN);
        sigpipe_ignored = 1;
    }
    if (((opts->stdin_data || opts->interactive) && osp_pipe(pin) != 0) ||
        ((opts->capture_stdout || opts->interactive) && osp_pipe(pout) != 0) ||
        (opts->capture_stderr && !opts->merge_stderr && osp_pipe(perr) != 0) ||
        osp_pipe(pexec) != 0) {
        if (err_out)
            *err_out = osp_errmsg("cannot create pipe for", argv[0], errno);
        osp_close(&pin[0]); osp_close(&pin[1]);
        osp_close(&pout[0]); osp_close(&pout[1]);
        osp_close(&perr[0]); osp_close(&perr[1]);
        osp_close(&pexec[0]); osp_close(&pexec[1]);
        return NULL;
    }
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid < 0) {
        if (err_out)
            *err_out = osp_errmsg("cannot fork for", argv[0], errno);
        osp_close(&pin[0]); osp_close(&pin[1]);
        osp_close(&pout[0]); osp_close(&pout[1]);
        osp_close(&perr[0]); osp_close(&perr[1]);
        osp_close(&pexec[0]); osp_close(&pexec[1]);
        return NULL;
    }
    if (pid == 0) {
        /* child: only async-signal-safe calls from here on */
        if (pin[0] >= 0) {
            if (dup2(pin[0], 0) < 0)
                osp_child_fail(pexec[1], 0);
        } else if (opts->stdin_null) {
            int nfd = open("/dev/null", O_RDONLY);
            if (nfd >= 0 && nfd != 0) {
                dup2(nfd, 0);
                close(nfd);
            }
        }
        if (pout[1] >= 0 && dup2(pout[1], 1) < 0)
            osp_child_fail(pexec[1], 0);
        if (opts->merge_stderr) {
            if (dup2(1, 2) < 0)
                osp_child_fail(pexec[1], 0);
        } else if (perr[1] >= 0 && dup2(perr[1], 2) < 0) {
            osp_child_fail(pexec[1], 0);
        }
        if (opts->cwd && chdir(opts->cwd) != 0)
            osp_child_fail(pexec[1], 1);
        execvp(argv[0], argv);
        osp_child_fail(pexec[1], 2);
    }
    /* parent */
    osp_close(&pin[0]);
    osp_close(&pout[1]);
    osp_close(&perr[1]);
    osp_close(&pexec[1]);
    do {
        got = read(pexec[0], msg, sizeof msg);
    } while (got < 0 && errno == EINTR);
    osp_close(&pexec[0]);
    if (got == (ssize_t)sizeof msg) {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if (err_out) {
            if (msg[0] == 1)
                *err_out = osp_errmsg("cannot change directory to", opts->cwd, msg[1]);
            else if (msg[0] == 2)
                *err_out = osp_errmsg("cannot run", argv[0], msg[1]);
            else
                *err_out = osp_errmsg("cannot set up I/O for", argv[0], msg[1]);
        }
        osp_close(&pin[1]);
        osp_close(&pout[0]);
        osp_close(&perr[0]);
        return NULL;
    }
    p = (OsProc *)xcalloc(1, sizeof(OsProc));
    p->pid = pid;
    p->fd_in = pin[1];
    p->fd_out = pout[0];
    p->fd_err = perr[0];
    p->interactive = opts->interactive;
    if (p->fd_in >= 0 && !p->interactive) {
        if (opts->stdin_len) {
            int fl = fcntl(p->fd_in, F_GETFL);
            if (fl >= 0)
                fcntl(p->fd_in, F_SETFL, fl | O_NONBLOCK);
            p->in_data = (char *)xmemdup(opts->stdin_data, opts->stdin_len);
            p->in_len = opts->stdin_len;
        } else {
            osp_close(&p->fd_in);
        }
    }
    return p;
}

static int osp_has_fds(const OsProc *p)
{
    return p->fd_in >= 0 || p->fd_out >= 0 || p->fd_err >= 0;
}

static void osp_read_into(int *fd, OspBuf *b)
{
    char buf[65536];
    ssize_t r;
    do {
        r = read(*fd, buf, sizeof buf);
    } while (r < 0 && errno == EINTR);
    if (r > 0)
        osp_buf_append(b, buf, (size_t)r);
    else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        osp_close(fd);
}

static void osp_write_some(OsProc *p)
{
    size_t chunk = p->in_len - p->in_off;
    ssize_t w;
    if (chunk > 65536)
        chunk = 65536;
    do {
        w = write(p->fd_in, p->in_data + p->in_off, chunk);
    } while (w < 0 && errno == EINTR);
    if (w > 0)
        p->in_off += (size_t)w;
    else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        p->in_off = p->in_len; /* EPIPE: child does not want more */
    if (p->in_off >= p->in_len)
        osp_close(&p->fd_in);
}

int os_proc_wait_any(OsProc **procs, size_t n, int timeout_ms)
{
    uint64_t deadline = timeout_ms >= 0 ? os_time_ns() + (uint64_t)timeout_ms * 1000000u : 0;
    struct pollfd *pfds = NULL;
    size_t *owner = NULL;
    int *which = NULL;
    size_t cap = 0;
    int result = -1;
    for (;;) {
        size_t i, nfds = 0;
        int alive = 0, need_tick = 0, pt, rc;
        for (i = 0; i < n; i++) {
            if (procs[i] && procs[i]->done) {
                result = (int)i;
                goto out;
            }
        }
        for (i = 0; i < n; i++) {
            OsProc *p = procs[i];
            if (!p)
                continue;
            if (!osp_has_fds(p)) {
                if (osp_try_reap(p)) {
                    result = (int)i;
                    goto out;
                }
                need_tick = 1;
            }
            alive++;
        }
        if (!alive)
            goto out;
        if (cap < 3 * n) {
            cap = 3 * n;
            pfds = (struct pollfd *)xrealloc(pfds, cap * sizeof(struct pollfd));
            owner = (size_t *)xrealloc(owner, cap * sizeof(size_t));
            which = (int *)xrealloc(which, cap * sizeof(int));
        }
        for (i = 0; i < n; i++) {
            OsProc *p = procs[i];
            if (!p)
                continue;
            if (p->fd_in >= 0) {
                pfds[nfds].fd = p->fd_in;
                pfds[nfds].events = POLLOUT;
                pfds[nfds].revents = 0;
                owner[nfds] = i;
                which[nfds++] = 0;
            }
            if (p->fd_out >= 0) {
                pfds[nfds].fd = p->fd_out;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                owner[nfds] = i;
                which[nfds++] = 1;
            }
            if (p->fd_err >= 0) {
                pfds[nfds].fd = p->fd_err;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                owner[nfds] = i;
                which[nfds++] = 2;
            }
        }
        /* poll timeout: short ticks while some child has no pipes left
         * (we must poll waitpid), else until the deadline */
        pt = -1;
        if (need_tick)
            pt = 5;
        if (timeout_ms >= 0) {
            uint64_t now = os_time_ns();
            int rem = now >= deadline ? 0 : (int)((deadline - now) / 1000000u) + 1;
            if (pt < 0 || rem < pt)
                pt = rem;
        }
        rc = poll(pfds, (nfds_t)nfds, pt);
        if (rc < 0 && errno != EINTR) {
            result = -1;
            goto out;
        }
        if (rc > 0) {
            for (i = 0; i < nfds; i++) {
                OsProc *p = procs[owner[i]];
                short re = pfds[i].revents;
                if (!re)
                    continue;
                if (which[i] == 0) {
                    if (re & (POLLERR | POLLHUP | POLLNVAL))
                        osp_close(&p->fd_in);
                    else if (re & POLLOUT)
                        osp_write_some(p);
                } else if (which[i] == 1) {
                    osp_read_into(&p->fd_out, &p->out);
                } else {
                    osp_read_into(&p->fd_err, &p->err);
                }
            }
        }
        if (timeout_ms >= 0 && os_time_ns() >= deadline) {
            /* one last non-blocking reap before giving up */
            for (i = 0; i < n; i++) {
                if (procs[i] && !osp_has_fds(procs[i]) && osp_try_reap(procs[i])) {
                    result = (int)i;
                    goto out;
                }
            }
            result = -1;
            goto out;
        }
    }
out:
    free(pfds);
    free(owner);
    free(which);
    return result;
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
        *len = p->out.len;
    return p->out.data ? p->out.data : "";
}

const char *os_proc_stderr(const OsProc *p, size_t *len)
{
    if (len)
        *len = p->err.len;
    return p->err.data ? p->err.data : "";
}

void os_proc_kill(OsProc *p)
{
    if (!p->done)
        kill(p->pid, SIGKILL);
}

int os_proc_write(OsProc *p, const char *data, size_t len)
{
    size_t off = 0;
    if (!p || p->fd_in < 0)
        return -1;
    while (off < len) {
        ssize_t w = write(p->fd_in, data + off, len - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            osp_close(&p->fd_in);
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

char *os_proc_read_line(OsProc *p, int timeout_ms)
{
    uint64_t deadline = timeout_ms >= 0 ? os_time_ns() + (uint64_t)timeout_ms * 1000000u : 0;
    if (!p)
        return NULL;
    for (;;) {
        char *nl = p->out.data && p->out.len > p->out_off
                       ? (char *)memchr(p->out.data + p->out_off, '\n', p->out.len - p->out_off)
                       : NULL;
        struct pollfd pf;
        int pt = -1, rc;
        if (nl || (p->fd_out < 0 && p->out.len > p->out_off)) {
            size_t end = nl ? (size_t)(nl - p->out.data) : p->out.len;
            size_t n = end - p->out_off;
            char *line;
            if (n && p->out.data[p->out_off + n - 1] == '\r')
                n--;
            line = xstrndup(p->out.data + p->out_off, n);
            p->out_off = nl ? end + 1 : end;
            if (p->out_off == p->out.len) {
                p->out.len = 0;
                p->out_off = 0;
            }
            return line;
        }
        if (p->fd_out < 0)
            return NULL;
        if (timeout_ms >= 0) {
            uint64_t now = os_time_ns();
            if (now >= deadline)
                return NULL;
            pt = (int)((deadline - now) / 1000000u) + 1;
        }
        pf.fd = p->fd_out;
        pf.events = POLLIN;
        pf.revents = 0;
        rc = poll(&pf, 1, pt);
        if (rc < 0 && errno != EINTR)
            return NULL;
        if (rc > 0)
            osp_read_into(&p->fd_out, &p->out);
    }
}

void os_proc_close_stdin(OsProc *p)
{
    if (p)
        osp_close(&p->fd_in);
}

void os_proc_free(OsProc *p)
{
    if (!p)
        return;
    osp_close(&p->fd_in);
    osp_close(&p->fd_out);
    osp_close(&p->fd_err);
    if (!p->done) {
        int status;
        kill(p->pid, SIGKILL);
        while (waitpid(p->pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    free(p->in_data);
    free(p->out.data);
    free(p->err.data);
    free(p);
}

/* ----------------------------------------------------- shared libraries --- */

void *os_dl_open(const char *path, char **err_out)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h && err_out) {
        const char *e = dlerror();
        *err_out = xstrdup(e ? e : "dlopen failed");
    }
    return h;
}

OsDlFn os_dl_sym(void *lib, const char *name)
{
    OsDlFn f = NULL;
    void *p;
    if (!lib)
        return NULL;
    p = dlsym(lib, name);
    /* POSIX guarantees data and function pointers convert; copy the bits so
     * that ISO C pedantry does not complain about the cast */
    if (p)
        memcpy(&f, &p, sizeof f < sizeof p ? sizeof f : sizeof p);
    return f;
}

void os_dl_close(void *lib)
{
    if (lib)
        dlclose(lib);
}

const char *os_dl_ext(void)
{
#if defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

uint64_t os_process_id(void)
{
    return (uint64_t)getpid();
}

#else
/* keep the translation unit non-empty on other platforms */
typedef int os_posix_empty_tu;
#endif /* !_WIN32 */
