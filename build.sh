#!/bin/bash
# Build the host simulator for an RPU sketch.
#   build.sh path/to/MySketch     -> build/MySketch/sim
#   (no argument: the folder above this one, for rpu-sim kept inside a sketch as sim/)
# Needs the Arduino IDE's arduino-cli (to add the sketch's function prototypes) and clang++.
set -euo pipefail
SIM="$(cd "$(dirname "$0")" && pwd)"
SKETCH="$(cd "${1:-$(dirname "$SIM")}" && pwd)"
NAME="$(basename "$SKETCH")"
OUT="$SIM/build/$NAME"
CLI="${ARDUINO_CLI:-/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli}"
FQBN="arduino:avr:mega:cpu=atmega2560"
# The PinNode library: installed in the Arduino libraries folder, or a checkout named here
PINNODE_LIB="${PINNODE_LIB:-}"
if [ -z "$PINNODE_LIB" ]; then
  for d in "$HOME/Documents/Arduino/libraries/PinNode"; do
    if [ -f "$d/src/PinNodeCtrl.cpp" ]; then PINNODE_LIB="$d"; break; fi
  done
fi
if [ ! -f "$PINNODE_LIB/src/PinNodeCtrl.cpp" ]; then
  echo "PinNode library not found: install it (Library Manager: PinNode) or set PINNODE_LIB" >&2; exit 1
fi
# The RPUMachines library (machine definitions): optional, for sketches that include one
MACHINES_LIB="${MACHINES_LIB:-}"
if [ -z "$MACHINES_LIB" ]; then
  for d in "$HOME/Documents/Arduino/libraries/RPUMachines"; do
    if [ -d "$d/src" ]; then MACHINES_LIB="$d"; break; fi
  done
fi
LIBFLAGS=(--library "$PINNODE_LIB")
INCFLAGS=(-I"$PINNODE_LIB/src")
HEADERS=("$SKETCH"/*.h "$SKETCH"/*.ino)
if [ -n "$MACHINES_LIB" ]; then
  LIBFLAGS+=(--library "$MACHINES_LIB"); INCFLAGS+=(-I"$MACHINES_LIB/src")
  # the log names a number from the first header that defines it: the machine library's
  # names lead when the sketch is built for one of its machines (SIM_DEFINES)
  case "${SIM_DEFINES:-}" in
    *MACHINE_*) HEADERS=("$MACHINES_LIB"/src/*.h "${HEADERS[@]}") ;;
    *) HEADERS+=("$MACHINES_LIB"/src/*.h) ;;
  esac
fi
mkdir -p "$OUT"

# 1. the sketch as one .cpp, with prototypes, exactly as the Arduino build sees it
# SIM_DEFINES="-DMACHINE_BALLY_STAR_TREK" builds the sketch for that machine
SIM_DEFINES="${SIM_DEFINES:-}"
"$CLI" compile -b "$FQBN" "${LIBFLAGS[@]}" ${SIM_DEFINES:+--build-property "build.extra_flags=$SIM_DEFINES"} --preprocess "$SKETCH" > "$OUT/sketch.cpp"

# 2. names for the log and the scenarios: every numeric SW_/SOL_/LAMP_/ACH_/STAT_/SET_/AUD_/ACT_ constant
python3 - "${HEADERS[@]}" > "$OUT/sim_names.inc" <<'PY'
import re, sys, glob, os
seen = set()
for path in sys.argv[1:]:
    if os.path.basename(path) in ('RPU.h', 'RPU_Config.h', 'RPULink.h', 'PinNodeCtrl.h', 'PinNode.h'):
        continue
    text = open(path, errors='replace').read()
    for m in re.finditer(r'^\s*#define\s+((?:SW|SOL|LAMP)_\w+)\s+(\d+|0x[0-9A-Fa-f]+)\b', text, re.M):
        if m.group(1) not in seen and 'ANIMATION' not in m.group(1):
            seen.add(m.group(1)); print('  {"%s", %s},' % (m.group(1), m.group(2)))
    for m in re.finditer(r'^\s*((?:ACH|STAT|SET|AUD|ACT)_\w+)\s*=\s*(\d+)\s*,', text, re.M):
        if m.group(1) not in seen:
            seen.add(m.group(1)); print('  {"%s", %s},' % (m.group(1), m.group(2)))
PY

# 3. compile: the mock Arduino headers first, then the sketch folder
# (the library's PinNodeCtrl.cpp is the real one; its RPULink is replaced by sim_core.cpp)
SRCS=("$OUT/sketch.cpp" "$SIM/sim_core.cpp" "$PINNODE_LIB/src/PinNodeCtrl.cpp")
for f in "$SKETCH"/*.cpp; do
  case "$(basename "$f")" in
    RPU.cpp|RPULink.cpp) ;;                 # replaced by sim_core.cpp
    *) SRCS+=("$f") ;;
  esac
done
clang++ -std=gnu++17 -g -O1 -w -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -DPINNODE_HOST_BUILD $SIM_DEFINES \
  -I"$SIM/mock" -I"$OUT" -I"$SKETCH" "${INCFLAGS[@]}" "${SRCS[@]}" -o "$OUT/sim"
echo "built $OUT/sim"
