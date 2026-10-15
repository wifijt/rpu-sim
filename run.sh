#!/bin/bash
# Build the simulator for a sketch and run scenarios against it.
#   run.sh path/to/MySketch scenario.sim [more.sim ...]
# Set SIM_DEFINES (e.g. "-DMACHINE_BALLY_STAR_TREK") to build the sketch for one machine.
set -uo pipefail
SIM="$(cd "$(dirname "$0")" && pwd)"
[ $# -ge 2 ] || { echo "usage: run.sh path/to/sketch scenario.sim [...]" >&2; exit 2; }
SKETCH="$1"; shift
"$SIM/build.sh" "$SKETCH" || exit 1
BIN="$SIM/build/$(basename "$(cd "$SKETCH" && pwd)")/sim"
fail=0
for f in "$@"; do
  case "$(basename "$f")" in machine.sim) continue ;; esac
  "$BIN" "$f" || fail=1
done
exit $fail
