// Host-side EEPROM (4 KB, as on the Mega 2560), for the simulator in sim/.
#pragma once
#include <stdint.h>
#define SIM_EEPROM_SIZE 4096
struct SimEEPROMClass {
  uint8_t read(int addr);
  void write(int addr, uint8_t value);
  void update(int addr, uint8_t value) { if (read(addr) != value) write(addr, value); }
};
extern SimEEPROMClass EEPROM;
