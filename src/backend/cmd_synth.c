/* cmd_synth.c — the `tern synth` command: build, write the tool script
 * (synth.c) to build/<target>/synth/, run the tool when it is installed
 * and print the utilization / timing summary.
 *
 *     tern synth [--target <t>] [--tool yosys|vivado|quartus] [--top <m>]
 *                [--features <l>] [--no-run] [<files>...]
 */
#include "../base/common.h"
#include "../base/args.h"
#include "../base/arena.h"
#include "../base/str.h"
#include "../base/diag.h"
#include "../os/os.h"
#include "../driver.h"
#include "../cmds.h"
#include "backend.h"

static char *csyn_read(const char *dir, const char *name)
{
    char *p = os_path_join(dir, name), *text = NULL;
    size_t len = 0;
    if (os_read_file(p, &text, &len) != 0)
        text = NULL;
    free(p);
    return text;
}

/* nextpnr device / package flags for ice40 / ecp5 */
static int csyn_nextpnr_args(const BkTarget *t, const char **prog, const char **devflag, const char **pkg)
{
    const char *d = t->device ? t->device : "";
    char lower[64];
    size_t i;
    for (i = 0; d[i] && i + 1 < sizeof lower; i++)
        lower[i] = (char)tern_to_lower((unsigned char)d[i]);
    lower[i] = 0;
    if (t->family == BK_FAM_ICE40) {
        static const char *const devs[] = {"up5k", "up3k", "u4k", "hx8k", "hx4k", "hx1k", "lp8k", "lp4k", "lp1k", "lp384"};
        static const char *const pkgs[] = {"sg48", "sg48", "sg48", "ct256", "tq144", "tq144", "cm81", "cm81", "cm81", "qn32"};
        *prog = "nextpnr-ice40";
        for (i = 0; i < TERN_ARRAY_LEN(devs); i++)
            if (strstr(lower, devs[i])) {
                static char flag[16];
                snprintf(flag, sizeof flag, "--%s", devs[i]);
                *devflag = flag;
                *pkg = t->package ? t->package : pkgs[i];
                return 1;
            }
        return 0;
    }
    if (t->family == BK_FAM_ECP5) {
        *prog = "nextpnr-ecp5";
        *pkg = t->package ? t->package : "CABGA381";
        if (strstr(lower, "um5g") && strstr(lower, "85"))
            *devflag = "--um5g-85k";
        else if (strstr(lower, "85"))
            *devflag = "--85k";
        else if (strstr(lower, "45"))
            *devflag = "--45k";
        else if (strstr(lower, "25"))
            *devflag = "--25k";
        else if (strstr(lower, "12"))
            *devflag = "--12k";
        else
            return 0;
        return 1;
    }
    return 0;
}

static double csyn_target_mhz(const BkBuild *b)
{
    uint32_t i;
    const IrModule *top;
    if (b->target.clock_hz)
        return (double)b->target.clock_hz / 1e6;
    if (!b->ndesigns)
        return 0;
    top = b->designs[0]->top;
    if (top->default_domain != IR_NONE && top->default_domain < top->ndomains && top->domains[top->default_domain].hz)
        return (double)top->domains[top->default_domain].hz / 1e6;
    for (i = 0; i < top->ndomains; i++)
        if (top->domains[i].hz)
            return (double)top->domains[i].hz / 1e6;
    return 0;
}

static int csyn_run(char **argv, const char *cwd, const char *log_name, int quiet)
{
    OsSpawnOpts so;
    OsRunResult res;
    int r;
    memset(&so, 0, sizeof so);
    so.cwd = cwd;
    so.stdin_null = 1;
    so.capture_stdout = 1;
    so.merge_stderr = 1;
    r = os_run(argv, &so, &res);
    if (log_name) {
        char *p = os_path_join(cwd, log_name);
        os_write_file(p, res.out ? res.out : "", res.out_len);
        free(p);
    }
    if (r != 0 && !quiet && res.out) {
        /* the tail of the output */
        size_t n = res.out_len, lines = 0;
        while (n > 0 && lines < 15) {
            n--;
            if (res.out[n] == '\n')
                lines++;
        }
        fwrite(res.out + n, 1, res.out_len - n, stdout);
    }
    os_run_result_free(&res);
    return r;
}

int cmd_synth(int argc, char **argv)
{
    ArgParser ap;
    BkBuildOptions o;
    BkBuild b;
    BkPathVec paths = {0}, files = {0};
    const char *val = NULL;
    BkTool tool = BK_TOOL_NONE;
    int no_run = 0, rc = TERN_EXIT_OK;
    char *sdir = NULL, *sdisp = NULL, *exe = NULL;
    const char *top, *xdc = NULL, *sdc = NULL;
    SynScriptIn in;
    SvOutput v2005;
    StrBuf script = {0}, cmdline = {0};
    size_t i, k;
    double target_mhz;

    bk_build_options_init(&o);
    o.cmd = "synth";
    memset(&v2005, 0, sizeof v2005);
    args_init(&ap, argc, argv);
    while (args_next(&ap)) {
        if (args_flag(&ap, "help|h")) {
            cmd_print_usage("synth");
            vec_free(paths);
            return TERN_EXIT_OK;
        } else if (args_opt(&ap, "target|t", &val)) {
            o.target = val;
        } else if (args_opt(&ap, "tool", &val)) {
            if (bk_tool_parse(val, &tool) != 0)
                args_error(&ap, "invalid --tool `%s` (yosys, vivado or quartus)", val);
        } else if (args_opt(&ap, "top", &val)) {
            o.top = val;
        } else if (args_opt(&ap, "features", &val)) {
            o.features = val;
        } else if (args_flag(&ap, "no-run")) {
            no_run = 1;
        } else if (args_positional(&ap)) {
            vec_push(paths, ap.arg);
        } else {
            args_unknown(&ap);
        }
    }
    if (ap.errors) {
        vec_free(paths);
        return TERN_EXIT_USAGE;
    }
    o.paths = paths.data;
    o.npaths = paths.len;
    if (bk_build_run(&o, &b) != 0) {
        rc = b.usage_error ? TERN_EXIT_USAGE : TERN_EXIT_FAIL;
        goto done;
    }
    if (bk_build_write(&b, 0) != 0) {
        rc = TERN_EXIT_FAIL;
        goto done;
    }
    if (tool == BK_TOOL_NONE)
        tool = b.target.tool != BK_TOOL_NONE ? b.target.tool : bk_family_default_tool(b.target.family);
    top = bk_build_top_name(&b);
    sdir = os_path_join(b.out_dir, "synth");
    sdisp = os_path_join(b.out_dir_display, "synth");
    if (os_mkdir_p(sdir) != 0) {
        diag_error(span_none(), "cannot create directory `%s`", sdir);
        rc = TERN_EXIT_FAIL;
        goto done;
    }
    memset(&in, 0, sizeof in);
    in.tool = tool;
    in.family = b.target.family;
    in.device = b.target.device;
    in.part = b.target.part;
    in.package = b.target.package;
    in.family_name = b.target.family_name;
    in.top = top;
    target_mhz = csyn_target_mhz(&b);
    in.clock_mhz = target_mhz;
    for (i = 0; i < b.out.nfiles; i++) {
        if (b.out.files[i].kind == SV_FILE_XDC)
            xdc = b.out.files[i].name;
        if (b.out.files[i].kind == SV_FILE_SDC)
            sdc = b.out.files[i].name;
    }
    if (tool == BK_TOOL_YOSYS) {
        /* Yosys reads a Verilog-2005 emission of the same design */
        SvOptions so = b.sv;
        so.std = SV_STD_V2005;
        so.constraints = 0;
        so.regmaps = 0;
        so.emit_assertions = 0;
        for (i = 0; i < b.ndesigns; i++) {
            SvOutput one;
            memset(&one, 0, sizeof one);
            if (emit_design(b.designs[i], &so, &one) != 0)
                rc = TERN_EXIT_FAIL;
            for (k = 0; k < one.nfiles; k++) {
                StrBuf sb = {0};
                sb_append(&sb, one.files[k].data, one.files[k].len);
                sv_output_add(&v2005, one.files[k].name, one.files[k].kind, &sb);
                sb_free(&sb);
            }
            sv_output_free(&one);
        }
        /* extern sources and memory files come from the build */
        for (i = 0; i < b.out.nfiles; i++)
            if ((b.out.files[i].kind == SV_FILE_MODULE && bk_is_extern_file(&b.out, b.out.files[i].name))) {
                StrBuf sb = {0};
                sb_append(&sb, b.out.files[i].data, b.out.files[i].len);
                sv_output_add(&v2005, b.out.files[i].name, SV_FILE_MODULE, &sb);
                sb_free(&sb);
            }
        for (i = 0; i < v2005.nfiles; i++) {
            char *p = os_path_join(sdir, v2005.files[i].name);
            os_write_file_if_changed(p, v2005.files[i].data, v2005.files[i].len);
            free(p);
        }
        bk_hdl_files(&v2005, &files);
        in.src_dir = ".";
        in.sv = 0;
    } else {
        /* vendor tools read the build output; memory files are copied next
         * to the script, where the tool runs */
        bk_hdl_files(&b.out, &files);
        for (i = 0; i < b.out.nfiles; i++)
            if (b.out.files[i].kind == SV_FILE_MEMINIT) {
                char *p = os_path_join(sdir, b.out.files[i].name);
                os_write_file_if_changed(p, b.out.files[i].data, b.out.files[i].len);
                free(p);
            }
        in.src_dir = "..";
        in.sv = b.sv.std == SV_STD_SV2012;
        in.xdc = xdc;
        in.sdc = sdc;
    }
    in.files = files.data;
    in.nfiles = files.len;
    syn_script(&in, &script);
    {
        char *p = os_path_join(sdir, syn_script_name(tool));
        if (os_write_file_if_changed(p, script.data, script.len) < 0) {
            diag_error(span_none(), "cannot write `%s`", p);
            rc = TERN_EXIT_FAIL;
        }
        free(p);
    }
    if (rc != TERN_EXIT_OK)
        goto done;
    printf("tern synth: %s (target %s, %s%s%s) -> %s/%s\n", b.designs[0]->top->display, b.target.name,
           bk_tool_name(tool), b.target.device ? ", " : "", b.target.device ? b.target.device : "", sdisp,
           syn_script_name(tool));
    syn_command(tool, &cmdline);
    exe = os_find_program(tool == BK_TOOL_VIVADO ? "vivado" : tool == BK_TOOL_QUARTUS ? "quartus_sh" : "yosys");
    if (no_run) {
        printf("  run it:\n");
        printf("    cd %s && %s\n", sdisp, cmdline.data);
        goto done;
    }
    if (!exe) {
        /* like `tern prove` without sby: the files are written, but nothing
         * was synthesized, so the status says so (exit 2) */
        fflush(stdout);
        fprintf(stderr,
                "tern synth: error: %s is not on PATH; the script is in %s/\n"
                "  install %s,\n  then run `tern synth` again, or run the script yourself:\n"
                "    cd %s && %s\n",
                tool == BK_TOOL_QUARTUS ? "quartus_sh" : bk_tool_name(tool), sdisp,
                tool == BK_TOOL_VIVADO    ? "Vivado"
                : tool == BK_TOOL_QUARTUS ? "Quartus Prime"
                                          : "Yosys (e.g. from the OSS CAD Suite, https://github.com/YosysHQ/oss-cad-suite-build)",
                sdisp, cmdline.data);
        rc = TERN_EXIT_USAGE;
        goto done;
    }
    {
        SynReport rep;
        StrBuf out = {0};
        char *text;
        int r;
        syn_report_init(&rep);
        printf("  running %s ...\n", bk_tool_name(tool));
        fflush(stdout);
        if (tool == BK_TOOL_YOSYS) {
            char *av[7];
            av[0] = exe;
            av[1] = (char *)"-q";
            av[2] = (char *)"-l";
            av[3] = (char *)"synth.log";
            av[4] = (char *)"-s";
            av[5] = (char *)"synth.ys";
            av[6] = NULL;
            r = csyn_run(av, sdir, NULL, 0);
            if ((text = csyn_read(sdir, "utilization.txt")) != NULL) {
                syn_parse_yosys_stat(text, &rep);
                free(text);
            }
            if (r == 0 && (b.target.family == BK_FAM_ICE40 || b.target.family == BK_FAM_ECP5)) {
                const char *prog = NULL, *devflag = NULL, *pkg = NULL;
                char *pnr = NULL;
                if (csyn_nextpnr_args(&b.target, &prog, &devflag, &pkg) && (pnr = os_find_program(prog)) != NULL) {
                    char *av2[12];
                    StrBuf json = {0}, freq = {0};
                    size_t na = 0;
                    sb_printf(&json, "%s.json", top);
                    sb_printf(&freq, "%.3f", target_mhz > 0 ? target_mhz : 12.0);
                    av2[na++] = pnr;
                    av2[na++] = (char *)devflag;
                    av2[na++] = (char *)"--package";
                    av2[na++] = (char *)pkg;
                    av2[na++] = (char *)"--json";
                    av2[na++] = json.data;
                    av2[na++] = (char *)"--freq";
                    av2[na++] = freq.data;
                    av2[na++] = (char *)(b.target.family == BK_FAM_ICE40 ? "--pcf-allow-unconstrained"
                                                                        : "--lpf-allow-unconstrained");
                    av2[na++] = NULL;
                    printf("  running %s ...\n", prog);
                    fflush(stdout);
                    csyn_run(av2, sdir, "nextpnr.log", 1);
                    if ((text = csyn_read(sdir, "nextpnr.log")) != NULL) {
                        syn_parse_nextpnr(text, &rep);
                        free(text);
                    }
                    sb_free(&json);
                    sb_free(&freq);
                }
                free(pnr);
            }
        } else if (tool == BK_TOOL_VIVADO) {
            char *av[8];
            av[0] = exe;
            av[1] = (char *)"-mode";
            av[2] = (char *)"batch";
            av[3] = (char *)"-nojournal";
            av[4] = (char *)"-nolog";
            av[5] = (char *)"-source";
            av[6] = (char *)"synth.tcl";
            av[7] = NULL;
            r = csyn_run(av, sdir, "vivado.log", 0);
            if ((text = csyn_read(sdir, "utilization.rpt")) != NULL) {
                syn_parse_vivado_util(text, &rep);
                free(text);
            }
            if ((text = csyn_read(sdir, "timing.rpt")) != NULL) {
                syn_parse_vivado_timing(text, &rep);
                free(text);
            }
        } else {
            char *av[4];
            StrBuf f = {0};
            av[0] = exe;
            av[1] = (char *)"-t";
            av[2] = (char *)"synth_quartus.tcl";
            av[3] = NULL;
            r = csyn_run(av, sdir, "quartus.log", 0);
            sb_printf(&f, "output_files/%s.fit.summary", top);
            if ((text = csyn_read(sdir, f.data)) != NULL) {
                syn_parse_quartus_fit(text, &rep);
                free(text);
            }
            sb_reset(&f);
            sb_printf(&f, "output_files/%s.sta.summary", top);
            if ((text = csyn_read(sdir, f.data)) != NULL) {
                syn_parse_quartus_sta(text, &rep);
                free(text);
            }
            sb_reset(&f);
            sb_printf(&f, "output_files/%s.sta.rpt", top);
            if ((text = csyn_read(sdir, f.data)) != NULL) {
                syn_parse_quartus_sta(text, &rep);
                free(text);
            }
            sb_free(&f);
        }
        if (r != 0) {
            printf("tern synth: %s failed (exit %d); see the log in %s/\n", bk_tool_name(tool), r, sdisp);
            rc = TERN_EXIT_FAIL;
        }
        if (r == 0 || rep.luts >= 0) {
            syn_report_print(&rep, target_mhz, &out);
            fwrite(out.data ? out.data : "", 1, out.len, stdout);
        }
        if (r == 0 && rep.fmax_mhz > 0 && target_mhz > 0 && rep.fmax_mhz < target_mhz)
            rc = TERN_EXIT_FAIL;
        sb_free(&out);
    }
done:
    free(exe);
    free(sdir);
    free(sdisp);
    sb_free(&script);
    sb_free(&cmdline);
    sv_output_free(&v2005);
    vec_free(files);
    bk_build_free(&b);
    vec_free(paths);
    return rc;
}
