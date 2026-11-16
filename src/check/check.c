/* check.c — entry point of the IR checks (see check.h). The individual
 * passes are added here as they are written. */
#include "check.h"

int check_design(IrDesign *d, const CheckOptions *opt)
{
    TERN_UNUSED(d);
    TERN_UNUSED(opt);
    return 0;
}
