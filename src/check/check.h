/* check.h — design checks over the elaborated IR (spec §6, §8.3, §13.2):
 * multiple / missing drivers, the full-assignment (no-latch) proof, the
 * CDC report and unused signals. `tern check`, `tern build` and the LSP
 * call check_design() after elaboration (src/driver.h).
 *
 * This file is the hook; the passes are implemented in src/check/
 * (drivers.c, assign.c, cdc.c, unused.c). Diagnostics go through diag.h,
 * with IrSignal.loc / IrStmt.loc spans; warnings become errors under
 * --strict (diag_set_strict, already applied by the command).
 *
 * The individual checks (src/check/):
 *   drivers.c  multiple drivers (two processes, a process and an assign,
 *              an instance output and anything else; bit-accurate), signals
 *              never driven (outputs, wires, vars, registers);
 *   assign.c   the no-latch proof of comb blocks (every bit of every
 *              signal a comb block drives is assigned on every path, with
 *              the failing path in the message), reads of a signal in the
 *              comb block that drives it before it is assigned, and
 *              combinational loops through assigns, comb blocks and
 *              instances (using each module's port dependency summary);
 *   cdc.c      clock-domain crossings: domains are inferred for wires and
 *              vars through their drivers; reading another domain's
 *              signal outside a std::cdc primitive, or mixing domains, is
 *              an error; resets taken from another domain warn; the CDC
 *              report lists every primitive instance and unsafe_cross;
 *   unused.c   (--unused) unread inputs, wires, vars, registers, memories
 *              and instance outputs.
 *
 * What the elaborator guarantees about the IR (src/elab/elab.c):
 *   - IrSignal.domain is set for registers, ports (their `@ dom` or the
 *     module's default domain) and instance-output wires; wires and vars
 *     may be IR_NONE (domains propagate through assignments);
 *   - a `var` with a default gets that default as the first statement of
 *     the comb process that drives it;
 *   - wires created by `unsafe_cross(x)` carry the IrAttr "unsafe_cross";
 *   - every port of an instance `x` is a parent wire `x_<port>` (outputs:
 *     driven through IrConn; data inputs: driven by IrAssign).
 */
#ifndef TERN_CHECK_H
#define TERN_CHECK_H

#include "../base/common.h"
#include "../ir/ir.h"

typedef struct CheckOptions {
    int strict;        /* --strict */
    int unused;        /* --unused: report unused signals and outputs */
    int cdc_report;    /* print the CDC report: 0 never (crossings are still
                          recorded in IrDesign.crossings), 1 when the design
                          has crossings (`tern check`), 2 always
                          (`tern check --cdc-report`) */
    FILE *report_out;  /* where the CDC report goes (NULL: stdout) */
    /* --unused warnings are only reported for signals declared in files for
       which this returns nonzero (NULL: every file), so library modules
       instantiated by the user stay quiet */
    int (*is_user_file)(uint32_t file, void *user);
    void *user;
} CheckOptions;

/* Runs every check over all modules of the design and records the
 * clock-domain crossings in d->crossings (one entry per instance of a
 * std::cdc primitive and per `unsafe_cross`, along every instance path
 * from the roots of the design) and prints them as the CDC report (spec
 * §8.3) as opt->cdc_report says. Returns the number of errors reported.
 * opt may be NULL (defaults: no --unused, no report). */
int check_design(IrDesign *d, const CheckOptions *opt);

/* Prints the CDC report of d->crossings (as check_design does). */
void check_print_cdc_report(const IrDesign *d, FILE *out);

#endif /* TERN_CHECK_H */
