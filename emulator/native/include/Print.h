#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>

#include "Printable.h"
#include "WString.h"

class Print {
 public:
  virtual ~Print() = default;
  virtual void flush() {}
  virtual size_t write(uint8_t value) = 0;
  virtual size_t write(const uint8_t* buffer, size_t size) {
    size_t written = 0;
    while (written < size && write(buffer[written]) == 1) ++written;
    return written;
  }
  size_t write(const char* value) {
    return value == nullptr ? 0 : write(reinterpret_cast<const uint8_t*>(value), std::strlen(value));
  }
  size_t print(const String& value) { return write(reinterpret_cast<const uint8_t*>(value.data()), value.size()); }
  size_t print(const char* value) { return write(value); }
  size_t print(char value) { return write(static_cast<uint8_t>(value)); }
  size_t print(unsigned char value, int base = 10) { return print(String(value, base)); }
  size_t print(int value, int base = 10) { return print(String(value, base)); }
  size_t print(unsigned int value, int base = 10) { return print(String(value, base)); }
  size_t print(long value, int base = 10) { return print(String(value, base)); }
  size_t print(unsigned long value, int base = 10) { return print(String(value, base)); }
  size_t print(long long value, int base = 10) { return print(String(value, base)); }
  size_t print(unsigned long long value, int base = 10) { return print(String(value, base)); }
  size_t print(double value, int decimals = 2) { return print(String(value, decimals)); }
  size_t print(const Printable& value) { return value.printTo(*this); }
  size_t println() { return print("\r\n"); }
  template <typename Value>
  size_t println(const Value& value) {
    return print(value) + println();
  }
  size_t printf(const char* format, ...);
  size_t vprintf(const char* format, va_list arguments);
};
