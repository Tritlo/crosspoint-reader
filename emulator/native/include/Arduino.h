#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

unsigned long millis();
unsigned long micros();
void delay(unsigned long milliseconds);
void delayMicroseconds(unsigned int microseconds);
void yield();

constexpr uint8_t LOW = 0;
constexpr uint8_t HIGH = 1;
constexpr uint8_t INPUT = 1;
constexpr uint8_t OUTPUT = 3;
constexpr uint8_t INPUT_PULLUP = 5;

#define RTC_NOINIT_ATTR
#define RTC_DATA_ATTR
#define IRAM_ATTR
#define PROGMEM
