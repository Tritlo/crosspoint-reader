#pragma once

#include <Arduino.h>

class MD5Builder {
 public:
  void begin() { data.clear(); }
  void add(const char* value) {
    if (value != nullptr) data.append(value);
  }
  void add(const uint8_t* value, size_t size) {
    if (value != nullptr) data.append(reinterpret_cast<const char*>(value), size);
  }
  void calculate() {}
  String toString() const { return "00000000000000000000000000000000"; }

 private:
  String data;
};
