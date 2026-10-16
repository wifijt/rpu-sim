# rpu-sim

![Star Trek 2022 running in rpu-sim: backglass, playfield with lamps lit, watch and lamp panels](docs/screenshot.jpg)

Run a RetroPinUpgrade (RPU) game on your Mac, with no Mega and no machine. Your game's own code is
built into a Mac program that runs in two modes:

- **Scenarios (headless).** `run.sh` plays a text file of switch hits, waits and checks against the
  game, and prints `PASS` or `FAIL` with the line that failed, and a log of everything the game did.
  For testing: repeatable, and a long game takes seconds.
- **Play (browser).** `play.py` runs the game in real time behind a page with the backglass, the
  playfield with its lamps lighting, switches you click, coils firing and the game's own sounds.
  For trying things out by hand.

## How it works

```
                                                      ┌─ run.sh <scenario.sim> ─▶ PASS / FAIL + log
your game (.ino, .cpp) ──clang++──▶ build/<game>/sim ─┤
                                                      └─ play.py ◀──▶ the page in your browser
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

## Run scenarios (headless)

Try it with Dick's ExampleMachine. From any folder:

```bash
git clone https://github.com/wifijt/rpu-sim
git clone https://github.com/RetroPinUpgrade/ExampleMachine ExampleMachineMEGA   # folder = the sketch's name
cd rpu-sim
./run.sh ../ExampleMachineMEGA examples/ExampleMachine/01-start-a-game.sim
# PASS examples/ExampleMachine/01-start-a-game.sim  (5 checks, 74110 ms simulated, ...)
```

A scenario is a text file: one command per line.

```
# the machine's physics: which switches each coil opens or closes
include machine.sim
# power on and settle into attract, then press Start
run 3000
press SW_CREDIT_RESET
run 3000
# the ball was served
expect SOL fire SOL_OUTHOLE
check ball == 1
press SW_POP_BUMPER
check score 1 >= 1
```

```bash
./run.sh <your game's folder> <scenario.sim>     # build and run: PASS, or FAIL and the line
./build/<game>/sim -v <scenario.sim>             # run it again, with the whole log
./build/ExampleMachineMEGA/sim -v examples/ExampleMachine/01-start-a-game.sim     # for example
```

Time is game time: `run 60000` takes a moment, not a minute. The log shows every switch, coil and
sound with its time. `examples/ExampleMachine/` has a scenario to copy from. All the commands and
checks are in [docs/SCENARIOS.md](docs/SCENARIOS.md).

## Play a game (browser)

Try it with ExampleMachine (cloned in the scenario try-it above), using the example layout that
comes with rpu-sim:

```bash
cd rpu-sim
./play.py ../ExampleMachineMEGA --layout examples/ExampleMachine/layout.json --physics examples/ExampleMachine/machine.sim
```

It builds the game (about 30 s), runs it, and opens http://localhost:8765. Coin (`5`), Start (`1`),
then click switches on the playfield. ExampleMachine isn't a real
machine, so its playfield is made up.

Your own game, with its playfield picture and sounds:

```bash
./play.py <your game's folder> --image <playfield picture> --sounds <WAV Trigger folder>
```

A machine with a layout in RPUMachines (Bally Star Trek so far) shows its switches, lamps and coils
on the playfield. For anything else, see Add a machine.

| Option | |
|---|---|
| `--image <picture>` | A straight-on picture of the playfield: a photo, or the one inside a Visual Pinball table. Without it you get plain shapes. |
| `--sounds <folder>` | The game's WAV Trigger folder (what goes on its SD card: `201.wav` or `0201_name.wav`). |
| `--watch GameMode,BonusX` | Game variables to show live. |
| `--machine <id>` | The RPUMachines machine, when it can't tell from the header the game includes. |
| `--layout <file.json>` | A layout file to use and save to, in place of the machine's. |
| `--physics <file.sim>` | Which switches each coil opens or closes. Without it, RPUMachines' `physics/<machine>.sim`. |
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
5. **Add its physics.** Your game only fires coils; on a real machine the coil then does something
   that moves switches. The trough kicks the ball into the shooter lane, so the outhole switch
   opens (and a shooter-lane switch, if there is one, closes). A drop target reset stands the
   targets up, so their switches open. A saucer kicks out its ball, so its switch opens. The sim
   has no ball and no mechanics, so you tell it, for each coil, which switches it opens or closes.
   Without this the ball is never served and the game waits forever.

   Write it as a `.sim` file, like
   [examples/ExampleMachine/machine.sim](examples/ExampleMachine/machine.sim):

   ```
   # The trough serves the ball: the outhole switch opens
   physics SOL_SERVE_BALL opens SW_OUTHOLE
   # The drop target reset stands all three up
   physics SOL_DROP_BANK_R_RESET opens SW_DROP_R_1 SW_DROP_R_2 SW_DROP_R_3
   # A machine with a shooter-lane switch: the ball lands on it
   physics SOL_SERVE_BALL opens SW_OUTHOLE closes SW_SHOOTER_LANE
   # Power on with the ball in the outhole
   close SW_OUTHOLE
   ```

   Try it with `./play.py <your game's folder> --physics machine.sim`. The Log shows each switch the
   physics moved, as `SW open SW_OUTHOLE (by SOL_SERVE_BALL)`. Switches the ball hits while it's in
   play (bumpers, slings, rollovers) aren't physics: you click those.

   **Machines with a ball trough** (several balls, often multiball) need the sim to count balls,
   because what a coil does depends on how many are where. Describe the trough instead:

   ```
   # Scorpion: two balls; the outhole kicker feeds the trough, the ball ramp thrower serves
   trough SW_BALL_1 SW_BALL_2 serve SOL_BALL_RAMP_THROWER feed SOL_OUTHOLE from SW_OUTHOLE balls 2
   ```

   - List the trough's switches **from the exit end** (the ball served next sits on the first).
     The sim fills them from that end and keeps them matching the number of balls.
   - `serve` takes one ball out; the last switch in line opens. `to SW_X` (optional) says where
     the ball lands, for a trough that serves onto a switch or into another trough.
   - `feed ... from SW_OUTHOLE` (optional) moves a ball waiting on that switch into the trough.
     Every **Drain** is one more ball on it, so with two balls in play you drain twice.
   - `balls N` is how many it holds at power-on, in place of `close SW_OUTHOLE`.
   - A machine can have more than one trough: one `trough` line each. A lock that holds several
     balls is a trough too.

   The page shows each trough's count (`Trough 1 +1`: one in it, one waiting to go in), and the
   Log shows each move (`TROUGH SW_BALL_1 served a ball: 1 left`).

   The same file works in scenarios (`include machine.sim`); there `fire SOL_X` fires a coil
   yourself, to try your physics. Physics stays out of the layout: the layout is positions only.
6. **Save layout.** To share the layout and the physics, copy them into a clone of RPUMachines as
   `layouts/<machine>.json` and `physics/<machine>.sim` (like
   [Star Trek's](https://github.com/wifijt/RPUMachines/blob/main/physics/bally-star-trek-1979.sim))
   and open a pull request. play.py then finds both on its own.

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

## Where things are

| | |
|---|---|
| `play.py`, `web/` | The player and its page |
| `build.sh`, `sim_core.cpp`, `mock/` | Building a game into a Mac program |
| `run.sh`, `docs/SCENARIOS.md` | Scenarios |
| `examples/ExampleMachine/` | A scenario, a physics file and a layout for Dick's ExampleMachine |
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
