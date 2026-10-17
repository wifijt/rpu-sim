#!/usr/bin/env python3
"""Build the simulator for a sketch and run scenarios against it.

    python3 run.py path/to/MySketch scenario.sim [more.sim ...]

Set SIM_DEFINES (e.g. "-DMACHINE_BALLY_STAR_TREK") to build the sketch for one machine.
"""
import os
import subprocess
import sys
from pathlib import Path

SIM = Path(__file__).resolve().parent
if len(sys.argv) < 3:
    sys.exit("usage: run.py path/to/sketch scenario.sim [...]")
sketch = Path(sys.argv[1]).resolve()
if subprocess.call([sys.executable, str(SIM / "build.py"), str(sketch)]) != 0:
    sys.exit(1)
binary = SIM / "build" / sketch.name / ("sim.exe" if os.name == "nt" else "sim")
failed = 0
for f in sys.argv[2:]:
    if os.path.basename(f) == "machine.sim":
        continue
    failed |= subprocess.call([str(binary), f]) != 0
sys.exit(1 if failed else 0)
