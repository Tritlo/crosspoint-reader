#pragma once

#include <Arduino.h>

class HWCDC : public Print {
 public:
  void begin(unsigned long) {}
  void setTxTimeoutMs(uint32_t) {}
  operator bool() const { return true; }
  int available() const { return 0; }
  String readStringUntil(char) { return {}; }
  size_t write(uint8_t value) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  void flush() override;
};

using HardwareSerial = HWCDC;

extern HWCDC Serial;
