#!/bin/sh
# Build the host simulator for an RPU sketch: see build.py (this is the same, for the shell).
#   build.sh path/to/MySketch     -> build/MySketch/sim
exec python3 "$(dirname "$0")/build.py" "$@"
