#pragma once

#include <cstdint>
#include <optional>

namespace emulator {

class SimulatedClock {
 public:
  uint64_t nowMicroseconds() const { return nowUs; }
  bool advance(uint64_t microseconds);

 private:
  uint64_t nowUs = 0;
};

}  // namespace emulator
