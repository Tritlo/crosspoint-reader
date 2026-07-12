#pragma once

#include <cstdint>

#include "emulator/DeterministicScheduler.h"
#include "emulator/SimulatedClock.h"

namespace emulator {

// Installs the process-wide Arduino/FreeRTOS compatibility context. The
// emulator intentionally supports one device session per process.
class FreeRtosRuntime {
 public:
  FreeRtosRuntime(DeterministicScheduler& scheduler, SimulatedClock& clock);
  ~FreeRtosRuntime();

  FreeRtosRuntime(const FreeRtosRuntime&) = delete;
  FreeRtosRuntime& operator=(const FreeRtosRuntime&) = delete;
};

uint64_t runtimeMicroseconds();
void runtimeDelay(uint64_t microseconds);
void runtimeYield();

}  // namespace emulator
