#pragma once
#define WDTO_15MS 0
inline void wdt_enable(int) {}
inline void wdt_disable() {}
inline void wdt_reset() {}
