#pragma once

#include <cstdint>

namespace emulator {

void setPhysicalControl(uint8_t buttonIndex, bool pressed);
bool physicalControlPressed(uint8_t buttonIndex);
uint8_t physicalControlMask();

}  // namespace emulator
