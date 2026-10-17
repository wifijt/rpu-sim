#!/bin/sh
# Build the simulator for a sketch and run scenarios against it: see run.py.
#   run.sh path/to/MySketch scenario.sim [more.sim ...]
exec python3 "$(dirname "$0")/run.py" "$@"
