/**************************************************************************
 * rpu-sim: a host-side simulator for RetroPinUpgrade (RPU) games (no Mega needed).
 *
 * Copyright (C) 2026 John Turner (wifijt). GPL-3.0: see LICENSE.
 *
 * The real sketch, DisplayHandler, OperatorMenus, AudioHandler (and PinNodeCtrl, for PinNode games)
 * are compiled for the host and linked against this file, which stands in for:
 *   - the Arduino core (millis, Serial, EEPROM),
 *   - RPU.cpp (switches, lamps, displays, solenoids: the RPU.h API),
 *   - RPULink.cpp, for PinNode games (the link to the node: records are decoded and logged),
 *   - the WAV Trigger on Serial1 (commands are decoded, track reports sent back).
 *
 * A scenario file drives it: switch closures, time, records from the node, and
 * checks on the log and on the game's state. See README.md.
 *
 * What it can't tell you: real timing (a loop() pass is always 1 ms here), RAM
 * and flash limits, 16-bit int overflow (int is 32 bits on the host, unsigned
 * long is 64), or anything about the real MPU bus.
 **************************************************************************/
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <fstream>
#include <sstream>
#include <cstdarg>
#include <thread>
#include <iostream>
#include <mutex>
#include <chrono>

#include "Arduino.h"
#include "EEPROM.h"
#include "Wire.h"
#include "RPU_Config.h"
#include "RPU.h"
#ifdef RPU_SIM_PINNODE                      // build.sh sets it when the sketch includes PinNode.h
#include "RPULink.h"
#endif

// ---- the sketch
void setup();
void loop();
extern unsigned long CurrentScores[];
extern byte CurrentPlayer, CurrentBallInPlay, CurrentNumPlayers;
extern char MachineState;

// ------------------------------------------------------------------ names
struct SimName { const char *name; int value; };
static const SimName SimNames[] = {
#include "sim_names.inc"
  {NULL, 0}
};

static int LookupName(const std::string &token, bool *ok = NULL) {
  if (ok) *ok = true;
  if (token.empty()) { if (ok) *ok = false; return 0; }
  if (isdigit((unsigned char)token[0]) || token[0] == '-') return (int)strtol(token.c_str(), NULL, 0);
  for (const SimName *n = SimNames; n->name; n++) if (token == n->name) return n->value;
  if (token == "SW_SELF_TEST_SWITCH") return SW_SELF_TEST_SWITCH;
  if (ok) *ok = false;
  return 0;
}

static std::string NameOf(const char *prefix, int value) {
  size_t pl = strlen(prefix);
  for (const SimName *n = SimNames; n->name; n++) {
    if (n->value == value && strncmp(n->name, prefix, pl) == 0) return n->name;
  }
  if (strcmp(prefix, "SW_") == 0 && value == SW_SELF_TEST_SWITCH) return "SW_SELF_TEST_SWITCH";
  char buf[24];
  snprintf(buf, sizeof(buf), "%s%d", prefix, value);
  return buf;
}

// ------------------------------------------------------------------ log
static std::vector<int> LiveFired, LiveSounds;   // live mode: coils fired and tracks started since the last frame
static bool LiveMode = false;
static unsigned long SimNow = 0;
static std::vector<std::string> SimLog;
static bool SimEcho = false;

static void Log(const char *fmt, ...) {
  char body[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  char line[560];
  snprintf(line, sizeof(line), "[%7lu] %s", SimNow, body);
  SimLog.push_back(line);
  if (SimEcho) printf("%s\n", line);
}

// ------------------------------------------------------------------ time
static unsigned long SimDelayTotal = 0, SimDelayMax = 0;
static bool SimBooted = false;

unsigned long millis() { return SimNow; }
unsigned long micros() { return SimNow * 1000UL; }
void delay(unsigned long ms) {
  // A delay() after boot stalls the loop: the PinNode link drops at 100 ms
  if (SimBooted) {
    Log("DELAY %lu ms", ms);
    SimDelayTotal += ms;
    if (ms > SimDelayMax) SimDelayMax = ms;
  }
  SimNow += ms;
}
void delayMicroseconds(unsigned int) {}

// ------------------------------------------------------------------ EEPROM
static uint8_t SimEEPROMData[SIM_EEPROM_SIZE];
static unsigned long SimEEPROMWrites = 0;
SimEEPROMClass EEPROM;
uint8_t SimEEPROMClass::read(int addr) { return (addr >= 0 && addr < SIM_EEPROM_SIZE) ? SimEEPROMData[addr] : 0xFF; }
void SimEEPROMClass::write(int addr, uint8_t value) {
  if (addr < 0 || addr >= SIM_EEPROM_SIZE) return;
  SimEEPROMData[addr] = value;
  SimEEPROMWrites += 1;
}

// ------------------------------------------------------------------ WAV Trigger (Serial1)
#define SIM_WAV_VOICES 14
struct SimVoice { int track; unsigned long endsAt; bool looping; };
static SimVoice SimVoices[SIM_WAV_VOICES];
static std::deque<uint8_t> WavRx;      // bytes for the Mega to read
static std::vector<uint8_t> WavTx;     // command being assembled
static std::map<int, unsigned long> WavTrackLength;
static unsigned long WavDefaultLength = 1200;
static bool SimLogGains = false;   // "wavgains on": log every track gain and fade

static void WavReport(int track, int voice, bool on) {
  uint16_t t = (uint16_t)(track - 1);
  uint8_t msg[9] = {0xF0, 0xAA, 9, 132, (uint8_t)(t & 0xFF), (uint8_t)(t >> 8), (uint8_t)voice, (uint8_t)(on ? 1 : 0), 0x55};
  for (uint8_t b : msg) WavRx.push_back(b);
}

static void WavStopTrack(int track) {
  for (int v = 0; v < SIM_WAV_VOICES; v++) {
    if (SimVoices[v].track == track) {
      WavReport(track, v, false);
      SimVoices[v].track = 0;
    }
  }
}

static void WavPlay(int track, bool solo) {
  if (LiveMode) LiveSounds.push_back(track);
  if (solo) for (int v = 0; v < SIM_WAV_VOICES; v++) if (SimVoices[v].track) WavStopTrack(SimVoices[v].track);
  WavStopTrack(track);
  for (int v = 0; v < SIM_WAV_VOICES; v++) {
    if (SimVoices[v].track == 0) {
      SimVoices[v].track = track;
      SimVoices[v].looping = false;
      unsigned long len = WavTrackLength.count(track) ? WavTrackLength[track] : WavDefaultLength;
      SimVoices[v].endsAt = SimNow + len;
      WavReport(track, v, true);
      return;
    }
  }
}

static void WavTick() {
  for (int v = 0; v < SIM_WAV_VOICES; v++) {
    if (SimVoices[v].track && !SimVoices[v].looping && SimNow >= SimVoices[v].endsAt) {
      WavReport(SimVoices[v].track, v, false);
      SimVoices[v].track = 0;
    }
  }
}

static void WavCommand(const std::vector<uint8_t> &m) {
  // m = F0 AA len cmd ... 55
  if (m.size() < 5) return;
  uint8_t cmd = m[3];
  if (cmd == 3 || cmd == 13) {            // TRACK_CONTROL / TRACK_CONTROL_EX
    if (m.size() < 8) return;
    int code = m[4];
    int track = m[5] | (m[6] << 8);
    switch (code) {
      case 0: Log("WAV play %d (solo)", track); WavPlay(track, true); break;
      case 1: Log("WAV play %d", track); WavPlay(track, false); break;
      case 4: Log("WAV stop %d", track); WavStopTrack(track); break;
      case 5: Log("WAV loop %d on", track);
        for (int v = 0; v < SIM_WAV_VOICES; v++) if (SimVoices[v].track == track) SimVoices[v].looping = true;
        break;
      case 6: Log("WAV loop %d off", track);
        for (int v = 0; v < SIM_WAV_VOICES; v++) if (SimVoices[v].track == track) { SimVoices[v].looping = false; SimVoices[v].endsAt = SimNow + 500; }
        break;
      default: break;
    }
  } else if (cmd == 8 && m.size() >= 9) {   // TRACK_VOLUME: track u16, gain i16 (dB)
    int track = m[4] | (m[5] << 8);
    int gain = (int16_t)(m[6] | (m[7] << 8));
    if (SimLogGains) Log("WAV gain %d %d dB", track, gain);
  } else if (cmd == 10 && m.size() >= 12) { // TRACK_FADE: track u16, gain i16, ms u16, stop u8
    int track = m[4] | (m[5] << 8);
    int gain = (int16_t)(m[6] | (m[7] << 8));
    int ms = m[8] | (m[9] << 8);
    if (SimLogGains) Log("WAV fade %d to %d dB in %d ms%s", track, gain, ms, m[10] ? " then stop" : "");
  } else if (cmd == 4) {
    Log("WAV stop all");
    for (int v = 0; v < SIM_WAV_VOICES; v++) if (SimVoices[v].track) WavStopTrack(SimVoices[v].track);
  }
  // volume, fade, reporting and the rest are accepted silently
}

static void WavByte(uint8_t b) {
  if (WavTx.empty() && b != 0xF0) return;
  WavTx.push_back(b);
  if (WavTx.size() == 2 && WavTx[1] != 0xAA) { WavTx.clear(); return; }
  if (WavTx.size() >= 3 && WavTx.size() == WavTx[2]) {
    if (WavTx.back() == 0x55) WavCommand(WavTx);
    WavTx.clear();
  }
  if (WavTx.size() > 40) WavTx.clear();
}

SimWire Wire;

// ------------------------------------------------------------------ Serial
HardwareSerial Serial(0), Serial1(1), Serial2(2), Serial3(3);
static std::string SerialLine;

size_t HardwareSerial::write(uint8_t b) {
  if (port == 0) {
    if (b == '\n') { if (!SerialLine.empty()) Log("SER %s", SerialLine.c_str()); SerialLine.clear(); }
    else if (b != '\r') SerialLine += (char)b;
  } else if (port == 1) {
    WavByte(b);
  } else if (port == 2) {
    Log("ERROR Serial2 used: USART2 belongs to RPULink");
  }
  return 1;
}
size_t HardwareSerial::write(const uint8_t *buf, size_t len) { for (size_t i = 0; i < len; i++) write(buf[i]); return len; }
int HardwareSerial::available() { return port == 1 ? (int)WavRx.size() : 0; }
int HardwareSerial::read() {
  if (port != 1 || WavRx.empty()) return -1;
  int b = WavRx.front();
  WavRx.pop_front();
  return b;
}
int HardwareSerial::peek() { return (port == 1 && !WavRx.empty()) ? WavRx.front() : -1; }
size_t HardwareSerial::print(long v, int base) { char b[32]; snprintf(b, sizeof(b), base == 16 ? "%lX" : "%ld", v); return write(b); }
size_t HardwareSerial::print(unsigned long v, int base) { char b[32]; snprintf(b, sizeof(b), base == 16 ? "%lX" : "%lu", v); return write(b); }

static unsigned long LoopPass = 0;   // counts passes of loop(), to catch something drawn twice in one

// ------------------------------------------------------------------ RPU: switches
#define SIM_NUM_SWITCHES 128
static bool SwitchState[SIM_NUM_SWITCHES];
static std::deque<byte> SwitchStack;
static int NumGameSwitchesSim = 0;
static PlayfieldAndCabinetSwitch *GameSwitchesSim = NULL;

static void FireSolenoid(byte sol, const char *why);

static void SimCloseSwitch(int sw) {
  if (sw < 0 || sw >= SIM_NUM_SWITCHES) return;
  if (SwitchState[sw]) return;
  SwitchState[sw] = true;
  Log("SW close %s", NameOf("SW_", sw).c_str());
  SwitchStack.push_back((byte)sw);
  // pop bumpers and slingshots are fired by the OS itself, from the switch
  for (int i = 0; i < NumGameSwitchesSim; i++) {
    if (GameSwitchesSim[i].switchNum == sw && GameSwitchesSim[i].solenoid != SOL_NONE && RPU_IsSolenoidStackEnabled()) {
      FireSolenoid(GameSwitchesSim[i].solenoid, "switch");
    }
  }
}

static void SimOpenSwitch(int sw, bool quiet = false) {
  if (sw < 0 || sw >= SIM_NUM_SWITCHES) return;
  if (!SwitchState[sw]) return;
  SwitchState[sw] = false;
  if (!quiet) Log("SW open %s", NameOf("SW_", sw).c_str());
}

void RPU_SetupGameSwitches(int s_numSwitches, int, PlayfieldAndCabinetSwitch *s_gameSwitchArray) {
  NumGameSwitchesSim = s_numSwitches;
  GameSwitchesSim = s_gameSwitchArray;
}
byte RPU_GetDipSwitches(byte) { return 0; }
byte RPU_PullFirstFromSwitchStack() {
  if (SwitchStack.empty()) return SWITCH_STACK_EMPTY;
  byte sw = SwitchStack.front();
  SwitchStack.pop_front();
  return sw;
}
boolean RPU_SetSwitchInversion(byte) { return true; }
boolean RPU_ReadSingleSwitchState(byte switchNum) { return switchNum < SIM_NUM_SWITCHES ? SwitchState[switchNum] : false; }
void RPU_PushToSwitchStack(byte switchNumber) { SwitchStack.push_back(switchNumber); }
boolean RPU_GetUpDownSwitchState() { return true; }
void RPU_ClearUpDownSwitchState() {}

// ------------------------------------------------------------------ RPU: solenoids
struct SimPhysics { int solenoid; std::vector<int> opens; };
static std::vector<SimPhysics> Physics;
struct SimTimedSol { byte sol; unsigned long when; bool override; };
static std::vector<SimTimedSol> TimedSols;
static bool SolenoidStackOn = false;
static bool FlippersDisabled = true;
static unsigned long SolenoidFireCount[32];

static void FireSolenoid(byte sol, const char *why) {
  if (LiveMode) LiveFired.push_back(sol);
  Log("SOL fire %s%s%s", NameOf("SOL_", sol).c_str(), why[0] ? " by " : "", why);
  if (sol < 32) SolenoidFireCount[sol] += 1;
  for (const SimPhysics &p : Physics) {
    if (p.solenoid != sol) continue;
    for (int sw : p.opens) {
      if (SwitchState[sw]) { SwitchState[sw] = false; Log("SW open %s (by %s)", NameOf("SW_", sw).c_str(), NameOf("SOL_", sol).c_str()); }
    }
  }
}

void RPU_PushToSolenoidStack(byte solenoidNumber, byte, boolean disableOverride) {
  if (!disableOverride && !SolenoidStackOn) { Log("SOL dropped %s (stack disabled)", NameOf("SOL_", solenoidNumber).c_str()); return; }
  FireSolenoid(solenoidNumber, "");
}
boolean RPU_PushToTimedSolenoidStack(byte solenoidNumber, byte, unsigned long whenToFire, boolean disableOverride) {
  TimedSols.push_back({solenoidNumber, whenToFire, (bool)disableOverride});
  return true;
}
void RPU_UpdateTimedSolenoidStack(unsigned long curTime) {
  for (size_t i = 0; i < TimedSols.size();) {
    if (TimedSols[i].when < curTime) {
      SimTimedSol t = TimedSols[i];
      TimedSols.erase(TimedSols.begin() + i);
      RPU_PushToSolenoidStack(t.sol, 1, t.override);
    } else {
      i++;
    }
  }
}
void RPU_SetCoinLockout(boolean, byte) {}
void RPU_SetDisableFlippers(boolean disableFlippers, byte) {
  if (FlippersDisabled != (bool)disableFlippers) Log("FLIPPERS %s", disableFlippers ? "off" : "on");
  FlippersDisabled = disableFlippers;
}
boolean RPU_GetDisableFlippers(byte) { return FlippersDisabled; }
void RPU_SetContinuousSolenoidBit(boolean, byte) {}
boolean RPU_FireContinuousSolenoid(byte, byte) { return true; }
byte RPU_ReadContinuousSolenoids() { return 0; }
void RPU_DisableSolenoidStack() { SolenoidStackOn = false; }
void RPU_EnableSolenoidStack() { SolenoidStackOn = true; }
boolean RPU_IsSolenoidStackEnabled() { return SolenoidStackOn; }
void RPU_SetSolenoidDefaultPulse(byte, byte) {}

// ------------------------------------------------------------------ RPU: displays
static byte DisplayDigits[5][RPU_OS_NUM_DIGITS];
static byte DisplayEnable[5];
static int CreditsShown = -1, BallInPlayShown = -1;
// A display written with two different values in one pass of loop() flickers on the real
// machine: the display interrupt shows whichever was there when it fired.
static unsigned long DisplayWritePass[5];
static unsigned long DisplayWriteValue[5];
static unsigned long DisplayConflicts[5];

byte RPU_SetDisplay(int displayNumber, unsigned long value, boolean blankByMagnitude, byte minDigits, boolean) {
  if (displayNumber < 0 || displayNumber > 4) return 0;
  if (DisplayWritePass[displayNumber] == LoopPass && DisplayWriteValue[displayNumber] != value) DisplayConflicts[displayNumber] += 1;
  DisplayWritePass[displayNumber] = LoopPass;
  DisplayWriteValue[displayNumber] = value;
  byte blank = 0x00;
  for (int count = 0; count < RPU_OS_NUM_DIGITS; count++) {
    blank = blank * 2;
    if (value != 0 || count < minDigits) blank |= 1;
    DisplayDigits[displayNumber][(RPU_OS_NUM_DIGITS - 1) - count] = value % 10;
    value /= 10;
  }
  if (blankByMagnitude) DisplayEnable[displayNumber] = blank;
  return blank;
}
void RPU_SetDisplayBlank(int displayNumber, byte bitMask) { if (displayNumber >= 0 && displayNumber <= 4) DisplayEnable[displayNumber] = bitMask; }
byte RPU_GetDisplayBlank(int displayNumber) { return (displayNumber >= 0 && displayNumber <= 4) ? DisplayEnable[displayNumber] : 0; }
void RPU_SetDisplayCredits(int value, boolean displayOn, boolean) { CreditsShown = displayOn ? value : -1; }
void RPU_SetDisplayMatch(int value, boolean displayOn, boolean) { BallInPlayShown = displayOn ? value : -1; }
void RPU_SetDisplayBallInPlay(int value, boolean displayOn, boolean) { BallInPlayShown = displayOn ? value : -1; }
void RPU_SetDisplayFlash(int displayNumber, unsigned long value, unsigned long curTime, int period, byte minDigits) {
  if (period) {
    if ((curTime / period) % 2) RPU_SetDisplay(displayNumber, value, true, minDigits);
    else RPU_SetDisplayBlank(displayNumber, 0);
  }
}
void RPU_SetDisplayFlashCredits(unsigned long, int) {}
void RPU_CycleAllDisplays(unsigned long, byte, byte) {}

static std::string DisplayText(int d) {
  std::string s;
  for (int i = 0; i < RPU_OS_NUM_DIGITS; i++) {
    // the mask bit for the leftmost digit is bit 0 (see RPU_SetDisplay)
    s += (DisplayEnable[d] & (1 << i)) ? (char)('0' + DisplayDigits[d][i]) : ' ';
  }
  return s;
}

// ------------------------------------------------------------------ RPU: lamps
static byte LampOn[RPU_MAX_LAMPS], LampDim[RPU_MAX_LAMPS];
static int LampFlash[RPU_MAX_LAMPS];
// A lamp set two different ways in one pass of loop() flickers on the real machine
static unsigned long LampWritePass[RPU_MAX_LAMPS];
static unsigned long LampConflicts[RPU_MAX_LAMPS];

void RPU_SetLampState(int lampNum, byte s_lampState, byte s_lampDim, int s_lampFlashPeriod) {
  if (lampNum < 0 || lampNum >= RPU_MAX_LAMPS) return;
  if (LampWritePass[lampNum] == LoopPass && LoopPass != 0 &&
      (LampOn[lampNum] != (s_lampState ? 1 : 0) || LampFlash[lampNum] != (s_lampState ? s_lampFlashPeriod : 0))) LampConflicts[lampNum] += 1;
  LampWritePass[lampNum] = LoopPass;
  LampOn[lampNum] = s_lampState ? 1 : 0;
  LampDim[lampNum] = s_lampDim;
  LampFlash[lampNum] = s_lampState ? s_lampFlashPeriod : 0;
}
void RPU_ApplyFlashToLamps(unsigned long) {}
void RPU_FlashAllLamps(unsigned long) {}
void RPU_TurnOffAllLamps() { memset(LampOn, 0, sizeof(LampOn)); memset(LampFlash, 0, sizeof(LampFlash)); memset(LampDim, 0, sizeof(LampDim)); }
void RPU_SetDimDivisor(byte, byte) {}
byte RPU_ReadLampState(int lampNum) { return (lampNum >= 0 && lampNum < RPU_MAX_LAMPS) ? LampOn[lampNum] : 0; }
byte RPU_ReadLampDim(int lampNum) { return (lampNum >= 0 && lampNum < RPU_MAX_LAMPS) ? LampDim[lampNum] : 0; }
int RPU_ReadLampFlash(int lampNum) { return (lampNum >= 0 && lampNum < RPU_MAX_LAMPS) ? LampFlash[lampNum] : 0; }

// ------------------------------------------------------------------ RPU: the rest
// "bootoriginal MS": the board says the original ROM game was chosen. setup() then never
// returns (the sketch keeps the link up while the original runs), so the fake link ends
// the run after MS of it.
static bool SimBootOriginal = false;
static unsigned long SimOriginalMs = 0;
static unsigned long SimInitOptions = 0;
struct SimOriginalDone {};
unsigned long RPU_InitializeMPU(unsigned long initOptions, byte) {
  SimInitOptions = initOptions;
  return RPU_RET_SELECTOR_SWITCH_ON | (SimBootOriginal ? RPU_RET_ORIGINAL_CODE_REQUESTED : 0);
}
// The sketch has no business on the original game's bus: reads and writes are counted, and
// scenario 09 checks there are none while the original game runs.
static int SimBusAccesses = 0;
byte RPU_DataRead(int) { SimBusAccesses += 1; return 0; }
void RPU_DataWrite(int address, byte value) {
  SimBusAccesses += 1;
  Log("BUS write 0x%03X = 0x%02X", address, value);
}
void RPU_Update(unsigned long currentTime) { RPU_UpdateTimedSolenoidStack(currentTime); }
#if RPU_MPU_ARCHITECTURE>9
void RPU_SetBoardLEDs(boolean, boolean, byte) {}
#endif
#ifdef RPU_OS_USE_S_AND_T
void RPU_PlaySoundSAndT(byte soundByte) { Log("SND S&T %d", soundByte); }
#endif
#ifdef RPU_OS_USE_SB100
void RPU_PlaySB100(byte soundByte) { Log("SND SB100 %d", soundByte); }
#endif
#if defined(RPU_OS_USE_DASH50) || defined(RPU_OS_USE_DASH51)
void RPU_PlaySoundDash51(byte soundByte) { Log("SND -51 %d", soundByte); }
#endif
#if (RPU_OS_HARDWARE_REV>=2 && defined(RPU_OS_USE_SB300))
void RPU_PlaySB300SquareWave(byte, byte) {}
void RPU_PlaySB300Analog(byte, byte) {}
#endif

byte RPU_ReadByteFromEEProm(unsigned short startByte) { return EEPROM.read(startByte); }
void RPU_WriteByteToEEProm(unsigned short startByte, byte value) { EEPROM.write(startByte, value); }
void RPU_WriteULToEEProm(unsigned short startByte, unsigned long value) {
  for (int i = 0; i < 4; i++) EEPROM.write(startByte + i, (byte)(value >> (8 * i)));
}
unsigned long RPU_ReadULFromEEProm(unsigned short startByte, unsigned long defaultValue) {
  unsigned long value = 0;
  for (int i = 0; i < 4; i++) value |= ((unsigned long)EEPROM.read(startByte + i)) << (8 * i);
  if (value == 0xFFFFFFFFUL) {
    value = defaultValue;
    RPU_WriteULToEEProm(startByte, value);
  }
  return value;
}

static unsigned long RecordCount[256];         // PinNode records sent (0 without PinNode)

#ifdef RPU_SIM_PINNODE
// ------------------------------------------------------------------ RPULink
static bool LinkUp = true;
static RPULinkRecordHandler LinkHandler = NULL;

static uint32_t rd32(const byte *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const byte *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Mega -> node records (the PinNode link), decoded
static void DecodeRecord(byte type, const byte *d, byte len) {
  char buf[400];
  int n = 0;
  switch (type) {
    case 0x42: {
      if (len < 15) { Log("PN HELLO (short)"); break; }
      byte vl = d[13];
      std::string version((const char *)d + 14, vl);
      const byte *t = d + 14 + vl;
      Log("PN HELLO proto=%d build=%08x manifest=%02x%02x%02x%02x%02x%02x%02x%02x version=%s maxPlayers=%d settings=%d flags=%d hwRev=%d",
          d[0], rd32(d + 1), d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], version.c_str(), t[0], t[1], t[2], rd16(t + 3));
      break;
    }
    case 0x43: Log("PN HELLO_EXT ruleset=%.*s", d[0], d + 1); break;
    case 0x20: Log("PN GAME_START g=%d credits=%d freePlay=%d", rd16(d), d[2], d[3]); break;
    case 0x21: Log("PN PLAYER_ADDED g=%d slot=%d", rd16(d), d[2]); break;
    case 0x22: Log("PN BALL_START g=%d slot=%d ball=%d", rd16(d), d[2], d[3]); break;
    case 0x23: Log("PN BALL_END g=%d slot=%d ball=%d score=%u ms=%u", rd16(d), d[2], d[3], rd32(d + 4), rd32(d + 8)); break;
    case 0x24: {
      n = snprintf(buf, sizeof(buf), "PN GAME_END g=%d endedBy=%d scores=", rd16(d), d[2]);
      for (int i = 0; i < d[3]; i++) n += snprintf(buf + n, sizeof(buf) - n, "%s%u", i ? "," : "", rd32(d + 4 + 4 * i));
      snprintf(buf + n, sizeof(buf) - n, " ms=%u", rd32(d + 4 + 4 * d[3]));
      Log("%s", buf);
      break;
    }
    case 0x25: Log("PN ACH g=%d slot=%d ach=%d (%s) v=%d", rd16(d), d[2], d[3], NameOf("ACH_", d[3]).c_str(), (int32_t)rd32(d + 4)); break;
    case 0x26: Log("PN STAT g=%d slot=%d stat=%d (%s) op=%s v=%d", rd16(d), d[2], d[3], NameOf("STAT_", d[3]).c_str(), d[4] ? "max" : "add", (int32_t)rd32(d + 5)); break;
    case 0x27: Log("PN COIN chute=%d credits=%d", d[0], d[1]); break;
    case 0x28: Log("PN CREDIT source=%d n=%d", d[0], d[1]); break;
    case 0x29: Log("PN TILT g=%d slot=%d kind=%d", rd16(d), d[2], d[3]); break;
    case 0x2A: Log("PN FAULT code=%d arg=%d", d[0], rd16(d + 1)); break;
    case 0x2B: {
      n = snprintf(buf, sizeof(buf), "PN SETTINGS_REPORT start=%d n=%d:", d[0], d[1]);
      for (int i = 0; i < d[1] && n < 380; i++) n += snprintf(buf + n, sizeof(buf) - n, " %d=%d", d[2 + 5 * i], (int32_t)rd32(d + 3 + 5 * i));
      Log("%s", buf);
      break;
    }
    case 0x2C: Log("PN SETTING_CHANGED index=%d (%s) value=%d source=%d reqId=%d result=%d", d[0], NameOf("SET_", d[0]).c_str(), (int32_t)rd32(d + 1), d[5], rd16(d + 6), d[8]); break;
    case 0x2D: {
      n = snprintf(buf, sizeof(buf), "PN AUDIT_SNAPSHOT start=%d n=%d:", d[0], d[1]);
      for (int i = 0; i < d[1] && n < 380; i++) n += snprintf(buf + n, sizeof(buf) - n, " %d=%u", d[2 + 5 * i], rd32(d + 3 + 5 * i));
      Log("%s", buf);
      break;
    }
    case 0x2E: Log("PN MODE mode=%d ball=%d slot=%d", d[0], d[1], d[2]); break;
    case 0x30: Log("PN LOG %.*s", len, d); break;
    case 0x31: Log("PN START_RESULT reqId=%d result=%d slot=%d", rd16(d), d[2], d[3]); break;
    case 0x32: {
      static const char *results[] = {"ok", "not_now", "unknown", "local_only", "done"};
      static const char *reasons[] = {"finished", "stopped", "timeout", "?", "diagnostics", "link"};
      n = snprintf(buf, sizeof(buf), "PN OP_RESULT reqId=%d action=%d (%s) result=%s value=%d", rd16(d), d[2], NameOf("ACT_", d[2]).c_str(),
                   d[3] < 5 ? results[d[3]] : "?", (int32_t)rd32(d + 4));
      if (len > 8) snprintf(buf + n, sizeof(buf) - n, " reason=%s", d[8] < 6 ? reasons[d[8]] : "?");
      Log("%s", buf);
      break;
    }
    case 0x33: Log("PN SWITCH_EVENT sw=%d (%s) closed=%d", d[0], NameOf("SW_", d[0]).c_str(), d[1]); break;
    case 0x38: Log("PN CREDIT_RESULT reqId=%d result=%d credits=%d", rd16(d), d[2], d[3]); break;
    case 0x35: Log("PN CREDITS credits=%d freePlay=%d", d[0], d[1]); break;
    case 0x34: {
      n = snprintf(buf, sizeof(buf), "PN ACTIONS_REPORT n=%d:", d[0]);
      for (int i = 0; i < d[0] && n < 380; i++) n += snprintf(buf + n, sizeof(buf) - n, " %d=0x%02X", d[1 + 2 * i], d[2 + 2 * i]);
      Log("%s", buf);
      break;
    }
    default: Log("PN record 0x%02X len=%d", type, len); break;
  }
}

void RPULink_BeginUart(unsigned long) {}
void RPULink_BeginSpi(byte) {}
void RPULink_SetHandler(RPULinkRecordHandler handler) { LinkHandler = handler; }
void RPULink_Update(unsigned long) {
  if (SimBootOriginal) {
    SimNow += 1;
    if (SimNow >= SimOriginalMs) throw SimOriginalDone{};
  }
}
void RPULink_Poll() {}
boolean RPULink_Queue(byte type, const byte *data, byte len) {
  if (len > RPULINK_PAYLOAD_MAX - 2) { Log("ERROR record 0x%02X too long (%d)", type, len); return false; }
  RecordCount[type] += 1;
  static const byte none[1] = {0};
  DecodeRecord(type, data ? data : none, len);
  return true;
}
boolean RPULink_Pending() { return false; }   // nothing waits here either: a record is "sent" as it is queued
void RPULink_DropQueued() {}   // nothing waits here: records are logged as they are queued
boolean RPULink_QueueText(byte type, const char *text) { return RPULink_Queue(type, (const byte *)text, (byte)strlen(text)); }
boolean RPULink_IsUp() { return LinkUp; }
boolean RPULink_IsDormant() { return false; }
void RPULink_SetFlag(byte, boolean) {}
unsigned int RPULink_DroppedRecords() { return 0; }
unsigned int RPULink_BadFrames() { return 0; }
unsigned int RPULink_RxOverruns() { return 0; }
unsigned int RPULink_FramingErrors() { return 0; }
unsigned int RPULink_DataOverruns() { return 0; }
unsigned long RPULink_RawRxBytes() { return 0; }
unsigned long RPULink_RawTxBytes() { return 0; }
uint16_t RPULink_CRC16(const byte *, unsigned int) { return 0; }

// weak in the library (for a future PinNode library); the host linker wants a definition
void PinNodeCtrl_OnPinNodeRecord(byte, const byte *, byte) {}

char __data_load_end;   // PinNodeCtrl's build CRC walks flash up to this symbol on the Mega
#endif  // RPU_SIM_PINNODE

// ------------------------------------------------------------------ scenario runner
static int Checks = 0;
static size_t ExpectCursor = 0;
static std::string ScenarioName;
static int CurrentLine = 0;
static std::string CurrentFile;

static bool SoftFail = false;   // inside "until": a failed check throws instead of ending the run
struct SimCheckFailed { std::string why; };

static void Fail(const char *fmt, ...) {
  char body[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  if (SoftFail) throw SimCheckFailed{body};
  size_t from = SimLog.size() > 60 ? SimLog.size() - 60 : 0;
  printf("---- last log lines ----\n");
  for (size_t i = from; i < SimLog.size(); i++) printf("%s\n", SimLog[i].c_str());
  printf("FAIL %s (%s:%d) at %lu ms: %s\n", ScenarioName.c_str(), CurrentFile.c_str(), CurrentLine, SimNow, body);
  exit(1);
}

static void Boot() {
  if (SimBooted) return;
  setup();
  SimBooted = true;
  Log("NOTE setup() done");
}

static void RunFor(unsigned long ms) {
  Boot();
  for (unsigned long i = 0; i < ms; i++) {
    SimNow += 1;
    LoopPass += 1;
    WavTick();
    loop();
  }
}

static int NeedName(const std::string &token) {
  bool ok;
  int v = LookupName(token, &ok);
  if (!ok) Fail("unknown name '%s'", token.c_str());
  return v;
}

static bool Compare(long a, const std::string &op, long b) {
  if (op == "==" || op == "=") return a == b;
  if (op == "!=") return a != b;
  if (op == ">=") return a >= b;
  if (op == "<=") return a <= b;
  if (op == ">") return a > b;
  if (op == "<") return a < b;
  Fail("unknown operator '%s'", op.c_str());
  return false;
}

static void DumpState() {
  printf("  t=%lu ms  MachineState=%d  player=%d of %d  ball=%d  credits=%d  bip=%d\n", SimNow, (int)MachineState,
         CurrentPlayer + 1, CurrentNumPlayers, CurrentBallInPlay, CreditsShown, BallInPlayShown);
  for (int d = 0; d < 4; d++) printf("  display %d [%s]  score %lu\n", d + 1, DisplayText(d).c_str(), CurrentScores[d]);
  printf("  lamps on:");
  for (int l = 0; l < RPU_MAX_LAMPS; l++) if (LampOn[l]) printf(" %s%s", NameOf("LAMP_", l).c_str(), LampFlash[l] ? "*" : "");
  printf("\n  switches closed:");
  for (int s = 0; s < SIM_NUM_SWITCHES; s++) if (SwitchState[s]) printf(" %s", NameOf("SW_", s).c_str());
  printf("\n");
}

static void RunFile(const std::string &path);

static void RunCommand(const std::string &line, const std::string &dir) {
  std::istringstream in(line);
  std::string cmd;
  in >> cmd;
  if (cmd.empty() || cmd[0] == '#') return;

  if (cmd == "include") {
    std::string f; in >> f;
    std::string savedFile = CurrentFile; int savedLine = CurrentLine;
    RunFile(dir + "/" + f);
    CurrentFile = savedFile; CurrentLine = savedLine;
  } else if (cmd == "physics") {            // physics SOL_X opens SW_A SW_B ...
    std::string sol, word, sw;
    in >> sol >> word;
    SimPhysics p;
    p.solenoid = NeedName(sol);
    while (in >> sw) p.opens.push_back(NeedName(sw));
    Physics.push_back(p);
  } else if (cmd == "eeprom") {             // eeprom ADDR VALUE   (before boot: a preset)
    int addr, value; in >> addr >> value;
    SimEEPROMData[addr] = (uint8_t)value;
  } else if (cmd == "wavlen") {             // wavlen TRACK MS  (or: wavlen default MS)
    std::string t; unsigned long ms; in >> t >> ms;
    if (t == "default") WavDefaultLength = ms; else WavTrackLength[atoi(t.c_str())] = ms;
  } else if (cmd == "wavgains") {           // wavgains on|off: log track gains and fades too
    std::string s; in >> s;
    SimLogGains = (s == "on");
  } else if (cmd == "bootoriginal") {       // bootoriginal MS: power on with the original ROM game chosen
    in >> SimOriginalMs;
    SimBootOriginal = true;
    try { setup(); Fail("setup() returned: the sketch should stay with the link while the original runs"); }
    catch (SimOriginalDone &) { Log("NOTE the original game ran for %lu ms: %d bus reads and writes", SimOriginalMs, SimBusAccesses); }
    SimBooted = true;
  } else if (cmd == "boot") {
    Boot();
  } else if (cmd == "run") {
    unsigned long ms; in >> ms;
    RunFor(ms);
  } else if (cmd == "close") {
    std::string sw; in >> sw;
    Boot(); SimCloseSwitch(NeedName(sw));
  } else if (cmd == "open") {
    std::string sw; in >> sw;
    Boot(); SimOpenSwitch(NeedName(sw));
  } else if (cmd == "press") {              // press SW [ms]: close, wait, open
    std::string sw; unsigned long ms = 40; in >> sw; in >> ms;
    Boot();
    int n = NeedName(sw);
    SimCloseSwitch(n);
    RunFor(ms);
    SimOpenSwitch(n, true);
    RunFor(10);
#ifdef RPU_SIM_PINNODE
  } else if (cmd == "link") {
    std::string s; in >> s;
    LinkUp = (s == "up");
    Log("NOTE link %s", LinkUp ? "up" : "down");
  } else if (cmd == "node") {               // records from the node (the PinNode link)
    std::string what; in >> what;
    Boot();
    if (!LinkHandler) Fail("no link handler registered (PinNodeCtrl_Begin not called?)");
    if (what == "start") {
      int req = 1, slot = 0; in >> req >> slot;
      byte d[3] = {(byte)req, (byte)(req >> 8), (byte)slot};
      Log("NODE START_REQUEST reqId=%d slot=%d", req, slot);
      LinkHandler(0xAA, d, 3);
    } else if (what == "claim") {           // node claim SLOT CODE SECS
      int slot; unsigned long code; int secs; in >> slot >> code >> secs;
      byte d[10] = {0, 0, 1, (byte)slot, (byte)code, (byte)(code >> 8), (byte)(code >> 16), (byte)(code >> 24), (byte)secs, (byte)(secs >> 8)};
      Log("NODE CLAIM_DISPLAY slot=%d code=%lu secs=%d", slot, code, secs);
      LinkHandler(0xA9, d, 10);
    } else if (what == "set") {             // node set REQID INDEX VALUE
      int req; std::string index; long value; in >> req >> index >> value;
      int idx = NeedName(index);
      byte d[8] = {(byte)req, (byte)(req >> 8), (byte)idx, (byte)value, (byte)(value >> 8), (byte)(value >> 16), (byte)(value >> 24), 1};
      Log("NODE SETTING_SET reqId=%d index=%d value=%ld", req, idx, value);
      LinkHandler(0xA1, d, 8);
    } else if (what == "action") {          // node action REQID ACTION start|stop|keepalive [ARG]
      int req; std::string action, run; long arg = 0; in >> req >> action >> run >> arg;
      int idx = NeedName(action);
      int op = (run == "start") ? 0 : (run == "stop") ? 1 : (run == "keepalive") ? 2 : -1;
      if (op < 0) Fail("node action: start, stop or keepalive, not '%s'", run.c_str());
      byte d[8] = {(byte)req, (byte)(req >> 8), (byte)idx, (byte)op, (byte)arg, (byte)(arg >> 8), (byte)(arg >> 16), (byte)(arg >> 24)};
      Log("NODE OP_ACTION reqId=%d action=%d op=%s arg=%ld", req, idx, run.c_str(), arg);
      LinkHandler(0xAB, d, 8);
    } else if (what == "lock") {            // node lock on|off   (a tournament holds the settings)
      std::string onoff; in >> onoff;
      byte d[5] = {(byte)(onoff == "on" ? 1 : 0), 0, 0, 0, 0};
      Log("NODE SETTINGS_LOCK %s", onoff.c_str());
      LinkHandler(0xA7, d, 5);
    } else if (what == "credit") {          // node credit REQID N   (the owner's "Add credit")
      int req, n; in >> req >> n;
      byte d[3] = {(byte)req, (byte)(req >> 8), (byte)n};
      Log("NODE CREDIT_ADD reqId=%d n=%d", req, n);
      LinkHandler(0xA8, d, 3);
    } else if (what == "audit") {
      Log("NODE AUDIT_REQUEST");
      LinkHandler(0xA3, NULL, 0);
    } else {
      Fail("unknown node command '%s'", what.c_str());
    }
#else
  } else if (cmd == "link" || cmd == "node") {
    Fail("%s: this game doesn't use PinNode (no #include <PinNode.h>)", cmd.c_str());
#endif
  } else if (cmd == "expect") {             // expect TEXT: in the log, after the last expect
    std::string text;
    std::getline(in, text);
    size_t a = text.find_first_not_of(" \t");
    text = (a == std::string::npos) ? "" : text.substr(a);
    Checks++;
    bool found = false;
    for (size_t i = ExpectCursor; i < SimLog.size(); i++) {
      if (SimLog[i].find(text) != std::string::npos) { ExpectCursor = i + 1; found = true; break; }
    }
    if (!found) Fail("expected log line containing \"%s\"", text.c_str());
  } else if (cmd == "never") {              // never TEXT: nowhere in the log so far
    std::string text;
    std::getline(in, text);
    size_t a = text.find_first_not_of(" \t");
    text = (a == std::string::npos) ? "" : text.substr(a);
    Checks++;
    for (const std::string &l : SimLog) if (l.find(text) != std::string::npos) Fail("log has \"%s\": %s", text.c_str(), l.c_str());
  } else if (cmd == "count") {              // count TEXT == N  (whole log)
    std::string rest;
    std::getline(in, rest);
    size_t opPos = rest.find_last_of("=<>!");
    size_t opStart = rest.find_last_not_of("=<>!", opPos);
    std::string text = rest.substr(0, opStart + 1), op = rest.substr(opStart + 1, opPos - opStart);
    long want = atol(rest.substr(opPos + 1).c_str());
    size_t a = text.find_first_not_of(" \t"), b = text.find_last_not_of(" \t");
    text = text.substr(a, b - a + 1);
    long got = 0;
    for (const std::string &l : SimLog) if (l.find(text) != std::string::npos) got++;
    Checks++;
    if (!Compare(got, op, want)) Fail("count \"%s\" is %ld, wanted %s %ld", text.c_str(), got, op.c_str(), want);
  } else if (cmd == "check") {
    std::string what; in >> what;
    Checks++;
    if (what == "score") {                  // check score PLAYER OP VALUE
      int p; std::string op; long v; in >> p >> op >> v;
      if (!Compare((long)CurrentScores[p - 1], op, v)) Fail("score %d is %lu, wanted %s %ld", p, CurrentScores[p - 1], op.c_str(), v);
    } else if (what == "ball") {
      std::string op; long v; in >> op >> v;
      if (!Compare(CurrentBallInPlay, op, v)) Fail("ball is %d, wanted %s %ld", CurrentBallInPlay, op.c_str(), v);
    } else if (what == "player") {
      std::string op; long v; in >> op >> v;
      if (!Compare(CurrentPlayer + 1, op, v)) Fail("player is %d, wanted %s %ld", CurrentPlayer + 1, op.c_str(), v);
    } else if (what == "players") {
      std::string op; long v; in >> op >> v;
      if (!Compare(CurrentNumPlayers, op, v)) Fail("players is %d, wanted %s %ld", CurrentNumPlayers, op.c_str(), v);
    } else if (what == "state") {           // check state == N  (MachineState)
      std::string op; long v; in >> op >> v;
      if (!Compare((long)MachineState, op, v)) Fail("MachineState is %d, wanted %s %ld", (int)MachineState, op.c_str(), v);
    } else if (what == "lamp") {            // check lamp NAME on|off|flash
      std::string lamp, want; in >> lamp >> want;
      int l = NeedName(lamp);
      bool on = LampOn[l], flash = LampFlash[l] != 0;
      if (want == "on" && !on) Fail("%s is off", lamp.c_str());
      if (want == "off" && on) Fail("%s is on", lamp.c_str());
      if (want == "flash" && !(on && flash)) Fail("%s is not flashing", lamp.c_str());
    } else if (what == "switch") {          // check switch NAME open|closed
      std::string sw, want; in >> sw >> want;
      bool closed = SwitchState[NeedName(sw)];
      if ((want == "closed") != closed) Fail("%s is %s", sw.c_str(), closed ? "closed" : "open");
    } else if (what == "display") {         // check display N VALUE   (N 1-4; VALUE digits, or "blank")
      int d; std::string want; in >> d >> want;
      std::string text = DisplayText(d - 1), shown;
      for (char c : text) if (c != ' ') shown += c;
      if (want == "blank") { if (!shown.empty()) Fail("display %d shows [%s]", d, text.c_str()); }
      else if (atol(shown.c_str()) != atol(want.c_str()) || shown.empty()) Fail("display %d shows [%s], wanted %s", d, text.c_str(), want.c_str());
    } else if (what == "lampsteady") {      // check lampsteady LAMP: not set two different ways in any pass since "forget lampsteady LAMP"
      std::string lamp; in >> lamp;
      if (lamp == "all") {                  // every lamp; a handful of single passes (a mode change) is not a flicker
        std::string bad;
        for (int l = 0; l < RPU_MAX_LAMPS; l++) {
          if (LampConflicts[l] > 5) { char b[64]; snprintf(b, sizeof(b), " %s (%lu)", NameOf("LAMP_", l).c_str(), LampConflicts[l]); bad += b; }
          LampConflicts[l] = 0;
        }
        if (!bad.empty()) Fail("lamps set two different ways in one pass:%s", bad.c_str());
      } else {
        int l = NeedName(lamp);
        unsigned long conflicts = LampConflicts[l];
        LampConflicts[l] = 0;
        if (conflicts) Fail("%s was set two different ways in one pass, %lu times", lamp.c_str(), conflicts);
      }
    } else if (what == "steady") {          // check steady N: display N wasn't written twice, differently, in any pass since the last such check
      int d; in >> d;
      unsigned long conflicts = DisplayConflicts[d - 1];
      DisplayConflicts[d - 1] = 0;
      if (conflicts) Fail("display %d was written with two different values in one pass, %lu times", d, conflicts);
    } else if (what == "credits") {
      std::string op; long v; in >> op >> v;
      if (!Compare(CreditsShown, op, v)) Fail("credit display is %d, wanted %s %ld", CreditsShown, op.c_str(), v);
    } else if (what == "eeprom") {          // check eeprom ADDR OP VALUE (one byte)
      int addr; std::string op; long v; in >> addr >> op >> v;
      if (!Compare(SimEEPROMData[addr], op, v)) Fail("EEPROM[%d] is %d, wanted %s %ld", addr, SimEEPROMData[addr], op.c_str(), v);
    } else if (what == "fired") {           // check fired SOL OP N  (times since boot)
      std::string sol, op; long v; in >> sol >> op >> v;
      int s = NeedName(sol);
      if (!Compare((long)SolenoidFireCount[s], op, v)) Fail("%s fired %lu times, wanted %s %ld", sol.c_str(), SolenoidFireCount[s], op.c_str(), v);
    } else if (what == "nodelay") {         // no delay() longer than N ms since boot
      long v = 0; in >> v;
      if ((long)SimDelayMax > v) Fail("a delay() of %lu ms ran after boot", SimDelayMax);
    } else {
      Fail("unknown check '%s'", what.c_str());
    }
  } else if (cmd == "until") {              // until MS check ...: run until the check passes, or fail after MS
    unsigned long ms; in >> ms;
    std::string rest;
    std::getline(in, rest);
    Boot();
    unsigned long deadline = SimNow + ms;
    for (;;) {
      SoftFail = true;
      try {
        RunCommand(rest, dir);
        SoftFail = false;
        break;
      } catch (SimCheckFailed &f) {
        SoftFail = false;
        Checks--;
        if (SimNow >= deadline) Fail("still failing after %lu ms: %s", ms, f.why.c_str());
        RunFor(10);
      }
    }
  } else if (cmd == "forget") {             // forget steady N: start counting display N's double writes from here
    std::string what, which; in >> what >> which;
    if (what == "lampsteady" && which == "all") memset(LampConflicts, 0, sizeof(LampConflicts));
    else if (what == "lampsteady") LampConflicts[NeedName(which)] = 0;
    else DisplayConflicts[atoi(which.c_str()) - 1] = 0;
  } else if (cmd == "note") {
    std::string text; std::getline(in, text);
    Log("NOTE%s", text.c_str());
  } else if (cmd == "dump") {
    DumpState();
  } else if (cmd == "echo") {
    std::string s; in >> s;
    SimEcho = (s == "on");
  } else {
    Fail("unknown command '%s'", cmd.c_str());
  }
}

static void RunFile(const std::string &path) {
  std::ifstream f(path);
  if (!f) { printf("can't open %s\n", path.c_str()); exit(2); }
  std::string dir = path.find('/') == std::string::npos ? "." : path.substr(0, path.find_last_of('/'));
  CurrentFile = path;
  CurrentLine = 0;
  std::string line;
  while (std::getline(f, line)) {
    CurrentLine++;
    RunCommand(line, dir);
  }
}


// ------------------------------------------------------------------ live mode
// sim --live [setup.sim]: run in real time for a front end (play.py). Commands arrive on stdin,
// one per line: "close SW", "open SW", "press SW" (closed for 80 ms), or any scenario command.
// Every 33 ms a JSON frame goes to stdout: lamps, displays, closed switches, coils fired, sounds,
// new log lines. The first line is the names table.
static std::mutex LiveLock;
static std::deque<std::string> LiveInbox;

static std::string JsonEsc(const std::string &t) {
  std::string o;
  for (char c : t) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((unsigned char)c < 0x20) o += ' ';
    else if ((unsigned char)c < 0x80) o += c;
    else o += '?';
  }
  return o;
}

static void LiveNames() {
  std::string o = "{\"names\":{";
  bool first = true;
  for (const SimName *n = SimNames; n->name; n++) {
    o += first ? "" : ","; first = false;
    o += "\"" + JsonEsc(n->name) + "\":" + std::to_string(n->value);
  }
  o += "},\"lamps\":" + std::to_string(RPU_MAX_LAMPS) + ",\"digits\":" + std::to_string(RPU_OS_NUM_DIGITS) + "}";
  printf("%s\n", o.c_str());
  fflush(stdout);
}

static size_t LiveLogFrom = 0;
static void LiveFrame() {
  std::string o = "{\"t\":" + std::to_string(SimNow) + ",\"state\":" + std::to_string((int)MachineState);
  o += ",\"ball\":" + std::to_string((int)CurrentBallInPlay) + ",\"player\":" + std::to_string((int)CurrentPlayer + 1);
  o += ",\"credits\":" + std::to_string(CreditsShown) + ",\"bip\":" + std::to_string(BallInPlayShown);
  o += ",\"lamps\":\"";
  for (int l = 0; l < RPU_MAX_LAMPS; l++) {
    bool on = LampOn[l] && (!LampFlash[l] || (SimNow / (unsigned long)LampFlash[l]) % 2 == 0);
    o += on ? (LampDim[l] ? 'd' : '1') : '0';
  }
  o += "\",\"disp\":[";
  for (int d = 0; d < 5; d++) o += (d ? ",\"" : "\"") + DisplayText(d) + "\"";
  o += "],\"sw\":[";
  bool first = true;
  for (int sw = 0; sw < SIM_NUM_SWITCHES; sw++) if (SwitchState[sw]) { o += (first ? "" : ","); o += std::to_string(sw); first = false; }
  o += "],\"fired\":[";
  for (size_t k = 0; k < LiveFired.size(); k++) o += (k ? "," : "") + std::to_string(LiveFired[k]);
  o += "],\"sounds\":[";
  for (size_t k = 0; k < LiveSounds.size(); k++) o += (k ? "," : "") + std::to_string(LiveSounds[k]);
  o += "],\"log\":[";
  for (size_t k = LiveLogFrom; k < SimLog.size(); k++) {
    if (SimLog[k].find(" LAMP ") != std::string::npos) continue;
    o += (k > LiveLogFrom ? "," : "") + std::string("\"") + JsonEsc(SimLog[k]) + "\"";
  }
  o += "]}";
  if (o.find("[,") != std::string::npos) o.replace(o.find("[,"), 2, "[");
  LiveLogFrom = SimLog.size();
  LiveFired.clear(); LiveSounds.clear();
  printf("%s\n", o.c_str());
  fflush(stdout);
}

static void LiveRun(const std::string &setup) {
  LiveMode = true;
  SoftFail = true;
  LiveNames();
  if (!setup.empty()) {
    try { RunFile(setup); } catch (SimCheckFailed &f) { printf("{\"error\":\"%s\"}\n", JsonEsc(f.why).c_str()); }
  }
  Boot();
  std::thread([] {
    std::string line;
    while (std::getline(std::cin, line)) { std::lock_guard<std::mutex> g(LiveLock); LiveInbox.push_back(line); }
    std::lock_guard<std::mutex> g(LiveLock); LiveInbox.push_back("quit");
  }).detach();
  std::map<int, unsigned long> releaseAt;               // pressed switches: open again at this time
  auto start = std::chrono::steady_clock::now();
  unsigned long startMs = SimNow, nextFrame = SimNow;
  for (;;) {
    std::deque<std::string> todo;
    { std::lock_guard<std::mutex> g(LiveLock); todo.swap(LiveInbox); }
    for (const std::string &line : todo) {
      std::istringstream in(line);
      std::string cmd, name; in >> cmd >> name;
      if (cmd == "quit") return;
      bool ok = true;
      int n = LookupName(name, &ok);
      if (cmd == "close" && ok) SimCloseSwitch(n);
      else if (cmd == "open" && ok) SimOpenSwitch(n);
      else if (cmd == "press" && ok) { SimCloseSwitch(n); releaseAt[n] = SimNow + 80; }
      else if (cmd == "toggle" && ok) { if (n >= 0 && n < SIM_NUM_SWITCHES && SwitchState[n]) SimOpenSwitch(n); else SimCloseSwitch(n); }
      else {
        try { RunCommand(line, "."); } catch (SimCheckFailed &f) { printf("{\"error\":\"%s\"}\n", JsonEsc(f.why).c_str()); }
      }
    }
    // keep game time level with the wall clock: one loop() pass per simulated millisecond
    unsigned long wall = startMs + (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    int passes = 0;
    while (SimNow < wall && passes < 200) {
      SimNow += 1; LoopPass += 1; WavTick(); loop(); passes++;
      for (auto it = releaseAt.begin(); it != releaseAt.end();) {
        if (SimNow >= it->second) { SimOpenSwitch(it->first); it = releaseAt.erase(it); } else ++it;
      }
    }
    if (SimNow >= nextFrame) { LiveFrame(); nextFrame = SimNow + 33; }
    if (SimLog.size() > 20000) { SimLog.erase(SimLog.begin(), SimLog.begin() + 10000); LiveLogFrom = LiveLogFrom > 10000 ? LiveLogFrom - 10000 : 0; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

int main(int argc, char **argv) {
  memset(SimEEPROMData, 0xFF, sizeof(SimEEPROMData));
  bool verbose = false;
  std::vector<std::string> files;
  bool live = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-v") == 0) verbose = true;
    else if (strcmp(argv[i], "--live") == 0) live = true;
    else files.push_back(argv[i]);
  }
  if (live) { LiveRun(files.empty() ? "" : files[0]); return 0; }
  if (files.size() != 1) {
    printf("usage: sim [-v] scenario.sim     (one scenario per run: the sketch can't be restarted)\n");
    return 2;
  }
  SimEcho = verbose;
  ScenarioName = files[0];
  RunFile(files[0]);
  for (const std::string &l : SimLog) {
    if (l.find("] ERROR ") != std::string::npos) { CurrentLine = 0; Fail("%s", l.c_str()); }
  }
  printf("PASS %s  (%d checks, %lu ms simulated, %lu records, %lu EEPROM writes, longest delay() %lu ms)\n",
         files[0].c_str(), Checks, SimNow,
         [] { unsigned long n = 0; for (int i = 0; i < 256; i++) n += RecordCount[i]; return n; }(),
         SimEEPROMWrites, SimDelayMax);
  return 0;
}
