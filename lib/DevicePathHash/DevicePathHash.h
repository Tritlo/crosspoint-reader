#pragma once

#include <cstdint>
#include <string_view>

namespace device_path {

// Matches libstdc++ std::hash<std::string> on the 32-bit ESP toolchain. Cache
// directories are persisted on the SD card, so their names must not depend on
// the host's size_t width.
uint32_t hash(std::string_view path);

}  // namespace device_path
