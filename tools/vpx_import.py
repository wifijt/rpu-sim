#!/usr/bin/env python3
"""Place a machine's lamps (and check its switches) from a Visual Pinball table.

    tools/vpx_import.py path/to/extracted-table [--machine bally-star-trek-1979] [--write] [--replace]

Extract the table first (vpxtool extract MyTable.vpx). Its gameitems/ folder has every light and
switch with its position; lights are named in PinMAME's numbering (l1, l2 ...) and switches swNN.
This turns those into RPU numbers, fits the table's coordinates to the layout's frame using the
switches both have, and then:
  - reports every lamp and switch, and how far the table's position is from the layout's;
  - with --write, adds the lamps the layout doesn't have yet (your placements are kept);
  - with --replace, also moves lamps you already placed to the table's positions.
The layout is RPUMachines' layouts/<machine>.json (MACHINES_LIB, or the Arduino IDE's copy).

Tables are made by hand and sometimes number a lamp differently from the manufacturer; check the
report before trusting a lamp the table disagrees about.
"""
import argparse
import glob
import json
import math
import os
from pathlib import Path


def rpu_lamp(pinmame: int, platform: str) -> int:
    if platform == "bally":                       # Bally/Stern (by35): PinMAME lamp n -> RPU lamp index
        return 4 * ((pinmame - 1) % 16) + (pinmame - 1) // 16
    return pinmame - 1                            # Williams System 3-7: lamps 1-64 -> 0-63


def fit(pairs):
    """Least squares for x' = a x + b and y' = c y + d over [(x, y, x', y')]."""
    def line(us, vs):
        n = len(us); mu, mv = sum(us) / n, sum(vs) / n
        su = sum((u - mu) ** 2 for u in us)
        a = sum((u - mu) * (v - mv) for u, v in zip(us, vs)) / su if su else 1.0
        return a, mv - a * mu
    a, b = line([p[0] for p in pairs], [p[2] for p in pairs])
    c, d = line([p[1] for p in pairs], [p[3] for p in pairs])
    return a, b, c, d


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("table", help="folder from vpxtool extract")
    ap.add_argument("--machine", help="RPUMachines id (default: the one the layout folder has, if only one matches)")
    ap.add_argument("--write", action="store_true", help="add the lamps the layout doesn't have")
    ap.add_argument("--replace", action="store_true", help="also move lamps already placed")
    a = ap.parse_args()

    t = Path(a.table)
    lib = Path(os.environ.get("MACHINES_LIB") or Path.home() / "Documents/Arduino/libraries/RPUMachines")
    mid = a.machine or (sorted(p.stem for p in (lib / "layouts").glob("*.json")) or [None])[0]
    lay_path, names_path = lib / "layouts" / f"{mid}.json", lib / "names" / f"{mid}.json"
    layout, names = json.loads(lay_path.read_text()), json.loads(names_path.read_text())
    platform = "williams" if "williams" in names["machine"].lower() else "bally"
    label = {k: {e["n"]: e["name"] for e in names[k]} for k in ("lamps", "switches")}

    g = json.loads((t / "gamedata.json").read_text())
    W, H = g["right"] - g["left"], g["bottom"] - g["top"]
    lamps, switches = {}, {}
    for f in glob.glob(str(t / "gameitems" / "*.json")):
        d = json.loads(Path(f).read_text())
        kind, item = next(iter(d.items()))
        name = item.get("name", "")
        c = item.get("center") or ({"x": item["vertex"]["x"], "y": item["vertex"]["y"]} if "vertex" in item else None)
        if not c:
            continue
        x, y = (c["x"] - g["left"]) / W, (c["y"] - g["top"]) / H
        if kind == "Light" and name[:1] == "l" and name[1:].isdigit() and not item.get("is_backglass"):
            if 0 <= x <= 1 and 0 <= y <= 1:       # off the playfield: a backglass lamp
                lamps[rpu_lamp(int(name[1:]), platform)] = (x, y, name)
        elif kind in ("Trigger", "HitTarget", "Kicker", "Rubber") and name[:2] == "sw" and name[2:].isdigit():
            switches.setdefault(int(name[2:]) - 1, (x, y, name))

    items = layout["items"]
    have_sw = {i["n"]: i for i in items if i["kind"] == "switch"}
    have_lamp = {i["n"]: i for i in items if i["kind"] == "lamp"}
    # Line the table up on lamps you've placed (they sit on the inserts), dropping the ones that disagree
    # most; with fewer than 4, on the switches instead (their hotspots may sit beside the part).
    lp = [(x, y, have_lamp[n]["x"], have_lamp[n]["y"]) for n, (x, y, _) in lamps.items() if n in have_lamp]
    lp += [(x, y, have_sw[n]["x"], have_sw[n]["y"]) for n, (x, y, _) in switches.items() if n in have_sw]   # spread wide: they fix the scale
    asp = layout.get("aspect", 2)
    for _ in range(3):
        if len(lp) < 4:
            break
        A0, B0, C0, D0 = fit(lp)
        gap = lambda p: math.hypot(A0 * p[0] + B0 - p[2], (C0 * p[1] + D0 - p[3]) * asp)
        keep = [p for p in lp if gap(p) < max(0.04, 2.5 * sorted(map(gap, lp))[len(lp) // 2])]
        if len(keep) == len(lp):
            break
        lp = keep
    pairs = lp if len(lp) >= 4 else [(x, y, have_sw[n]["x"], have_sw[n]["y"]) for n, (x, y, _) in switches.items() if n in have_sw]
    on = "placed lamps and switches" if len(lp) >= 4 else "switches"
    if len(pairs) < 3:
        print("too few switches in common to line the table up; using its coordinates as they are")
        A, B, C, D = 1, 0, 1, 0
    else:
        A, B, C, D = fit(pairs)
        resid = sorted(math.hypot(A * x + B - u, (C * y + D - v) * layout.get("aspect", 2)) for x, y, u, v in pairs)
        print(f"lined up on {len(pairs)} {on}: x' = {A:.3f}x {B:+.3f}, y' = {C:.3f}y {D:+.3f}; "
              f"median gap {100 * resid[len(resid) // 2]:.1f}% of the width")
        far = [(n, label['switches'].get(n, ''), 100 * math.hypot(A * x + B - have_sw[n]['x'], (C * y + D - have_sw[n]['y']) * layout.get('aspect', 2)))
               for n, (x, y, _) in switches.items() if n in have_sw]
        for n, nm, d in sorted(far, key=lambda r: -r[2])[:5]:
            if d > 6:
                print(f"  switch {n} {nm}: {d:.0f}% of the width from the layout's hotspot (fine if the hotspot sits on the manual's number label)")

    have = {i["n"]: i for i in items if i["kind"] == "lamp"}
    added = moved = 0
    print("\nlamps (RPU number, name, table light):")
    for n in sorted(lamps):
        x, y, tname = lamps[n]
        nx, ny = round(A * x + B, 4), round(C * y + D, 4)
        nm = label["lamps"].get(n, "?")
        if n in have:
            d = 100 * math.hypot(nx - have[n]["x"], (ny - have[n]["y"]) * layout.get("aspect", 2))
            note = "matches" if d < 4 else f"YOURS DIFFERS by {d:.0f}% of the width"
            if a.replace and d >= 1:
                have[n]["x"], have[n]["y"] = nx, ny; moved += 1; note += " -> moved"
            print(f"  {n:2} {nm:36} {tname:4} placed, {note}")
        else:
            if n not in label["lamps"]:
                print(f"  {n:2} {'(not in the names file)':36} {tname:4} skipped")
                continue
            print(f"  {n:2} {nm:36} {tname:4} new at ({nx:.3f}, {ny:.3f})")
            if a.write or a.replace:
                items.append({"kind": "lamp", "n": n, "x": nx, "y": ny}); added += 1
    missing = sorted(set(label["lamps"]) - set(lamps) - set(have))
    if missing:
        print("\nnot on the table's playfield (backbox lamps, or numbered differently):",
              ", ".join(f"{n} {label['lamps'][n]}" for n in missing))
    if a.write or a.replace:
        lay_path.write_text(json.dumps(layout, indent=1) + "\n")
        print(f"\nwrote {lay_path}: {added} lamps added, {moved} moved")
    else:
        print("\n(report only: --write adds the new lamps)")


if __name__ == "__main__":
    main()
