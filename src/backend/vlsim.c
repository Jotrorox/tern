/* vlsim.c — the Verilator backend of `tern test` (spec §13.4: `tern test
 * --backend verilator`, used automatically for designs with extern
 * modules).
 *
 * vls_sim_new() turns an elaborated test design into a SimModel whose
 * values live in a Verilator model (src/sim/remote.c has the protocol):
 *
 *   1. the design is emitted as SystemVerilog (assertions included) into
 *      build/verilator/<top>/, with the sources of its extern modules, a
 *      port-less wrapper `tern_tb` (sv_emit_wrapper: one variable per top
 *      port, so VPI can drive and read the ports) and the generic harness
 *      tb_main.cpp (vls_harness_source: no per-design C++);
 *   2. `verilator --cc --exe --build --vpi --public-flat-rw --assert
 *      --trace` compiles it into obj_<hash>/Vdut, where <hash> covers every
 *      source and the flags, so an unchanged design is not compiled again
 *      (the model is built in a private directory and renamed into place,
 *      so parallel test workers do not collide);
 *   3. the binary is started with pipes to its stdin/stdout
 *      (OsSpawnOpts.interactive) and the model's signals are named for VPI
 *      as TOP.tern_tb.<port> and TOP.tern_tb.dut.<instance...>.<signal>.
 */
#include "../base/common.h"
#include "../base/arena.h"
#include "../base/diag.h"
#include "../base/map.h"
#include "../base/str.h"
#include "../os/os.h"
#include "../driver.h"
#include "../sim/sim.h"
#include "backend.h"

#define VLS_WRAPPER "tern_tb"

int vls_available(void)
{
    char *p = os_find_program("verilator");
    int ok = p != NULL;
    free(p);
    return ok;
}

/* ------------------------------------------------------------- naming -- */

typedef struct VlsModel {
    const IrDesign *d;
    SvNames *names;
    OsProc *proc;
    Arena arena;
    SvOutput files;           /* the emitted modules, for vls_locate */
} VlsModel;

/* "<file>.sv:<line>" in a Verilator message -> the nearest `// tern:` line
 * marker at or above that line of the emitted file */
static int vls_locate(void *user, const char *msg, Span *out)
{
    VlsModel *v = (VlsModel *)user;
    size_t i;
    for (i = 0; i < v->files.nfiles; i++) {
        const SvFile *f = &v->files.files[i];
        size_t nl = strlen(f->name);
        const char *p = msg;
        while ((p = strstr(p, f->name)) != NULL) {
            const char *at = p, *q = p + nl, *t, *mark = NULL;
            uint32_t line = 0, cur = 1;
            p += nl;
            if ((at > msg && (tern_is_ident((unsigned char)at[-1]) || at[-1] == '.')) || *q != ':' ||
                !tern_is_digit((unsigned char)q[1]))
                continue;
            for (q++; tern_is_digit((unsigned char)*q); q++)
                line = line * 10 + (uint32_t)(*q - '0');
            for (t = f->data; *t && cur <= line; cur++) {
                const char *e = strchr(t, '\n'), *mk = strstr(t, "// tern: ");
                if (mk && (!e || mk < e))
                    mark = mk + 9;
                if (!e)
                    break;
                t = e + 1;
            }
            if (mark) {
                const char *colon = NULL, *s;
                for (s = mark; *s && *s != '\n'; s++)
                    if (*s == ':')
                        colon = s;
                if (colon) {
                    char *path = xstrndup(mark, (size_t)(colon - mark));
                    uint32_t id = src_find(path), ln = (uint32_t)strtoul(colon + 1, NULL, 10);
                    const SourceFile *sf = id ? src_get(id) : NULL;
                    free(path);
                    if (sf && ln >= 1 && ln <= sf->nlines) {
                        *out = span_make(id, sf->lines[ln - 1], 0);
                        return 1;
                    }
                }
            }
            return 0;
        }
    }
    return 0;
}

/* the VPI name of a handle: TOP.tern_tb.<port> for the top's ports, else
 * TOP.tern_tb.dut.<instances>.<signal> with the emitter's SV names */
static const char *vls_path(void *user, const SimModel *m, int h)
{
    VlsModel *v = (VlsModel *)user;
    const IrSignal *ir = sim_signal_ir(m, h);
    const char *inst = sim_signal_instance(m, h);
    const IrModule *mod = v->d->top;
    StrBuf sb = {0};
    const char *r;
    uint32_t sig;
    if (!ir)
        return "?";
    sb_puts(&sb, "TOP." VLS_WRAPPER);
    if (*inst || (ir->kind != IRS_IN && ir->kind != IRS_OUT))
        sb_puts(&sb, ".dut");
    while (*inst) {
        const char *dot = strchr(inst, '.');
        size_t n = dot ? (size_t)(dot - inst) : strlen(inst);
        uint32_t i;
        for (i = 0; i < mod->ninsts; i++)
            if (strlen(mod->insts[i].name) == n && strncmp(mod->insts[i].name, inst, n) == 0)
                break;
        if (i == mod->ninsts) {
            sb_printf(&sb, ".%.*s", (int)n, inst);
            mod = NULL;
            break;
        }
        sb_printf(&sb, ".%s", sv_names_instance(v->names, mod, i));
        mod = mod->insts[i].module;
        inst = dot ? dot + 1 : inst + n;
    }
    sig = mod && ir >= mod->sigs && ir < mod->sigs + mod->nsigs ? (uint32_t)(ir - mod->sigs) : IR_NONE;
    sb_printf(&sb, ".%s", sig != IR_NONE ? sv_names_signal(v->names, mod, sig) : ir->name);
    r = arena_strdup(&v->arena, sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

static int vls_write(void *user, const char *line)
{
    VlsModel *v = (VlsModel *)user;
    return os_proc_write(v->proc, line, strlen(line));
}

static char *vls_read_line(void *user)
{
    VlsModel *v = (VlsModel *)user;
    return os_proc_read_line(v->proc, -1);
}

static void vls_close(void *user)
{
    VlsModel *v = (VlsModel *)user;
    if (v->proc) {
        os_proc_close_stdin(v->proc);
        os_proc_wait(v->proc);
        os_proc_free(v->proc);
    }
    sv_names_free(v->names);
    sv_output_free(&v->files);
    arena_release(&v->arena);
    free(v);
}

/* ------------------------------------------------------------ sources -- */

/* the files of the model in compile order (names in out) */
static void vls_sources(const IrDesign *d, const char *root, SvOutput *out, StrBuf *err, int *errs)
{
    SvOptions so;
    StrBuf w = {0};
    uint32_t i;
    sv_options_init(&so);
    so.emit_assertions = 1;
    so.constraints = 0;
    so.regmaps = 0;
    *errs += sv_emit_design(d, &so, out);
    for (i = 0; i < d->nmodules; i++) {
        const IrModule *m = d->modules[i];
        char *src, *text = NULL;
        size_t len = 0;
        StrBuf sb = {0};
        if (!m->is_extern || !m->extern_file || !*m->extern_file)
            continue;
        if (sv_output_find(out, os_path_basename(m->extern_file)))
            continue;
        src = bk_find_extern_source(m->extern_file, root, m->loc);
        if (!src || os_read_file(src, &text, &len) != 0) {
            sb_printf(err, "cannot find the source of extern module `%s`: \"%s\" (paths are relative to the "
                           "project root or the declaring file)\n",
                      m->name, m->extern_file);
            (*errs)++;
            free(src);
            continue;
        }
        sb_append(&sb, text, len);
        sv_output_add(out, os_path_basename(m->extern_file), SV_FILE_MODULE, &sb);
        sb_free(&sb);
        free(text);
        free(src);
    }
    sv_emit_wrapper(d, &so, VLS_WRAPPER, &w);
    sv_output_add(out, VLS_WRAPPER ".sv", SV_FILE_MODULE, &w);
    sb_free(&w);
}

static void vls_tail(const char *log, StrBuf *err)
{
    /* the last lines of a tool log */
    size_t n = log ? strlen(log) : 0, lines = 0;
    while (n > 0 && lines < 25) {
        n--;
        if (log[n] == '\n')
            lines++;
    }
    if (log)
        sb_puts(err, log + n);
}

/* --------------------------------------------------------------- model -- */

SimModel *vls_sim_new(const char *root, const IrDesign *d, uint64_t seed)
{
    SvOutput out;
    SvNames *names;
    BkPathVec files = {0};
    StrBuf err = {0}, hash_in = {0}, objname = {0};
    char *base, *dir = NULL, *obj = NULL, *exe = NULL;
    const char *top;
    VlsModel *v;
    SimModel *m = NULL;
    OsSpawnOpts so;
    SimRemoteIo io;
    size_t i;
    int errs = 0;
    uint64_t h;
    static const char *const flags[] = {
        "--cc", "--exe", "--build", "-j", "0", "-Wno-fatal", "-Wno-lint", "-Wno-style", "--vpi",
        "--public-flat-rw", "--assert", "--trace", "--timescale", "1ps/1ps", "-DTERN_NO_SVA",
        "--top-module", VLS_WRAPPER, "--prefix", "Vdut", "-o", "Vdut",
    };
    memset(&out, 0, sizeof out);
    if (!vls_available()) {
        diag_error(span_none(), "the verilator backend needs Verilator on PATH (https://verilator.org)");
        return NULL;
    }
    names = sv_names_new(d);
    top = d->top->name; /* the IR name: an extern top with generics is per argument set */
    vls_sources(d, root, &out, &err, &errs);
    if (errs) {
        diag_error(span_none(), "verilator backend: %s", err.len ? sb_cstr(&err) : "cannot emit the design");
        goto fail;
    }
    /* build/verilator/<top>/ */
    base = os_path_join(root, "build/verilator");
    dir = os_path_join(base, top);
    free(base);
    if (os_mkdir_p(dir) != 0) {
        diag_error(span_none(), "verilator backend: cannot create `%s`", dir);
        goto fail;
    }
    bk_hdl_files(&out, &files);
    for (i = 0; i < TERN_ARRAY_LEN(flags); i++)
        sb_printf(&hash_in, "%s ", flags[i]);
    sb_putc(&hash_in, '\n');
    for (i = 0; i < out.nfiles; i++) {
        char *fp;
        if (out.files[i].kind != SV_FILE_MODULE && out.files[i].kind != SV_FILE_PACKAGE &&
            out.files[i].kind != SV_FILE_MEMINIT)
            continue;
        fp = os_path_join(dir, out.files[i].name);
        os_write_file_if_changed(fp, out.files[i].data, out.files[i].len);
        free(fp);
        sb_printf(&hash_in, "%s %u\n", out.files[i].name, (unsigned)out.files[i].len);
        sb_append(&hash_in, out.files[i].data, out.files[i].len);
    }
    {
        char *fp = os_path_join(dir, "tb_main.cpp");
        os_write_file_if_changed(fp, vls_harness_source(), strlen(vls_harness_source()));
        free(fp);
        sb_puts(&hash_in, vls_harness_source());
    }
    h = hash_bytes(hash_in.data, hash_in.len);
    sb_printf(&objname, "obj_%016llx", (unsigned long long)h);
    obj = os_path_join(dir, sb_cstr(&objname));
    exe = os_path_join(obj, "Vdut");
    if (!os_is_file(exe)) {
        char *alt = os_path_join(obj, "Vdut.exe");
        if (os_is_file(alt)) {
            free(exe);
            exe = alt;
        } else {
            free(alt);
        }
    }
    if (!os_is_file(exe)) {
        /* compile in a private directory, then move it into place */
        VEC(char *) argv = {0};
        OsSpawnOpts ro;
        OsRunResult rr;
        StrBuf tmp = {0};
        int rc;
        sb_printf(&tmp, "%s.tmp%llx", sb_cstr(&objname), (unsigned long long)(os_time_ns() ^ (uintptr_t)d));
        vec_push(argv, (char *)"verilator");
        for (i = 0; i < TERN_ARRAY_LEN(flags); i++)
            vec_push(argv, (char *)flags[i]);
        vec_push(argv, (char *)"--Mdir");
        vec_push(argv, tmp.data);
        for (i = 0; i < files.len; i++)
            vec_push(argv, (char *)files.data[i]);
        vec_push(argv, (char *)"tb_main.cpp");
        vec_push(argv, NULL);
        memset(&ro, 0, sizeof ro);
        ro.cwd = dir;
        ro.stdin_null = 1;
        ro.capture_stdout = 1;
        ro.merge_stderr = 1;
        rc = os_run(argv.data, &ro, &rr);
        vec_free(argv);
        if (rc != 0) {
            StrBuf msg = {0};
            char *tp = os_path_join(dir, tmp.data);
            vls_tail(rr.out, &msg);
            diag_error(span_none(), "verilator could not build the model of `%s` (in %s):\n%s", top, dir,
                       sb_cstr(&msg));
            sb_free(&msg);
            os_remove_all(tp);
            free(tp);
            os_run_result_free(&rr);
            sb_free(&tmp);
            goto fail;
        }
        os_run_result_free(&rr);
        {
            char *tp = os_path_join(dir, tmp.data);
            if (os_rename(tp, obj) != 0 && !os_is_dir(obj)) {
                diag_error(span_none(), "verilator backend: cannot move `%s` to `%s`", tp, obj);
                free(tp);
                sb_free(&tmp);
                goto fail;
            }
            if (os_is_dir(tp))
                os_remove_all(tp); /* another process won the race */
            free(tp);
        }
        sb_free(&tmp);
        free(exe);
        exe = os_path_join(obj, "Vdut");
        if (!os_is_file(exe)) {
            char *alt = os_path_join(obj, "Vdut.exe");
            free(exe);
            exe = alt;
        }
        /* models of earlier versions of the design */
        {
            OsDirList list;
            size_t k;
            if (os_list_dir(dir, &list) == 0) {
                for (k = 0; k < list.len; k++) {
                    const char *nm = list.items[k].name;
                    if (strncmp(nm, "obj_", 4) == 0 && strcmp(nm, sb_cstr(&objname)) != 0 && !strstr(nm, ".tmp")) {
                        char *p = os_path_join(dir, nm);
                        os_remove_all(p);
                        free(p);
                    }
                }
                os_dir_list_free(&list);
            }
        }
    }
    /* start it */
    v = (VlsModel *)xcalloc(1, sizeof(VlsModel));
    v->d = d;
    v->names = names;
    names = NULL;
    v->files = out;
    memset(&out, 0, sizeof out);
    memset(&so, 0, sizeof so);
    so.interactive = 1;
    so.merge_stderr = 1;
    so.cwd = dir; /* $readmemh files sit next to the sources */
    {
        char *argv[2];
        char *perr = NULL;
        argv[0] = exe;
        argv[1] = NULL;
        v->proc = os_spawn(argv, &so, &perr);
        if (!v->proc) {
            diag_error(span_none(), "verilator backend: %s", perr ? perr : "cannot start the model");
            free(perr);
            vls_close(v);
            goto fail;
        }
    }
    memset(&io, 0, sizeof io);
    io.user = v;
    io.write = vls_write;
    io.read_line = vls_read_line;
    io.path = vls_path;
    io.close = vls_close;
    io.locate = vls_locate;
    m = sim_new_remote(d, d->top, seed, &io);
fail:
    sv_names_free(names);
    sv_output_free(&out);
    vec_free(files);
    sb_free(&err);
    sb_free(&hash_in);
    sb_free(&objname);
    free(dir);
    free(obj);
    free(exe);
    return m;
}

/* the TestRunOptions.remote hook of cmd_test.c / cmd_sim.c */
SimModel *vls_test_model(void *user, Driver *dr, const IrDesign *d, uint64_t seed)
{
    char *cwd;
    SimModel *m;
    TERN_UNUSED(user);
    if (dr->has_project)
        return vls_sim_new(dr->prj.manifest.root, d, seed);
    cwd = os_getcwd();
    m = vls_sim_new(cwd ? cwd : ".", d, seed);
    free(cwd);
    return m;
}

#undef VLS_WRAPPER
