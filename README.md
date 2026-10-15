# rpu-sim

Runs a RetroPinUpgrade (RPU) game on your computer, with no Mega and no machine. The game's
own code (the sketch, `DisplayHandler`, `OperatorMenus`, `AudioHandler`, and the PinNode
library's `PinNodeCtrl` if the game uses PinNode) is compiled for the host and linked against
`sim_core.cpp`, which stands in for the Arduino core, `RPU.cpp` (switches, lamps, displays,
coils), the PinNode link, and the WAV Trigger on Serial1. Scenario files drive it: switch
closures, time passing, and checks on what the game did.

```bash
./run.sh ~/src/MyGame examples/pin-node-example/01-full-game.sim   # build MyGame, run a scenario
./build/MyGame/sim -v examples/pin-node-example/01-full-game.sim   # again, with the full log
SIM_DEFINES="-DMACHINE_BALLY_STAR_TREK" ./run.sh ~/src/MyGame examples/pin-node-example-star-trek/01-star-trek-coils.sim
```

`examples/` has scenarios for [PinNode-Example](https://github.com/pin-node/PinNode-Example).

**Needs:** the Arduino IDE (its `arduino-cli` adds the sketch's function prototypes; set
`ARDUINO_CLI` if yours is somewhere else), `clang++`, and Python 3. Games that use PinNode need
the PinNode library (Library Manager: PinNode; or set `PINNODE_LIB`). Games that include a
machine header need RPUMachines (Library Manager; or set `MACHINES_LIB`).

**What it can't tell you:** real timing (a `loop()` pass is always 1 ms here), RAM and flash
limits, 16-bit `int` overflow (`int` is 32 bits on the host), or anything about the real
MPU bus.

## The log

Every run produces a time-stamped log, in milliseconds since power-on:

| Prefix | What |
|---|---|
| `SW` | a switch closing or opening |
| `SOL` | a coil firing (or being dropped because the solenoid stack is off) |
| `WAV` | a WAV Trigger command (play, stop, loop) |
| `PN` | a record sent to the PinNode node, decoded |
| `NODE` | a record from the node |
| `SER` | a line the sketch wrote to `Serial` |
| `DELAY` | a `delay()` after boot: these stall the link and should not exist |
| `ERROR` | something that fails the run (for example any use of `Serial2`) |

## Scenario files

One command per line; `#` starts a comment. Names (`SW_…`, `SOL_…`, `LAMP_…`,
`ACH_…`, `STAT_…`, `SET_…`, `ACT_…`) come from the sketch's own headers.

| Command | Meaning |
|---|---|
| `include machine.sim` | read another file (the machine's physics) |
| `physics SOL_X opens SW_A SW_B` | when this coil fires, these switches open |
| `eeprom ADDR VALUE` | preset an EEPROM byte (before the first `run`) |
| `bootoriginal MS` | power on with the original ROM game chosen, and let it run this long (in place of the first `run`) |
| `run MS` | run `loop()` for this long (1 ms per pass) |
| `press SW [MS]` | close a switch, wait (40 ms), open it |
| `press SW_SELF_TEST_SWITCH` | the coin-door self-test button (an event only: it has no held state) |
| `close SW` / `open SW` | hold a switch (a ball in the outhole or a saucer) |
| `link down` / `link up` | the node disappearing and coming back |
| `node start REQID SLOT` | Start pressed in the app |
| `node set REQID SET_X VALUE` | a setting changed from the app |
| `node claim SLOT CODE SECS` | a claim code to show |
| `node audit` | the node asking for the audits |
| `node action REQID ACT_X start\|stop\|keepalive [ARG]` | an operator action from the phone |
| `node lock on\|off` | a tournament holding (or releasing) the difficulty settings |
| `wavlen TRACK MS` | how long a track plays (default 1200 ms) |
| `expect TEXT` | the log has this text, after the previous `expect` |
| `never TEXT` | the log does not have this text |
| `count TEXT == N` | how many log lines have it |
| `check score P OP N` | player P's score (`==`, `>=`, …) |
| `check ball OP N`, `check player OP N`, `check players OP N` | game position |
| `check state OP N` | `MachineState` |
| `check lamp LAMP on\|off\|flash` | a lamp |
| `check switch SW open\|closed` | a switch |
| `check display N VALUE\|blank` | what score display N (1-4) shows |
| `check steady N` | display N was never written with two different values in one pass of `loop()` (which flickers on the machine), since the last such check |
| `check credits OP N` | the credit display |
| `check eeprom ADDR OP N` | an EEPROM byte |
| `check fired SOL OP N` | how often a coil has fired |
| `check nodelay MS` | no `delay()` longer than this ran after boot |
| `until MS check …` | run until the check passes, fail after MS |
| `check lampsteady LAMP` | the lamp was never set two different ways in one pass (which flickers on the machine) |
| `forget steady N`, `forget lampsteady LAMP` | start that check counting from here |
| `note TEXT`, `dump`, `echo on` | annotate the log, print the state, print as it runs |

One scenario per run: the sketch has no way to be restarted.

## What it doesn't tell you

- Timing. A pass of `loop()` is always 1 ms here. On the Mega it must stay well
  under 100 ms or the link drops; the simulator only catches `delay()` calls.
- Memory. Check the flash and RAM figures of the real compile.
- Integer width. `int` is 32 bits and `unsigned long` 64 bits on the host, so
  code that only works (or only breaks) through 16- or 32-bit overflow behaves
  differently.
- Anything about the MPU bus, switch matrix timing, or coil strengths.

## Licence

GPL-3.0 (see LICENSE), as RPU and its games are. Copyright (C) 2026 John Turner (wifijt).
RPU is Dick Hamill's ([github.com/RetroPinUpgrade](https://github.com/RetroPinUpgrade)).
