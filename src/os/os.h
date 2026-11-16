/* os.h — the platform layer. The only code allowed to include OS headers is
 * os_posix.c and os_win32.c; everything else goes through this interface.
 *
 * Conventions:
 *   - Paths are UTF-8 strings. '/' works as a separator on every platform
 *     (os_win32.c converts as needed); functions that build paths use '/'.
 *   - Functions returning `char *` return a heap string the caller frees
 *     with free(), or NULL on failure.
 *   - Functions returning `int` status return 0 on success and -1 on failure
 *     unless documented otherwise.
 */
#ifndef TERN_OS_H
#define TERN_OS_H

#include "../base/common.h"

/* --------------------------------------------------------------- files --- */

/* Read a whole file. *data receives a heap buffer with a NUL appended (not
 * counted in *len). */
int os_read_file(const char *path, char **data, size_t *len);
/* Create/truncate and write a whole file. */
int os_write_file(const char *path, const void *data, size_t len);
/* Write only if the contents differ (keeps mtimes stable for make). Returns
 * 1 if written, 0 if unchanged, -1 on error. */
int os_write_file_if_changed(const char *path, const void *data, size_t len);
int os_copy_file(const char *from, const char *to);
int os_rename(const char *from, const char *to);   /* replaces `to` if it exists */
int os_remove(const char *path);                   /* file or empty directory */
int os_remove_all(const char *path);               /* recursive; 0 if path absent */
int os_mkdir(const char *path);                    /* one level; 0 if it is a directory already */
int os_mkdir_p(const char *path);                  /* with parents; 0 if it exists already */

typedef struct OsStat {
    int exists;
    int is_dir;
    int is_file;          /* regular file */
    int is_link;          /* the path itself is a symbolic link / reparse point
                             (the other fields describe its target) */
    uint64_t size;
    int64_t mtime_ns;     /* modification time, ns since the Unix epoch */
} OsStat;

int os_stat(const char *path, OsStat *st);         /* -1 (st->exists = 0) if absent */
int os_exists(const char *path);
int os_is_dir(const char *path);
int os_is_file(const char *path);

typedef struct OsDirEntry {
    char *name;           /* entry name (no directory part) */
    int is_dir;           /* directory (following symbolic links) */
    int is_link;          /* symbolic link / reparse point */
} OsDirEntry;

typedef struct OsDirList {
    OsDirEntry *items;    /* sorted by name (byte order); excludes . and .. */
    size_t len;
} OsDirList;

int os_list_dir(const char *path, OsDirList *out);
void os_dir_list_free(OsDirList *list);

/* ---------------------------------------------------------------- paths --- */

char *os_getcwd(void);
int os_chdir(const char *path);
char *os_home_dir(void);              /* $HOME / %USERPROFILE% */
char *os_temp_dir(void);
char *os_exe_path(void);              /* absolute path of the running executable */
/* Fallback for os_exe_path on platforms without a native query. */
void os_set_argv0(const char *argv0);
const char *os_get_argv0(void);       /* as set by os_set_argv0, or NULL */

char *os_path_join(const char *a, const char *b);   /* b absolute: returns a copy of b */
/* Lexically normalize: '\\' -> '/', collapse "//", remove ".", resolve ".."
 * where possible, drop a trailing '/'. "" becomes ".". */
char *os_path_normalize(const char *path);
char *os_path_abs(const char *path);                /* join with cwd, then normalize */
char *os_path_dirname(const char *path);            /* "a/b/c" -> "a/b", "c" -> "." */
const char *os_path_basename(const char *path);     /* pointer into path */
const char *os_path_ext(const char *path);          /* ".tern", or "" (pointer into path) */
int os_path_is_abs(const char *path);
/* Path of `path` relative to directory `base` when it lies inside it,
 * otherwise a copy of `path` (both are normalized first). */
char *os_path_relative(const char *path, const char *base);

/* ----------------------------------------------------------- environment --- */

char *os_getenv(const char *name);                  /* NULL if unset */
int os_setenv(const char *name, const char *value); /* value NULL unsets */
/* Search PATH (and PATHEXT on Windows) for an executable. */
char *os_find_program(const char *name);

/* ------------------------------------------------------------ time, tty --- */

uint64_t os_time_ns(void);            /* monotonic clock */
int64_t os_unix_time(void);           /* wall clock, seconds since the epoch */
void os_sleep_ms(uint32_t ms);
int os_isatty(int fd);                /* fd 0, 1 or 2 */
/* Should coloured output be written to fd (1 or 2)? A tty, NO_COLOR unset,
 * TERM != dumb. Enables ANSI processing on Windows consoles. */
int os_color_enabled(int fd);
int os_cpu_count(void);               /* >= 1 */
/* Put stdout/stdin in binary mode (no CRLF translation; no-op on POSIX). */
void os_set_binary_stdio(void);

/* ------------------------------------------------------------ processes --- */

typedef struct OsProc OsProc;

typedef struct OsSpawnOpts {
    const char *cwd;          /* working directory for the child, NULL = inherit */
    const char *stdin_data;   /* bytes fed to the child's stdin, or NULL */
    size_t stdin_len;
    int stdin_null;           /* stdin from the null device (when stdin_data is NULL) */
    int capture_stdout;       /* collect into a buffer (else inherit) */
    int capture_stderr;
    int merge_stderr;         /* child's stderr goes wherever its stdout goes */
} OsSpawnOpts;

/* Start argv[0] (searched in PATH when it has no directory part) with the
 * NULL-terminated argv. Returns NULL if the program could not be started
 * (*err_out, if non-NULL, gets a heap message). opts may be NULL. */
OsProc *os_spawn(char *const argv[], const OsSpawnOpts *opts, char **err_out);

/* Wait for one child to finish, pumping all captured pipes. Returns 0, or
 * -1 on an OS error. */
int os_proc_wait(OsProc *p);

/* Wait until any of procs[0..n) has finished; NULL entries are skipped.
 * Captured output of every listed process is pumped meanwhile, so children
 * never block on a full pipe. timeout_ms < 0 waits forever. Returns the
 * index of a finished process (an already-finished entry is returned
 * immediately, so the caller should free and NULL each returned entry), or
 * -1 on timeout / when no entry is left to wait for. The returned process
 * is reaped: its exit code and captured output are available. */
int os_proc_wait_any(OsProc **procs, size_t n, int timeout_ms);

int os_proc_done(const OsProc *p);
/* Exit status: the exit code, or 128+signal for a signal death (POSIX). */
int os_proc_exit_code(const OsProc *p);
const char *os_proc_stdout(const OsProc *p, size_t *len);   /* NUL-terminated, never NULL */
const char *os_proc_stderr(const OsProc *p, size_t *len);
void os_proc_kill(OsProc *p);
void os_proc_free(OsProc *p);   /* kills and reaps it if still running */

/* Convenience: spawn, wait, and hand back output. Returns the exit code, or
 * -1 if the program could not be started. out/err may be NULL (then that
 * stream is inherited unless captured via opts). */
typedef struct OsRunResult {
    int exit_code;
    char *out;        /* heap, NUL-terminated (or NULL if not captured) */
    size_t out_len;
    char *err;
    size_t err_len;
} OsRunResult;

int os_run(char *const argv[], const OsSpawnOpts *opts, OsRunResult *res);
void os_run_result_free(OsRunResult *res);

#endif /* TERN_OS_H */
