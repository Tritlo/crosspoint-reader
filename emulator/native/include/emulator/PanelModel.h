#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace emulator {

struct PanelSnapshot {
  uint16_t width = 0;
  uint16_t height = 0;
  uint64_t generation = 0;
  bool busy = false;
  std::string controller;
  std::vector<uint8_t> pixels;
};

struct PanelTransition {
  uint64_t simulatedTimeUs = 0;
  uint64_t generation = 0;
  std::string phase;
  std::vector<uint8_t> pixels;
};

PanelSnapshot panelSnapshot();
const std::vector<PanelTransition>& panelTransitions();

}  // namespace emulator
