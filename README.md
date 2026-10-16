# rpu-sim

![Star Trek 2022 running in rpu-sim: backglass, playfield with lamps lit, watch and lamp panels](docs/screenshot.jpg)

Play and test a RetroPinUpgrade (RPU) game on your Mac, with no Mega and no machine. You get the
backglass, the playfield with its lamps lighting, switches you click, coils firing and the game's
own sounds. Edit your game, run it again, and see what it does.

## How it works

```
your game (.ino, .cpp) ──clang++──▶ build/<game>/sim ◀──play.py──▶ the page in your browser
                                    your game's own code,          lamps, displays, sounds;
                                    sim_core.cpp in place          your clicks go back
                                    of the hardware
```

`sim_core.cpp` stands in for `RPU.cpp` (switches, lamps, displays, coils), the Arduino core, the WAV
Trigger and, if the game uses it, the PinNode link. Everything else is the game's own code,
unchanged. Nothing is written to the game's folder.

What it knows about each machine (lamp and switch names, where they sit, which coil opens which
switch) comes from [RPUMachines](https://github.com/wifijt/RPUMachines).

## Install

rpu-sim runs on a Mac.

1. Apple's Command Line Tools, for `clang++` (not the full Xcode app): `xcode-select --install`.
   `xcode-select -p` shows whether you already have them.
2. The [Arduino IDE](https://www.arduino.cc/en/software). rpu-sim uses its `arduino-cli`; set
   `ARDUINO_CLI` if yours is somewhere else. Python 3 comes with macOS.
3. In the IDE's Library Manager: **RPUMachines**, and **PinNode** if your game uses it.
4. `git clone https://github.com/wifijt/rpu-sim`

## Play a game

```bash
cd rpu-sim
./play.py ~/src/StarTrek2022 --image ~/Pictures/startrek-playfield.jpg --sounds ~/Sounds/StarTrek2022
```

It builds the game (about 30 s), runs it, and opens http://localhost:8765.

| Option | |
|---|---|
| `--image <picture>` | A straight-on picture of the playfield: a photo, or the one inside a Visual Pinball table. Without it you get plain shapes. |
| `--sounds <folder>` | The game's WAV Trigger folder (what goes on its SD card: `201.wav` or `0201_name.wav`). |
| `--watch GameMode,BonusX` | Game variables to show live. |
| `--machine <id>` | The RPUMachines machine, when it can't tell from the header the game includes. |
| `--port`, `--no-browser` | |

**On the page**

- **Start** (`1`), **Coin** (`5`), **Tilt** (`t`), **Slam**, **Drain** (`Space`): the cabinet.
- Click a switch, on the playfield or in the Switches list, to hit it. Shift-click holds it closed
  (a ball resting in a hole), or lets it go.
- Lamps light on the playfield and in the Lamps list. Coils flash when they fire and make a
  mechanical sound: **Coil sounds** turns that on and off and sets its volume.
- **Sound** turns the game's sounds on and off. Browsers only play sound after your first click.
- **Watch** shows game variables live. Type a list and click **Watch**; it rebuilds the game.
  `GameMode` and the machine state show by name.
- **Reset game**: as if the machine were switched off and on.
- The playfield bar zooms: **Fit**, **Width**, **−**, **+**, or ⌘-scroll or pinch. Drag the divider
  between the two sides; double-click it for 50/50.
- The **Log** shows every switch, coil and sound as it happens.

## Add a machine

You need the machine's lamp, switch and coil numbers (from its manual) and a picture of its
playfield.

1. **Define it in RPUMachines:** a header with the numbers and a names file. See RPUMachines'
   [CONTRIBUTING.md](https://github.com/wifijt/RPUMachines/blob/main/CONTRIBUTING.md). Without
   one the game still runs, with lamps and switches shown by number.
2. **Run your game with the picture:** `./play.py MyGame --image playfield.jpg`.
3. **Line up the picture:** click **Align image**, drag the picture's corners to the playfield's
   edges, then **Done aligning**. This is saved next to the picture (`playfield.jpg.align.json`).
4. **Place everything:** click **Edit layout**, then drag each switch, coil and lamp from the lists
   onto its spot. Click a placed one to select it: **Remove** (or Delete) takes it off, **+** and
   **−** resize it. If the machine has a Visual Pinball table, `tools/vpx_import.py` places most of
   the lamps for you (see Tools).
5. **Add its physics,** so the ball gets served. In the layout file, say which coil opens which
   switches (outhole kicker, saucers, drop target resets) and where the ball starts. Copy
   `physics` and `ballStarts` from the Star Trek layout.
6. **Save layout.** To share it, copy it into a clone of RPUMachines' `layouts/` and open a pull
   request.

Layouts are saved to `~/Documents/rpu-sim/layouts/` (or `RPU_SIM_LAYOUTS`), outside every repo, and
load from there first. If `MACHINES_LIB` points at a git clone of RPUMachines, they're saved
straight into it instead.

## Tools

**Lamps from a Visual Pinball table.** Extract the table with `vpxtool extract MyTable.vpx` (the
playfield picture is usually `images/pf.png`), then:

```bash
tools/vpx_import.py path/to/extracted-table           # report what agrees with your layout
tools/vpx_import.py path/to/extracted-table --write   # add the lamps you haven't placed
```

It turns the table's lamp numbers (PinMAME's) into RPU's, lines the table up with what you've
placed, and lists every disagreement. `--replace` also moves lamps you placed. Tables are made by
hand and sometimes number a lamp differently, so check the ones it disagrees about.

**Dick Hamill's [Lamp Animator](https://pinballrefresh.com/animation-generator)** generates lamp
animations. In **Edit layout**, **Export** saves your playfield picture and lamp positions in its
format, so you can skip mapping lamps there. **Import** reads positions saved from it.

**Scenarios** are scripted tests: switch hits, time passing, and checks on what the game did.

```bash
./run.sh ~/src/MyGame MyGame/sim/01-start.sim    # prints PASS, or FAIL and the line
```

See [docs/SCENARIOS.md](docs/SCENARIOS.md). `examples/` has scenarios for Dick's
[ExampleMachine](https://github.com/RetroPinUpgrade/ExampleMachine) and for
[PinNode-Example](https://github.com/pin-node/PinNode-Example).

## Where things are

| | |
|---|---|
| `play.py`, `web/` | The player and its page |
| `build.sh`, `sim_core.cpp`, `mock/` | Building a game into a Mac program |
| `run.sh`, `examples/`, `docs/SCENARIOS.md` | Scenarios |
| `tools/vpx_import.py` | Lamps from a Visual Pinball table |
| `build/<game>/` | The built game. Safe to delete. |
| `~/Documents/rpu-sim/layouts/` | Your layouts |
| next to your picture | `<picture>.align.json`: how it lines up |

## What it doesn't tell you

- **Timing.** A pass of `loop()` is always 1 ms here. On the Mega it must stay well under 100 ms.
- **Memory.** Check the flash and RAM figures of the real build.
- **Integer width.** `int` is 32 bits here and 16 on the Mega; `unsigned long` is 64 here.
- The MPU bus, switch matrix timing, or coil strengths.

## Other computers

rpu-sim is Mac only. A port to Linux, Windows or WebAssembly would be welcome: the game code and
`sim_core.cpp` are plain C++ and `play.py` is Python, so most of a port is `build.sh`.

## Licence

GPL-3.0 (see LICENSE), as RPU and its games are. Copyright (C) 2026 John Turner (wifijt).
RPU is Dick Hamill's ([github.com/RetroPinUpgrade](https://github.com/RetroPinUpgrade)).
