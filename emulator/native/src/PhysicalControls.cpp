#include "emulator/PhysicalControls.h"

#include <atomic>
#include <stdexcept>

namespace emulator {
namespace {

std::atomic<uint8_t> controls{0};

}  // namespace

void setPhysicalControl(uint8_t buttonIndex, bool pressed) {
  if (buttonIndex >= 7) throw std::out_of_range("physical control index");
  const uint8_t bit = static_cast<uint8_t>(1U << buttonIndex);
  if (pressed)
    controls.fetch_or(bit);
  else
    controls.fetch_and(static_cast<uint8_t>(~bit));
}

bool physicalControlPressed(uint8_t buttonIndex) {
  return buttonIndex < 7 && (controls.load() & static_cast<uint8_t>(1U << buttonIndex)) != 0;
}

uint8_t physicalControlMask() { return controls.load(); }

}  // namespace emulator
