#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace emulator {

enum class Device { X3, X4 };

struct DeviceProfile {
  Device device;
  const char* id;
  uint16_t panelWidth;
  uint16_t panelHeight;
  const char* controller;
};

struct Configuration {
  DeviceProfile profile;
  std::filesystem::path artifactDirectory;
  std::string rtcStart = "2000-01-01T00:00:00Z";
  uint64_t randomSeed = 0;
};

const DeviceProfile& profileFor(Device device);
std::optional<Configuration> parseConfiguration(int argc, char** argv, std::string& error);

}  // namespace emulator
