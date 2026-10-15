#!/usr/bin/env python3
"""Play an RPU game in your browser: no Mega, no machine.

    ./play.py path/to/MyGame [--machine bally-star-trek-1979] [--manual manual.pdf] [--port 8765]

Builds the game (build.sh), runs it in real time (sim --live), and serves a page at
http://localhost:8765 with the backglass displays, the playfield, the lamps, and switches you click.
Keys: 1 Start, 5 Coin, Space or Enter drain.

The machine (its names and its layout) comes from RPUMachines: the library folder in MACHINES_LIB,
or the one the Arduino IDE installed. Without --machine it's the machine header the game includes.
Saved layouts go back into RPUMachines when MACHINES_LIB is a git clone, else into machines/<id>/.
"""
import argparse, json, os, queue, subprocess, sys, threading, webbrowser
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from pathlib import Path

HERE = Path(__file__).resolve().parent
ap = argparse.ArgumentParser()
ap.add_argument("sketch")
ap.add_argument("--machine", help="the machine's id in RPUMachines (e.g. bally-star-trek-1979), or a folder with layout.json")
ap.add_argument("--manual", help="your copy of the machine's manual (PDF): its playfield drawing becomes the background")
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

mid = machine_id()
legacy = mid and Path(mid).is_dir()                     # a folder with layout.json (and machine.sim)
mdir = Path(mid).resolve() if legacy else (HERE / "machines" / mid if mid else None)
mid = mdir.name if legacy else mid
if subprocess.call([str(HERE / "build.sh"), str(sketch)]) != 0:
    sys.exit("build failed")
binary = HERE / "build" / sketch.name / "sim"
cmd = [str(binary), "--live"] + ([str(mdir / "machine.sim")] if mdir and (mdir / "machine.sim").exists() else [])
if mdir and not (mdir / "machine.sim").exists():
    print(f"no {mdir / 'machine.sim'}: the ball won't start in the outhole (see README: Your own game)")
sim = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1,
                       cwd=str(mdir if mdir and mdir.is_dir() else HERE))

names = json.loads(sim.stdout.readline())        # the first line: SW_/SOL_/LAMP_/... names
layout, layout_path = {}, None
for cand in ([mdir / "layout.json"] if mdir else []) + ([lib / "layouts" / f"{mid}.json"] if mid and not legacy else []):
    if cand.exists():
        layout, layout_path = json.loads(cand.read_text()), cand
        break
# where a saved layout goes: back into RPUMachines if that's a git clone, else next to the sim
save_path = (lib / "layouts" / f"{mid}.json") if (mid and not legacy and (lib / ".git").exists()) else (mdir / "layout.json" if mdir else None)
if layout_path:
    print(f"layout: {layout_path}")
background = None                                 # the manual's playfield, rendered here, never stored in the repo
crop = None                                       # the playfield alone, for the Lamp Animator export
if a.manual and layout.get("manual"):
    out = HERE / "build" / sketch.name
    out.mkdir(parents=True, exist_ok=True)
    m, pdf = layout["manual"], str(Path(a.manual).resolve())
    render = ["swift", str(HERE / "tools" / "render_page.swift"), pdf, str(m["page"])]
    if subprocess.call(render + [str(out / "manual-page.png"), "3"]) == 0:
        background = (out / "manual-page.png").read_bytes()
        r = subprocess.run(render + [str(out / "playfield.png"), "2"] + [str(v) for v in m["frame"]], capture_output=True, text=True)
        if r.returncode == 0:
            w, h = (int(v) for v in r.stdout.split()[:2])
            crop = {"png": (out / "playfield.png").read_bytes(), "w": w, "h": h}
    else:
        print("couldn't render the manual page; using the plain drawing")
labels = {}                                       # the manufacturer's names, from RPUMachines
if mid and (lib / "names" / f"{mid}.json").exists():
    labels = json.loads((lib / "names" / f"{mid}.json").read_text())
elif layout.get("names") and (lib / "names" / layout["names"]).exists():
    labels = json.loads((lib / "names" / layout["names"]).read_text())

clients, lock = [], threading.Lock()
def pump():
    for line in sim.stdout:
        with lock:
            for q in list(clients): q.put(line)
    with lock:
        for q in list(clients): q.put(None)
threading.Thread(target=pump, daemon=True).start()

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
                                       "background": background is not None, "machine": mid,
                                       "saveTo": str(save_path) if save_path else None,
                                       "crop": {"w": crop["w"], "h": crop["h"]} if crop else None}).encode(), "application/json")
        elif self.path == "/playfield.png" and crop:
            self.send(200, crop["png"], "image/png")
        elif self.path == "/background.png" and background:
            self.send(200, background, "image/png")
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
