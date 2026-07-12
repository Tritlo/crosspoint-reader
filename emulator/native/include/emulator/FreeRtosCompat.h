#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "emulator/DeterministicScheduler.h"
#include "emulator/SimulatedClock.h"

namespace emulator {

class DirectoryStorage;
using StorageTraceCallback =
    std::function<void(std::string_view operation, std::string_view path, uint64_t bytes, bool success)>;
using PanelTraceCallback = std::function<void(std::string_view event, std::string_view detail, uint64_t value)>;
using ApplicationTraceCallback = std::function<void(std::string_view event, std::string_view detail, uint64_t value)>;

// Installs the process-wide Arduino/FreeRTOS compatibility context. The
// emulator intentionally supports one device session per process.
class FreeRtosRuntime {
 public:
  FreeRtosRuntime(DeterministicScheduler& scheduler, SimulatedClock& clock, DirectoryStorage* storage = nullptr,
                  StorageTraceCallback storageTrace = {}, PanelTraceCallback panelTrace = {},
                  ApplicationTraceCallback applicationTrace = {});
  ~FreeRtosRuntime();

  FreeRtosRuntime(const FreeRtosRuntime&) = delete;
  FreeRtosRuntime& operator=(const FreeRtosRuntime&) = delete;
};

uint64_t runtimeMicroseconds();
bool runtimeIsActive();
void runtimeDelay(uint64_t microseconds);
void runtimeYield();
DirectoryStorage& runtimeStorage();
void runtimeTraceStorage(std::string_view operation, std::string_view path, uint64_t bytes, bool success);
void runtimeTracePanel(std::string_view event, std::string_view detail, uint64_t value);
void runtimeTraceApplication(std::string_view event, std::string_view detail, uint64_t value);

}  // namespace emulator
