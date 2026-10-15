#!/usr/bin/env python3
"""Play an RPU game in your browser: no Mega, no machine.

    ./play.py path/to/MyGame [--machine machines/bally-star-trek-1979] [--port 8765]

Builds the game (build.sh), runs it in real time (sim --live), and serves a page at
http://localhost:8765 with the backglass displays, the playfield drawn from the machine's
layout.json, the lamps, and switches you click. Keys: 1 Start, 5 Coin, Space or Enter drain.
"""
import argparse, json, os, queue, subprocess, sys, threading, webbrowser
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from pathlib import Path

HERE = Path(__file__).resolve().parent
ap = argparse.ArgumentParser()
ap.add_argument("sketch")
ap.add_argument("--machine", help="folder with layout.json and machine.sim (see machines/)")
ap.add_argument("--port", type=int, default=8765)
ap.add_argument("--no-browser", action="store_true")
a = ap.parse_args()
sketch = Path(a.sketch).resolve()
mdir = Path(a.machine).resolve() if a.machine else None

if subprocess.call([str(HERE / "build.sh"), str(sketch)]) != 0:
    sys.exit("build failed")
binary = HERE / "build" / sketch.name / "sim"
cmd = [str(binary), "--live"] + ([str(mdir / "machine.sim")] if mdir and (mdir / "machine.sim").exists() else [])
sim = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1, cwd=str(mdir or HERE))

names = json.loads(sim.stdout.readline())        # the first line: SW_/SOL_/LAMP_/... names
layout = {}
if mdir and (mdir / "layout.json").exists():
    layout = json.loads((mdir / "layout.json").read_text())
labels = {}                                       # the manufacturer's names, from RPUMachines
lib = os.environ.get("MACHINES_LIB") or str(Path.home() / "Documents/Arduino/libraries/RPUMachines")
if layout.get("names") and (Path(lib) / "names" / layout["names"]).exists():
    labels = json.loads((Path(lib) / "names" / layout["names"]).read_text())

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
            self.send(200, json.dumps({"names": names, "layout": layout, "labels": labels, "game": sketch.name}).encode(), "application/json")
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
