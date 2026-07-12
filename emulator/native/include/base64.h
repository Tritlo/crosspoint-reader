#pragma once

#include <Arduino.h>

namespace base64 {
String encode(const uint8_t* data, size_t size);
inline String encode(const char* data) {
  return data == nullptr ? String() : encode(reinterpret_cast<const uint8_t*>(data), std::strlen(data));
}
}  // namespace base64
