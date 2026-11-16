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
    int cdc_report;    /* print the list of clock-domain crossings */
} CheckOptions;

/* Runs every check over all modules of the design. Returns the number of
 * errors reported. */
int check_design(IrDesign *d, const CheckOptions *opt);

#endif /* TERN_CHECK_H */
