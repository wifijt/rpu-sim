#!/usr/bin/env python3
"""Play an RPU game in your browser: no Mega, no machine.

    ./play.py path/to/MyGame [--image playfield.jpg] [--sounds folder] [--machine bally-star-trek-1979] [--port 8765]

Builds the game (build.sh), runs it in real time (sim --live), and serves a page at
http://localhost:8765 with the backglass displays, the playfield, the lamps, and switches you click.
Keys: 1 Start, 5 Coin, Space or Enter drain. --sounds plays the game's WAV files in the browser.

The machine (its names and its layout) comes from RPUMachines: the library folder in MACHINES_LIB,
or the one the Arduino IDE installed. Without --machine it's the machine header the game includes.
Saved layouts go to ~/Documents/rpu-sim/layouts, or into RPUMachines when MACHINES_LIB is a git clone.
"""
import argparse, json, os, queue, subprocess, sys, threading, webbrowser
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from pathlib import Path

HERE = Path(__file__).resolve().parent
ap = argparse.ArgumentParser()
ap.add_argument("sketch")
ap.add_argument("--machine", help="the machine's id in RPUMachines (e.g. bally-star-trek-1979), or a folder with layout.json")
ap.add_argument("--image", help="a picture of the playfield (e.g. extracted from a VPX table) to use as the background; line it up once with Align image")
ap.add_argument("--sounds", help="the game's WAV Trigger folder (files named by track number: 0201_x.wav, 201.wav); your browser plays them")
ap.add_argument("--watch", help="game variables to show live, e.g. GameMode,BonusX (remembered for next time)")
ap.add_argument("--port", type=int, default=8765)
ap.add_argument("--no-browser", action="store_true")
a = ap.parse_args()
sketch = Path(a.sketch).resolve()
lib = Path(os.environ.get("MACHINES_LIB") or Path.home() / "Documents/Arduino/libraries/RPUMachines")

def machine_id():
    """--machine, or the RPUMachines header this sketch includes."""
    if a.machine:
        return a.machine
    import re
    includes = set()
    for f in list(sketch.glob("*.ino")) + list(sketch.glob("*.h")):
        includes |= set(re.findall(r'#include\s*[<"]([^>"]+)[>"]', f.read_text(errors="replace")))
    for n in sorted((lib / "names").glob("*.json")):
        try:
            if json.loads(n.read_text()).get("header") in includes:
                return n.stem
        except (json.JSONDecodeError, OSError):
            pass
    return None

mid = machine_id() or sketch.name.lower()
legacy = mid and Path(mid).is_dir()                     # a folder with layout.json (and machine.sim)
mdir = Path(mid).resolve() if legacy else (HERE / "machines" / mid if mid else None)
mid = mdir.name if legacy else mid
watch_file = HERE / "build" / sketch.name / "watch.txt"
watch = a.watch if a.watch is not None else (watch_file.read_text().strip() if watch_file.exists() else "")

def build():
    env = dict(os.environ, SIM_WATCH=watch)
    return subprocess.call([str(HERE / "build.sh"), str(sketch)], env=env) == 0

if not build():
    sys.exit("build failed")
watch_file.write_text(watch)
binary = HERE / "build" / sketch.name / "sim"
# Layouts you're working on live outside every repo; they load before RPUMachines' copy
local_layouts = Path(os.environ.get("RPU_SIM_LAYOUTS") or Path.home() / "Documents/rpu-sim/layouts")
layout, layout_path = {}, None
for cand in ([mdir / "layout.json"] if legacy else []) + [local_layouts / f"{mid}.json"] + ([lib / "layouts" / f"{mid}.json"] if not legacy else []):
    if cand.exists():
        layout, layout_path = json.loads(cand.read_text()), cand
        break
# where a saved layout goes: straight into RPUMachines if that's a git clone (ready for a pull request),
# else your local layouts folder (copy it into RPUMachines to contribute it)
save_path = (mdir / "layout.json") if legacy else (lib / "layouts" / f"{mid}.json") if (lib / ".git").exists() else (local_layouts / f"{mid}.json")
if layout_path:
    print(f"layout: {layout_path}")

def setup_file():
    """What the playfield does on its own: the layout's physics, else a machines/<id>/machine.sim."""
    if layout.get("physics") or layout.get("ballStarts"):
        out = HERE / "build" / sketch.name / "machine.sim"
        out.parent.mkdir(parents=True, exist_ok=True)
        lines = [f"physics {p['coil']} opens {' '.join(str(w) for w in p['opens'])}" for p in layout.get("physics", [])]
        lines += [f"close {w}" for w in layout.get("ballStarts", [])]
        out.write_text("\n".join(lines) + "\n")
        return out
    if mdir and (mdir / "machine.sim").exists():
        return mdir / "machine.sim"
    print("this layout has no physics: the ball won't start in the outhole (add physics in RPUMachines, see its README)")
    return None

def start_sim():
    setup = setup_file()
    cmd = [str(binary), "--live"] + ([str(setup)] if setup else [])
    return subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1,
                            cwd=str(mdir if mdir and mdir.is_dir() else HERE))
sim = start_sim()
names = json.loads(sim.stdout.readline())        # the first line: SW_/SOL_/LAMP_/... names

wavlens = []                                      # "wavlen TRACK MS" lines, sent again after a reset
sounds = {}                                       # track number -> file, from the WAV Trigger folder
if a.sounds:
    import re, wave
    for f in sorted(Path(a.sounds).expanduser().glob("*")):
        m = re.match(r"(\d+)", f.name)
        if m and f.suffix.lower() == ".wav":
            t = int(m.group(1))
            sounds.setdefault(t, f)
            try:                                  # the game hears each track end when the real one would
                with wave.open(str(f)) as w:
                    wavlens.append(f"wavlen {t} {int(1000 * w.getnframes() / w.getframerate())}\n")
                    sim.stdin.write(wavlens[-1])
            except (wave.Error, EOFError, OSError):
                pass
    sim.stdin.flush()
    print(f"sounds: {len(sounds)} tracks from {a.sounds}")
pfimage = None                                    # your own playfield picture: stays on your Mac, alignment saved beside it
if a.image:
    ip = Path(a.image).expanduser().resolve()
    ctype = {".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".webp": "image/webp"}.get(ip.suffix.lower())
    if not ip.exists() or not ctype:
        sys.exit(f"--image: {ip} isn't a PNG, JPEG or WebP file")
    align_path = ip.with_name(ip.name + ".align.json")
    align = json.loads(align_path.read_text()) if align_path.exists() else {"rect": [0, 0, 1, 1]}
    pfimage = {"bytes": ip.read_bytes(), "type": ctype, "align": align, "align_path": align_path}

labels = {}                                       # the manufacturer's names, from RPUMachines
if mid and (lib / "names" / f"{mid}.json").exists():
    labels = json.loads((lib / "names" / f"{mid}.json").read_text())
elif layout.get("names") and (lib / "names" / layout["names"]).exists():
    labels = json.loads((lib / "names" / layout["names"]).read_text())

clients, lock = [], threading.Lock()
def pump(proc):
    for line in proc.stdout:
        if proc is not sim:                      # a reset replaced it
            return
        with lock:
            for q in list(clients): q.put(line)
threading.Thread(target=pump, args=(sim,), daemon=True).start()

def reset():
    """Restart the game, like switching the machine off and on (EEPROM starts fresh too)."""
    global sim
    old = sim
    sim = start_sim()
    old.terminate()
    sim.stdout.readline()                        # the names line
    for l in wavlens: sim.stdin.write(l)
    sim.stdin.flush()
    threading.Thread(target=pump, args=(sim,), daemon=True).start()

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body, ctype):
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Cache-Control", "no-store")
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self.send(200, (HERE / "web" / "index.html").read_bytes(), "text/html; charset=utf-8")
        elif self.path == "/setup":
            self.send(200, json.dumps({"names": names, "layout": layout, "labels": labels, "game": sketch.name,
                                       "machine": mid,
                                       "saveTo": str(save_path) if save_path else None,
                                       "sounds": sorted(sounds), "watch": watch,
                                       "image": pfimage["align"] if pfimage else None}).encode(), "application/json")
        elif self.path.startswith("/pfimage") and pfimage:
            self.send(200, pfimage["bytes"], pfimage["type"])
        elif self.path.startswith("/sound/"):
            try:
                f = sounds.get(int(self.path.split("/")[2].split(".")[0]))
            except ValueError:
                f = None
            if f:
                self.send(200, f.read_bytes(), "audio/wav")
            else:
                self.send(404, b"no such track", "text/plain")
        elif self.path == "/events":
            self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.send_header("Cache-Control", "no-store"); self.end_headers()
            q = queue.Queue(maxsize=200)
            with lock: clients.append(q)
            try:
                while True:
                    line = q.get()
                    if line is None: break
                    self.wfile.write(b"data: " + line.strip().encode() + b"\n\n"); self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                with lock: clients.remove(q)
        else:
            self.send(404, b"not found", "text/plain")
    def do_POST(self):
        if self.path == "/layout":
            if not save_path:
                return self.send(400, b"no machine: start play.py with --machine to save a layout", "text/plain")
            body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            try:
                new = json.loads(body)
                assert isinstance(new.get("items"), list)
            except Exception:
                return self.send(400, b"not a layout", "text/plain")
            save_path.parent.mkdir(parents=True, exist_ok=True)
            save_path.write_text(json.dumps(new, indent=1) + "\n")
            layout.clear(); layout.update(new)
            return self.send(204, b"", "text/plain")
        if self.path == "/imagealign" and pfimage:
            try:
                new = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))))
                r = [float(v) for v in new["rect"]]
                assert len(r) == 4 and r[0] < r[2] and r[1] < r[3]
            except Exception:
                return self.send(400, b"not an alignment", "text/plain")
            pfimage["align"] = {"rect": [round(v, 5) for v in r]}
            pfimage["align_path"].write_text(json.dumps(pfimage["align"]) + "\n")
            return self.send(204, b"", "text/plain")
        if self.path == "/watch":
            global watch
            import re
            wanted = ",".join(w for w in re.split(r"[\s,]+", self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()) if w)
            if wanted and not re.fullmatch(r"[A-Za-z_]\w*(,[A-Za-z_]\w*)*", wanted):
                return self.send(400, b"variable names only, separated by commas", "text/plain")
            old, watch = watch, wanted
            if not build():
                watch = old
                build()
                return self.send(400, b"didn't compile: is every name one of the game's global variables?", "text/plain")
            watch_file.write_text(watch)
            with lock:
                reset()
            return self.send(204, b"", "text/plain")
        if self.path == "/reset":
            with lock:
                reset()
            return self.send(204, b"", "text/plain")
        if self.path == "/cmd":
            line = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode().strip().splitlines()[0][:200]
            sim.stdin.write(line + "\n"); sim.stdin.flush()
            self.send(204, b"", "text/plain")
        else:
            self.send(404, b"not found", "text/plain")

srv = ThreadingHTTPServer(("127.0.0.1", a.port), H)
url = f"http://localhost:{a.port}"
print(f"{sketch.name} is running: {url}  (Ctrl-C to stop)")
if not a.no_browser: webbrowser.open(url)
try:
    srv.serve_forever()
except KeyboardInterrupt:
    pass
finally:
    sim.terminate()
