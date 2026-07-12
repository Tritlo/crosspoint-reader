#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace emulator {

bool writeGrayscalePng(const std::filesystem::path& path, uint16_t width, uint16_t height,
                       const std::vector<uint8_t>& pixels, std::string& error);

std::vector<uint8_t> rotateGrayscaleClockwise(uint16_t width, uint16_t height, const std::vector<uint8_t>& pixels);

}  // namespace emulator
