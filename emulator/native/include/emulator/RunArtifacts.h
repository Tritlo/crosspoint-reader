#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

#include "Configuration.h"
#include "DirectoryStorage.h"

namespace emulator {

inline constexpr uint32_t TRACE_VERSION = 2;
inline constexpr uint32_t PANEL_MODEL_VERSION = 3;

class RunArtifacts {
 public:
  explicit RunArtifacts(const Configuration& configuration) : configuration(configuration) {}

  bool begin(const StorageMetadata& storage, std::string& error);
  void record(std::string_view type, uint64_t sequence, uint64_t simulatedTimeUs, const JsonObjectConst& fields);

 private:
  Configuration configuration;
  std::ofstream events;
};

}  // namespace emulator
