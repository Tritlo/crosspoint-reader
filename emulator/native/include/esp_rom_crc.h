#pragma once

#include <cstdint>

inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t* data, uint32_t length) {
  while (length-- > 0) {
    crc ^= *data++;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return crc;
}
