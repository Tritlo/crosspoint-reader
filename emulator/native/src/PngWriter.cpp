#include "emulator/PngWriter.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <fstream>

namespace emulator {
namespace {

void appendBigEndian(std::vector<uint8_t>& output, uint32_t value) {
  output.push_back(static_cast<uint8_t>(value >> 24));
  output.push_back(static_cast<uint8_t>(value >> 16));
  output.push_back(static_cast<uint8_t>(value >> 8));
  output.push_back(static_cast<uint8_t>(value));
}

uint32_t crc32(const uint8_t* bytes, size_t size) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t index = 0; index < size; ++index) {
    crc ^= bytes[index];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

void appendChunk(std::vector<uint8_t>& png, const std::array<uint8_t, 4>& type, const std::vector<uint8_t>& data) {
  appendBigEndian(png, static_cast<uint32_t>(data.size()));
  const size_t crcStart = png.size();
  png.insert(png.end(), type.begin(), type.end());
  png.insert(png.end(), data.begin(), data.end());
  appendBigEndian(png, crc32(png.data() + crcStart, png.size() - crcStart));
}

bool deflateCompressed(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& output) {
  uLongf size = compressBound(static_cast<uLong>(bytes.size()));
  output.resize(size);
  const int result =
      compress2(output.data(), &size, bytes.data(), static_cast<uLong>(bytes.size()), Z_BEST_COMPRESSION);
  if (result != Z_OK) return false;
  output.resize(size);
  return true;
}

}  // namespace

std::vector<uint8_t> rotateGrayscaleClockwise(uint16_t width, uint16_t height, const std::vector<uint8_t>& pixels) {
  if (pixels.size() != static_cast<size_t>(width) * height) return {};
  std::vector<uint8_t> rotated(pixels.size());
  for (uint16_t y = 0; y < height; ++y) {
    for (uint16_t x = 0; x < width; ++x) {
      const uint16_t rotatedX = static_cast<uint16_t>(height - 1 - y);
      const uint16_t rotatedY = x;
      rotated[static_cast<size_t>(rotatedY) * height + rotatedX] = pixels[static_cast<size_t>(y) * width + x];
    }
  }
  return rotated;
}

bool writeGrayscalePng(const std::filesystem::path& path, uint16_t width, uint16_t height,
                       const std::vector<uint8_t>& pixels, std::string& error) {
  if (width == 0 || height == 0 || pixels.size() != static_cast<size_t>(width) * height) {
    error = "invalid grayscale image dimensions";
    return false;
  }

  std::vector<uint8_t> scanlines;
  scanlines.reserve(static_cast<size_t>(width + 1) * height);
  for (uint16_t row = 0; row < height; ++row) {
    scanlines.push_back(0);
    const auto begin = pixels.begin() + static_cast<std::ptrdiff_t>(row) * width;
    scanlines.insert(scanlines.end(), begin, begin + width);
  }

  std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> header;
  appendBigEndian(header, width);
  appendBigEndian(header, height);
  header.insert(header.end(), {8, 0, 0, 0, 0});
  appendChunk(png, {'I', 'H', 'D', 'R'}, header);
  std::vector<uint8_t> compressed;
  if (!deflateCompressed(scanlines, compressed)) {
    error = "failed to compress PNG";
    return false;
  }
  appendChunk(png, {'I', 'D', 'A', 'T'}, compressed);
  appendChunk(png, {'I', 'E', 'N', 'D'}, {});

  std::error_code filesystemError;
  std::filesystem::create_directories(path.parent_path(), filesystemError);
  if (filesystemError) {
    error = "failed to create capture directory: " + filesystemError.message();
    return false;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  if (!output) {
    error = "failed to write PNG: " + path.string();
    return false;
  }
  return true;
}

}  // namespace emulator
