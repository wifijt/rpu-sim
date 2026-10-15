// Host-side stand-in for the Arduino core, for the simulator in sim/ (not used on the Mega).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef uint8_t byte;
typedef bool boolean;
typedef uint16_t word;

#define PROGMEM
#define F(x) x
#define pgm_read_byte(p)        (*(const uint8_t *)(p))
#define pgm_read_word(p)        (*(const uint16_t *)(p))
#define pgm_read_dword(p)       (*(const uint32_t *)(p))
#define pgm_read_byte_far(a)    ((uint8_t)0)
#define pgm_get_far_address(v)  ((uint32_t)0)

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define RISING 3
#define FALLING 2
#define CHANGE 1

unsigned long millis();
unsigned long micros();
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return 0; }
inline void noInterrupts() {}
inline void interrupts() {}
inline long random(long howbig) { return howbig ? rand() % howbig : 0; }
inline long random(long lo, long hi) { return hi > lo ? lo + rand() % (hi - lo) : lo; }

class HardwareSerial {
  public:
    explicit HardwareSerial(int portNum) : port(portNum) {}
    void begin(unsigned long) {}
    void end() {}
    void flush() {}
    int available();
    int read();
    int peek();
    size_t write(uint8_t b);
    size_t write(const uint8_t *buf, size_t len);
    size_t write(const char *s) { return write((const uint8_t *)s, strlen(s)); }
    size_t print(const char *s) { return write(s); }
    size_t print(char c) { return write((uint8_t)c); }
    size_t print(long v, int base = 10);
    size_t print(unsigned long v, int base = 10);
    size_t print(int v, int base = 10) { return print((long)v, base); }
    size_t print(unsigned int v, int base = 10) { return print((unsigned long)v, base); }
    size_t print(unsigned char v, int base = 10) { return print((unsigned long)v, base); }
    size_t println() { return write("\n"); }
    template <typename T> size_t println(T v) { size_t n = print(v); return n + println(); }
    template <typename T> size_t println(T v, int base) { size_t n = print(v, base); return n + println(); }
    operator bool() const { return true; }
  private:
    int port;
};
typedef HardwareSerial Print;   // enough of Print for code that takes a Print* to write to
extern HardwareSerial Serial, Serial1, Serial2, Serial3;

#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif
