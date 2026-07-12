#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Print.h"
#include "WString.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

unsigned long millis();
unsigned long micros();
void delay(unsigned long milliseconds);
void delayMicroseconds(unsigned int microseconds);
void yield();
void pinMode(int8_t pin, uint8_t mode);
void digitalWrite(int8_t pin, uint8_t level);
int digitalRead(int8_t pin);
int analogRead(int8_t pin);
void analogSetAttenuation(int attenuation);
void randomSeed(unsigned long seed);
long random(long upperBound);
long random(long lowerBound, long upperBound);

constexpr uint8_t LOW = 0;
constexpr uint8_t HIGH = 1;
constexpr uint8_t INPUT = 1;
constexpr uint8_t OUTPUT = 3;
constexpr uint8_t INPUT_PULLUP = 5;
constexpr uint8_t INPUT_PULLDOWN = 6;
constexpr int ADC_11db = 3;
constexpr uint8_t DEC = 10;
constexpr uint8_t HEX = 16;
constexpr uint8_t OCT = 8;
constexpr uint8_t BIN = 2;

using byte = uint8_t;
using boolean = bool;
using std::max;
using std::min;

class EspClass {
 public:
  uint32_t getFreeHeap() const { return 256U * 1024U * 1024U; }
  uint32_t getHeapSize() const { return 256U * 1024U * 1024U; }
  uint32_t getMinFreeHeap() const { return getFreeHeap(); }
  uint32_t getMaxAllocHeap() const { return getFreeHeap(); }
  void restart();
};

extern EspClass ESP;

#define RTC_NOINIT_ATTR
#define RTC_DATA_ATTR
#define IRAM_ATTR
#define PROGMEM
#define pgm_read_byte(address) (*reinterpret_cast<const uint8_t*>(address))
#define pgm_read_word(address) (*reinterpret_cast<const uint16_t*>(address))
#define pgm_read_dword(address) (*reinterpret_cast<const uint32_t*>(address))
#define pgm_read_ptr(address) (*reinterpret_cast<void* const*>(address))
#define memcpy_P(destination, source, size) std::memcpy(destination, source, size)
