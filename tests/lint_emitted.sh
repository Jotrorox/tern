#!/bin/sh
# lint_emitted.sh — check the golden emitted HDL in tests/fixtures/emit/ and
# the parameterized-module goldens tests/fixtures/build/param_*.sv (split
# into build/lint_emitted/) with external tools (run by CI; see
# docs/emitted-sv.md):
#
#   * every file set:            verilator --lint-only -Wall
#   * Verilog-2005 file sets:    yosys read_verilog -sv; synth -auto-top
#     (LINT_YOSYS_SV=1 also synthesizes the SystemVerilog sets)
#
# Usage: tests/lint_emitted.sh [<fixture-dir>...]   (default: all of them)
# Tools that are not installed are skipped. Exit status 1 if a check fails.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
if [ $# -eq 0 ]; then
    set -- "$root"/tests/fixtures/emit/*/
    # `tern build --stdout` concatenations: one directory per golden
    for g in "$root"/tests/fixtures/build/param_*.sv; do
        [ -f "$g" ] || continue
        d="$root/build/lint_emitted/$(basename "$g" .sv)"
        mkdir -p "$d"
        rm -f "$d"/*.sv "$d"/*.v
        awk -v dir="$d" '/^\/\/ ---- file: .* ----$/ { f = $4; next }
            f ~ /\.(sv|v)$/ { print > (dir "/" f) }' "$g"
        set -- "$@" "$d"
    done
fi

# Style-only warnings are waived; widths are checked exactly by test_emit
# (the simulator cross-check), and generated code legitimately leaves inputs
# or informational localparams unused.
VFLAGS="--lint-only -Wall -Wno-DECLFILENAME -Wno-UNUSED -Wno-WIDTH -Wno-PINCONNECTEMPTY -Wno-CASEINCOMPLETE -DTERN_NO_SVA"

have() { command -v "$1" >/dev/null 2>&1; }
# FPGA power-on values are declaration initializers (`logic [3:0] p = 4'd5;`)
# of registers that an always_ff also writes: the style warning PROCASSINIT
# (newer Verilators only; older ones reject unknown -Wno- codes)
if have verilator && verilator -Wno-PROCASSINIT --version >/dev/null 2>&1; then
    VFLAGS="$VFLAGS -Wno-PROCASSINIT"
fi
fail=0
have verilator && verilator --version
have yosys && yosys -V

for d in "$@"; do
    d=${d%/}
    name=$(basename "$d")
    case "$name" in
        *_v2005) v2005=1; base=${d%_v2005} ;;
        *) v2005=0; base=$d ;;
    esac
    # package first, then the modules, then stand-ins for extern modules
    files=""
    for f in "$d"/*_types_pkg.sv; do
        [ -f "$f" ] && files="$files $(basename "$f")"
    done
    for f in "$d"/*.sv "$d"/*.v; do
        [ -f "$f" ] || continue
        case "$f" in *_types_pkg.sv) continue ;; esac
        files="$files $(basename "$f")"
    done
    for f in "$base"/extern/*.v; do
        [ -f "$f" ] && files="$files $f"
    done
    [ -n "$files" ] || continue
    echo "== $name"
    if have verilator; then
        if [ $v2005 -eq 1 ]; then
            lang="--default-language 1364-2005"
        else
            lang="--default-language 1800-2012"
        fi
        # shellcheck disable=SC2086
        if ! (cd "$d" && verilator $VFLAGS $lang $files); then
            echo "lint_emitted: verilator failed on $name" >&2
            fail=1
        fi
    fi
    if have yosys && { [ $v2005 -eq 1 ] || [ "${LINT_YOSYS_SV:-0}" = 1 ]; }; then
        if ! (cd "$d" && yosys -q -p "read_verilog -sv $files; synth -auto-top" >/dev/null 2>&1); then
            echo "lint_emitted: yosys failed on $name" >&2
            (cd "$d" && yosys -q -p "read_verilog -sv $files; synth -auto-top" 2>&1 | tail -20) >&2
            fail=1
        fi
    fi
done
if [ $fail -ne 0 ]; then
    echo "lint_emitted: FAILED" >&2
    exit 1
fi
echo "lint_emitted: OK"
