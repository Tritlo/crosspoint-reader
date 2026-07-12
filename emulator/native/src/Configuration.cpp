#include "emulator/Configuration.h"

#include <charconv>
#include <string_view>

namespace emulator {
namespace {

constexpr DeviceProfile X3_PROFILE{Device::X3, "x3", 792, 528, "uc8253"};
constexpr DeviceProfile X4_PROFILE{Device::X4, "x4", 800, 480, "ssd1677"};

bool parseUnsigned(std::string_view value, uint64_t& result) {
  const auto conversion = std::from_chars(value.data(), value.data() + value.size(), result);
  return conversion.ec == std::errc{} && conversion.ptr == value.data() + value.size();
}

}  // namespace

const DeviceProfile& profileFor(Device device) { return device == Device::X3 ? X3_PROFILE : X4_PROFILE; }

std::optional<Configuration> parseConfiguration(int argc, char** argv, std::string& error) {
  std::optional<Device> device;
  std::optional<std::filesystem::path> artifacts;
  std::string rtcStart = "2000-01-01T00:00:00Z";
  uint64_t randomSeed = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument == "--device") {
      if (++i >= argc) {
        error = "--device requires x3 or x4";
        return std::nullopt;
      }
      const std::string_view value(argv[i]);
      if (value == "x3") device = Device::X3;
      else if (value == "x4") device = Device::X4;
      else {
        error = "--device must be x3 or x4";
        return std::nullopt;
      }
    } else if (argument == "--artifacts") {
      if (++i >= argc) {
        error = "--artifacts requires a directory";
        return std::nullopt;
      }
      artifacts = std::filesystem::path(argv[i]);
    } else if (argument == "--rtc-start") {
      if (++i >= argc) {
        error = "--rtc-start requires an ISO-8601 value";
        return std::nullopt;
      }
      rtcStart = argv[i];
    } else if (argument == "--seed") {
      if (++i >= argc || !parseUnsigned(argv[i], randomSeed)) {
        error = "--seed requires an unsigned integer";
        return std::nullopt;
      }
    } else {
      error = "unknown argument: " + std::string(argument);
      return std::nullopt;
    }
  }

  if (!device) {
    error = "--device is required";
    return std::nullopt;
  }
  if (!artifacts) {
    error = "--artifacts is required";
    return std::nullopt;
  }

  return Configuration{profileFor(*device), *artifacts, std::move(rtcStart), randomSeed};
}

}  // namespace emulator
