# Scenarios

Scripted tests for a game: run with `./run.sh path/to/MyGame scenario.sim`. Each run prints `PASS`, or `FAIL` and
the line that failed; add `-v` to the built `sim` for the whole log.

## Writing scenarios for your game

Make a folder for your scenarios, say `MyGame/sim/`, with two files.

**`machine.sim`: what the playfield does on its own.** A real ball moves when a coil fires; here
you say which switches that opens. Where the ball starts goes here too.

```
# The outhole kicker serves the ball: the outhole switch opens
physics SOL_OUTHOLE opens SW_OUTHOLE
# The saucer kicks out its ball
physics SOL_SAUCER opens SW_SAUCER
# The drop target bank reset stands all three targets up
physics SOL_DROP_RESET opens SW_DROP_1 SW_DROP_2 SW_DROP_3
# Power on with the ball in the outhole
close SW_OUTHOLE
```

Use your game's own names (the `SW_`, `SOL_` and `LAMP_` defines in its headers).

**A scenario: something to check.** For example, that a game starts, scores and ends a ball:

```
include machine.sim
run 3000                    # power on and settle into attract
check state == 0            # attract mode

press SW_CREDIT_RESET       # Start
run 3000
# the ball was served
expect SOL fire SOL_OUTHOLE
check ball == 1

press SW_POP_BUMPER         # hit something (a ball that drains untouched is served again)
check score 1 >= 1
run 60000                   # past any ball save
close SW_OUTHOLE            # drain
run 8000
check ball == 2
```

Run it with `./run.sh path/to/MyGame MyGame/sim/01-start.sim`. When a check fails, run again with
`-v` and read the log around that time.

**Tips**
- Times are in milliseconds of game time. A long `run` takes a moment, not a minute.
- `press` closes a switch for 40 ms. Use `close` and `open` for a ball resting on a switch.
- `expect`, `never` and `count` match the rest of the line, so put their comments on the line above.
- Building for one machine of several: `SIM_DEFINES="-DMACHINE_BALLY_STAR_TREK" ./run.sh …`.

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
| `link down` / `link up` | the node disappearing and coming back (PinNode games only) |
| `node start REQID SLOT` | Start pressed in the app (PinNode games only) |
| `node set REQID SET_X VALUE` | a setting changed from the app (PinNode games only) |
| `node claim SLOT CODE SECS` | a claim code to show (PinNode games only) |
| `node audit` | the node asking for the audits (PinNode games only) |
| `node action REQID ACT_X start\|stop\|keepalive [ARG]` | an operator action from the phone (PinNode games only) |
| `node lock on\|off` | a tournament holding (or releasing) the difficulty settings (PinNode games only) |
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
