#include "DevicePathHash.h"

#include <cstddef>

namespace device_path {

uint32_t hash(std::string_view path) {
  constexpr uint32_t multiplier = 0x5bd1e995U;
  uint32_t result = 0xc70f6907U ^ static_cast<uint32_t>(path.size());
  const auto* bytes = reinterpret_cast<const uint8_t*>(path.data());
  size_t remaining = path.size();

  while (remaining >= 4) {
    uint32_t value = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
                     (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
    value *= multiplier;
    value ^= value >> 24;
    value *= multiplier;
    result *= multiplier;
    result ^= value;
    bytes += 4;
    remaining -= 4;
  }

  switch (remaining) {
    case 3:
      result ^= static_cast<uint32_t>(bytes[2]) << 16;
      [[fallthrough]];
    case 2:
      result ^= static_cast<uint32_t>(bytes[1]) << 8;
      [[fallthrough]];
    case 1:
      result ^= bytes[0];
      result *= multiplier;
      break;
    default:
      break;
  }

  result ^= result >> 13;
  result *= multiplier;
  result ^= result >> 15;
  return result;
}

}  // namespace device_path
