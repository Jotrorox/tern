/* driver.h — the shared front half of every command (spec §13.1): locate
 * the project, collect and parse sources, load std, resolve, and elaborate.
 *
 *     DriverOptions o = {0};
 *     o.paths = argv_paths; o.npaths = n;     // files/dirs; none: the project's src/
 *     Driver *dr = driver_open(&o);           // NULL only for usage errors (reported)
 *     if (driver_parse_errors(dr)) ...        // syntax errors were reported
 *     Arena ir = {0};
 *     IrDesign *d = driver_elaborate_top(dr, NULL, &ir);   // first #[top] / target top
 *     ...
 *     driver_free(dr); arena_release(&ir);
 *
 * Modes:
 *   - project mode (a tern.toml at or above the current directory): sources
 *     are src/ (plus tests/ with include_tests), dependencies' src/ roots
 *     are library sources, [features] become cfg::name;
 *   - single-file mode (no tern.toml): the files/directories given.
 *   Explicit paths always win: they are the "user" files that are checked and
 *   whose roots are elaborated; the rest of the project is library code.
 *
 * Sources are registered with src_add once per path: opening the same
 * files again (the LSP re-analyses after every edit) reuses their source ids
 * and updates the text when it changed.
 *
 * std comes from $TERN_STD, else a `std/` directory next to the executable,
 * else the copy embedded in the binary (src/std_embed.c). When the user
 * files themselves declare `package std::...` (e.g. `tern check std`), those
 * packages replace the library's copy.
 */
#ifndef TERN_DRIVER_H
#define TERN_DRIVER_H

#include "base/common.h"
#include "base/arena.h"
#include "base/vec.h"
#include "sema/sema.h"
#include "elab/elab.h"
#include "ir/ir.h"
#include "pkg/pkg.h"

/* Contents that replace a file on disk (the LSP's open editor buffers):
 * returns the text of `path` (as the driver spells it: absolute when the
 * project root / given paths are) or NULL to read the file normally. */
typedef const char *(*DriverOverlayFn)(void *user, const char *path, size_t *len);

typedef struct DriverOptions {
    const char *const *paths;   /* command-line files / directories (may be empty) */
    size_t npaths;
    int include_tests;          /* also compile tests/ (tern test) */
    uint64_t const_eval_limit;  /* 0: default 10^7 */
    const char *features;       /* --features "a,-b", or NULL */
    const char *cmd;            /* command name for messages ("check") */
    const char *root;           /* where to look for tern.toml (upwards); NULL: cwd */
    DriverOverlayFn overlay;    /* NULL: files come from disk */
    void *overlay_user;
} DriverOptions;

typedef struct DriverItem {
    Item *item;
    FileUnit *file;
} DriverItem;

typedef VEC(DriverItem) DriverItemVec;

typedef struct Driver {
    Sema *S;
    DriverOptions opt;
    PkgProject prj;
    int has_project;
    VEC(Arena *) arenas;        /* one AST arena per file */
    int parse_errors;           /* syntax errors in any file */
    int user_parse_errors;      /* syntax errors in user files */
    const char *std_source;     /* "embedded", or the directory */
    size_t nuser;               /* number of user files */
} Driver;

/* Opens the project / files, parses everything and resolves names. Returns
 * NULL (after printing a message) for usage errors such as missing inputs. */
Driver *driver_open(const DriverOptions *opt);
void driver_free(Driver *dr);
Sema *driver_sema(Driver *dr);
int driver_parse_errors(Driver *dr);

/* Item lookup by path: "Top", "uart::UartTx", "std::fifo::Fifo". */
Item *driver_find_item(Driver *dr, const char *path, FileUnit **file_out);
/* #[top] modules of the user files, in source order. */
void driver_tops(Driver *dr, DriverItemVec *out);
/* Modules elaborated by `tern check` when there is no #[top]: every
 * non-generic module and every generic module whose generics all have
 * defaults (user files only; extern modules excluded). */
void driver_default_roots(Driver *dr, DriverItemVec *out);
/* `test` items of the user files (with their file = package scope). */
void driver_tests(Driver *dr, DriverItemVec *out);
/* Targets of tern.toml (NULL / 0 in single-file mode). */
size_t driver_targets(Driver *dr, const TernTarget **out);

/* A configured elaborator writing into `design` (NULL: interpreter only). */
Elab *driver_elab_new(Driver *dr, IrDesign *design);
/* Elaborates `item` with generic arguments `args` (NULL: defaults) into a
 * new design whose top is that module. NULL when the module failed. The
 * design lives in `ir_arena`. */
IrDesign *driver_elaborate_module(Driver *dr, Item *item, FileUnit *file, const ElabArgs *args, Arena *ir_arena);
/* By name (NULL: the --top / target top, else the first #[top], else the
 * first default root). */
IrDesign *driver_elaborate_top(Driver *dr, const char *name, Arena *ir_arena);
/* `tern check`: elaborates every root (given top, #[top]s or default roots,
 * plus the modules that the user's tests instantiate with `sim`) into one
 * design. Returns the design (never NULL). */
IrDesign *driver_elaborate_all(Driver *dr, const char *top, Arena *ir_arena);

#endif /* TERN_DRIVER_H */
