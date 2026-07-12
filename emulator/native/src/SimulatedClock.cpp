#include "emulator/SimulatedClock.h"

#include <limits>

namespace emulator {

bool SimulatedClock::advance(uint64_t microseconds) {
  if (microseconds > std::numeric_limits<uint64_t>::max() - nowUs) return false;
  nowUs += microseconds;
  return true;
}

}  // namespace emulator
