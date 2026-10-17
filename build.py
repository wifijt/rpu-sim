#!/usr/bin/env python3
"""Build the host simulator for an RPU sketch: build/<Sketch>/sim (sim.exe on Windows).

    python3 build.py path/to/MySketch
    (no argument: the folder above this one, for rpu-sim kept inside a sketch as sim/)

Needs the Arduino IDE (its arduino-cli adds the sketch's function prototypes), a C++ compiler
(clang++ or g++; set CXX to choose) and Python 3. Environment:
  ARDUINO_CLI   arduino-cli, if it isn't where the Arduino IDE puts it
  PINNODE_LIB   a PinNode library checkout (default: the one in your Arduino libraries folder)
  MACHINES_LIB  an RPUMachines checkout (default: the one in your Arduino libraries folder)
  SIM_DEFINES   e.g. "-DMACHINE_BALLY_STAR_TREK": build the sketch for one machine
  SIM_WATCH     e.g. "GameMode,BonusX": game variables sim --live reports
  SIM_SANITIZE  0: build without the address/undefined-behaviour checks (always off on Windows)
"""
import glob
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

SIM = Path(__file__).resolve().parent
WINDOWS = os.name == "nt"
EXE = ".exe" if WINDOWS else ""
FQBN = "arduino:avr:mega:cpu=atmega2560"


def fail(msg):
    sys.exit(msg)


def find_cli():
    if os.environ.get("ARDUINO_CLI"):
        return os.environ["ARDUINO_CLI"]
    rel = Path("resources/app/lib/backend/resources")
    candidates = [Path("/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli")]
    for base in (os.environ.get("ProgramFiles"), os.environ.get("LOCALAPPDATA") and Path(os.environ["LOCALAPPDATA"]) / "Programs"):
        if base:
            # the arduino.cc installer uses "Arduino IDE"; winget's package installs to "arduino-ide"
            candidates += [Path(base) / d / rel / "arduino-cli.exe" for d in ("Arduino IDE", "arduino-ide")]
    candidates.append(Path.home() / "Applications/arduino-ide" / rel / "arduino-cli")   # Linux AppImage unpacked
    for c in candidates:
        if c.exists():
            return str(c)
    found = shutil.which("arduino-cli")
    if found:
        return found
    fail("arduino-cli not found: install the Arduino IDE, or set ARDUINO_CLI to arduino-cli's path")


def libraries_dir(cli):
    """The Arduino libraries folder, as arduino-cli knows it (Windows may keep Documents in OneDrive)."""
    try:
        out = subprocess.run([cli, "config", "get", "directories.user"], capture_output=True, text=True, timeout=30)
        user = out.stdout.strip()
        if out.returncode == 0 and user:
            return Path(user) / "libraries"
    except (OSError, subprocess.SubprocessError):
        pass
    return Path.home() / "Documents" / "Arduino" / "libraries"


def find_cxx():
    if os.environ.get("CXX"):
        return os.environ["CXX"]
    for c in ("clang++", "g++"):
        if shutil.which(c):
            return c
    fail("no C++ compiler: install clang++ or g++ (on Windows, MinGW-w64, e.g. WinLibs), or set CXX")


def includes(files, pattern):
    rx = re.compile(r'#include\s*[<"]' + pattern + r'[>"]')
    for f in files:
        try:
            if rx.search(Path(f).read_text(errors="replace")):
                return True
        except OSError:
            pass
    return False


def names(headers):
    """Every numeric SW_/SOL_/LAMP_/ACH_/STAT_/SET_/AUD_/ACT_ constant, for the log and the scenarios."""
    seen, lines = set(), []
    for path in headers:
        if os.path.basename(path) in ('RPU.h', 'RPU_Config.h', 'RPULink.h', 'PinNodeCtrl.h', 'PinNode.h'):
            continue
        text = Path(path).read_text(errors='replace')
        for m in re.finditer(r'^\s*#define\s+((?:SW|SOL|LAMP)_\w+)\s+(\d+|0x[0-9A-Fa-f]+)\b', text, re.M):
            if m.group(1) not in seen and 'ANIMATION' not in m.group(1):
                seen.add(m.group(1)); lines.append('  {"%s", %s},' % (m.group(1), m.group(2)))
        for m in re.finditer(r'^\s*#define\s+((?:MACHINE_STATE|GAME_MODE)_\w+)\s+\(?(-?\d+)\)?\s*(?://.*)?$', text, re.M):
            if m.group(1) not in seen:
                seen.add(m.group(1)); lines.append('  {"%s", %s},' % (m.group(1), m.group(2)))
        for m in re.finditer(r'^\s*((?:ACH|STAT|SET|AUD|ACT)_\w+)\s*=\s*(\d+)\s*,', text, re.M):
            if m.group(1) not in seen:
                seen.add(m.group(1)); lines.append('  {"%s", %s},' % (m.group(1), m.group(2)))
    return "\n".join(lines) + ("\n" if lines else "")


def watch_block(watch):
    """SIM_WATCH: a function appended to the sketch, so the variables are in scope with their own types.
    Always defined (empty without a watch list): sim_core calls it."""
    if not watch:
        return '#include <string>\nvoid SimWatchValues(std::string &) {}\n'
    out = ['#include <string>', '#include <type_traits>',
           'template <class T> static std::string SimFmt(const T &v) {',
           '  if constexpr (std::is_array_v<T>) { std::string s = "["; for (size_t i = 0; i < std::extent_v<T> && i < 8; i++) { if (i) s += ","; s += SimFmt(v[i]); } return s + (std::extent_v<T> > 8 ? ",…]" : "]"); }',
           '  else if constexpr (std::is_enum_v<T>) return std::to_string((long long)v);',
           '  else if constexpr (std::is_arithmetic_v<T>) return std::to_string(v);',
           '  else return "?";', '}', 'void SimWatchValues(std::string &o) {']
    for w in watch.split(","):
        w = w.strip()
        if not w:
            continue
        if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', w):
            fail("SIM_WATCH: '%s' isn't a variable name" % w)
        out.append('  o += "\\"%s\\":\\"" + SimFmt(%s) + "\\",";' % (w, w))
    out.append('}')
    return "\n".join(out) + "\n"


def main():
    sketch = Path(sys.argv[1] if len(sys.argv) > 1 else SIM.parent).resolve()
    if not sketch.is_dir():
        fail("not a sketch folder: %s" % sketch)
    name = sketch.name
    out = SIM / "build" / name
    sketch_files = sorted(glob.glob(str(sketch / "*.ino")) + glob.glob(str(sketch / "*.h")))
    cli = find_cli()
    libs = libraries_dir(cli)

    # PinNode: only for sketches that include PinNode.h; then its link is simulated
    uses_pinnode = includes(sketch_files, r'PinNode\.h')
    pinnode_lib = os.environ.get("PINNODE_LIB") or ""
    if uses_pinnode:
        if not pinnode_lib and (libs / "PinNode" / "src" / "PinNodeCtrl.cpp").exists():
            pinnode_lib = str(libs / "PinNode")
        if not (Path(pinnode_lib) / "src" / "PinNodeCtrl.cpp").exists():
            fail("This game uses PinNode: install the PinNode library (Library Manager) or set PINNODE_LIB")
    # RPUMachines (machine definitions): optional, for sketches that include one
    machines_lib = os.environ.get("MACHINES_LIB") or ""
    if not machines_lib and (libs / "RPUMachines" / "src").is_dir():
        machines_lib = str(libs / "RPUMachines")

    lib_flags, inc_flags, sim_flags = [], [], []
    if uses_pinnode:
        lib_flags += ["--library", pinnode_lib]
        inc_flags += ["-I" + str(Path(pinnode_lib) / "src")]
        sim_flags += ["-DRPU_SIM_PINNODE", "-DPINNODE_HOST_BUILD"]
    headers = list(sketch_files)
    sim_defines = os.environ.get("SIM_DEFINES", "")
    if machines_lib:
        lib_flags += ["--library", machines_lib]
        inc_flags += ["-I" + str(Path(machines_lib) / "src")]
        # only the machine headers this sketch includes; their names lead when the sketch is built for
        # one of its machines (SIM_DEFINES)
        mheaders = [h for h in sorted(glob.glob(str(Path(machines_lib) / "src" / "*.h")))
                    if includes(sketch_files, re.escape(os.path.basename(h)))]
        headers = mheaders + headers if "MACHINE_" in sim_defines else headers + mheaders
    out.mkdir(parents=True, exist_ok=True)

    # 1. the sketch as one .cpp, with prototypes, exactly as the Arduino build sees it
    cmd = [cli, "compile", "-b", FQBN] + lib_flags
    if sim_defines:
        cmd += ["--build-property", "build.extra_flags=" + sim_defines]
    cmd += ["--preprocess", str(sketch)]
    pre = subprocess.run(cmd, capture_output=True, text=True)
    if pre.returncode != 0:
        sys.stderr.write(pre.stdout + pre.stderr)
        fail("arduino-cli couldn't preprocess the sketch (is the Arduino AVR core installed? Boards Manager: Arduino AVR Boards)")
    # 2. names, and 2b. the watch list, appended to the sketch itself
    (out / "sim_names.inc").write_text(names(headers))
    (out / "sketch.cpp").write_text(pre.stdout + watch_block(os.environ.get("SIM_WATCH", "")))

    # 3. compile: the mock Arduino headers first, then the sketch folder
    # (the library's PinNodeCtrl.cpp is the real one; RPU.cpp and its RPULink are replaced by sim_core.cpp)
    srcs = [str(out / "sketch.cpp"), str(SIM / "sim_core.cpp")]
    if uses_pinnode:
        srcs.append(str(Path(pinnode_lib) / "src" / "PinNodeCtrl.cpp"))
    srcs += [f for f in sorted(glob.glob(str(sketch / "*.cpp"))) if os.path.basename(f) not in ("RPU.cpp", "RPULink.cpp")]
    cxx = find_cxx()
    flags = ["-std=gnu++17", "-g", "-O1", "-w"]
    if WINDOWS:
        flags += ["-static"]          # one sim.exe, no MinGW DLLs to carry around
    if not WINDOWS and os.environ.get("SIM_SANITIZE", "1") != "0":   # MinGW has no sanitizers
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]   # out-of-range reads and the like stop the run
    binary = out / ("sim" + EXE)
    cmd = shlex.split(cxx, posix=not WINDOWS) + flags + sim_flags + sim_defines.split() + ["-I" + str(SIM / "mock"), "-I" + str(out), "-I" + str(sketch)] \
        + inc_flags + srcs + ["-o", str(binary)]
    if subprocess.call(cmd) != 0:
        fail("compile failed")
    print("built %s" % binary)


if __name__ == "__main__":
    main()
