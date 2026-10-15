#pragma once
#include "Arduino.h"
struct SimWire {
  void begin() {}
  void begin(int) {}
  void beginTransmission(int) {}
  size_t write(uint8_t) { return 1; }
  size_t write(const uint8_t *, size_t n) { return n; }
  int endTransmission() { return 0; }
  int requestFrom(int, int) { return 0; }
  int available() { return 0; }
  int read() { return -1; }
  void onReceive(void (*)(int)) {}
  void onRequest(void (*)(void)) {}
};
extern SimWire Wire;
