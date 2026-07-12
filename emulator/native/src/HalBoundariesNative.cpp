#include <Arduino.h>
#include <BoardConfig.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <InputManager.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "emulator/DirectoryStorage.h"
#include "emulator/FreeRtosCompat.h"

HalGPIO gpio;
HalPowerManager powerManager;
HalClock halClock;
HalTiltSensor halTiltSensor;

class HalGPIO::NativeState {
 public:
  InputManager input;
};

HalGPIO::HalGPIO() : native(std::make_unique<NativeState>()) {}
HalGPIO::~HalGPIO() = default;
void HalGPIO::begin() {
  _deviceType =
      emulator::runtimeStorage().config().profile.device == emulator::Device::X3 ? DeviceType::X3 : DeviceType::X4;
  BoardConfig::selectDevice(deviceIsX3() ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);
  native->input.begin();
}
void HalGPIO::update() {
  native->input.update();
  constexpr const char* names[] = {"back", "confirm", "left", "right", "up", "down", "power"};
  for (uint8_t button = 0; button < 7; ++button) {
    if (native->input.wasPressed(button)) {
      emulator::runtimeTraceApplication("input-pressed", names[button], native->input.getHeldTime());
    }
    if (native->input.wasReleased(button)) {
      emulator::runtimeTraceApplication("input-released", names[button], native->input.getHeldTime());
    }
  }
  usbStateChanged = false;
}
bool HalGPIO::isPressed(uint8_t buttonIndex) const { return native->input.isPressed(buttonIndex); }
bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return native->input.wasPressed(buttonIndex); }
bool HalGPIO::wasAnyPressed() const { return native->input.wasAnyPressed(); }
bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return native->input.wasReleased(buttonIndex); }
bool HalGPIO::wasAnyReleased() const { return native->input.wasAnyReleased(); }
unsigned long HalGPIO::getHeldTime() const { return native->input.getHeldTime(); }
unsigned long HalGPIO::getPowerButtonHeldTime() const { return native->input.getPowerButtonHeldTime(); }
void HalGPIO::startDeepSleep() { std::cerr << "emulator: GPIO deep sleep is not implemented\n"; }
void HalGPIO::verifyPowerButtonWakeup(uint16_t, bool) {}
bool HalGPIO::isUsbConnected() const { return lastUsbConnected; }
bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }
HalGPIO::WakeupReason HalGPIO::getWakeupReason() const { return WakeupReason::Other; }

void HalPowerManager::begin() {
  normalFreq = 160;
  modeMutex = xSemaphoreCreateMutex();
}
void HalPowerManager::setPowerSaving(bool enabled) { isLowPower = enabled; }
void HalPowerManager::startDeepSleep(HalGPIO&) const {
  std::cerr << "emulator: power-manager deep sleep is not implemented\n";
}
uint16_t HalPowerManager::getBatteryPercentage() const { return 100; }

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  valid = true;
  powerManager.currentLockMode = HalPowerManager::NormalSpeed;
  powerManager.setPowerSaving(false);
  xSemaphoreGive(powerManager.modeMutex);
}
HalPowerManager::Lock::~Lock() {
  if (!valid) return;
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  powerManager.currentLockMode = HalPowerManager::None;
  xSemaphoreGive(powerManager.modeMutex);
}

void HalClock::begin() { _available = gpio.deviceIsX3(); }
bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;
  const std::string& rtc = emulator::runtimeStorage().config().rtcStart;
  if (rtc.size() < 16) return false;
  hour = static_cast<uint8_t>(std::stoi(rtc.substr(11, 2)));
  minute = static_cast<uint8_t>(std::stoi(rtc.substr(14, 2)));
  const uint64_t elapsedMinutes = emulator::runtimeMicroseconds() / 60000000;
  const uint64_t total = hour * 60 + minute + elapsedMinutes;
  hour = static_cast<uint8_t>((total / 60) % 24);
  minute = static_cast<uint8_t>(total % 60);
  return true;
}
bool HalClock::formatTime(char* buffer, size_t bufferSize, uint8_t offsetBiased, bool use12Hour) const {
  uint8_t hour;
  uint8_t minute;
  if (!getTime(hour, minute) || buffer == nullptr || bufferSize < (use12Hour ? 9U : 6U)) return false;
  int total = hour * 60 + minute + (static_cast<int>(std::min<uint8_t>(offsetBiased, 104)) - 48) * 15;
  total = ((total % 1440) + 1440) % 1440;
  if (use12Hour) {
    const bool pm = total >= 720;
    int displayHour = (total / 60) % 12;
    if (displayHour == 0) displayHour = 12;
    std::snprintf(buffer, bufferSize, "%d:%02d %s", displayHour, total % 60, pm ? "PM" : "AM");
  } else {
    std::snprintf(buffer, bufferSize, "%02d:%02d", total / 60, total % 60);
  }
  return true;
}
bool HalClock::writeTimeToRTC(uint8_t, uint8_t, uint8_t) { return false; }
bool HalClock::syncFromNTP() {
  std::cerr << "emulator: NTP clock sync is not supported\n";
  return false;
}

void HalTiltSensor::begin() { _available = false; }
bool HalTiltSensor::wake() { return false; }
bool HalTiltSensor::deepSleep() { return false; }
void HalTiltSensor::update(uint8_t, uint8_t, bool) { _hadActivity = false; }
bool HalTiltSensor::wasTiltedForward() { return std::exchange(_tiltForwardEvent, false); }
bool HalTiltSensor::wasTiltedBack() { return std::exchange(_tiltBackEvent, false); }
bool HalTiltSensor::hadActivity() { return std::exchange(_hadActivity, false); }
void HalTiltSensor::clearPendingEvents() {
  _tiltForwardEvent = false;
  _tiltBackEvent = false;
  _hadActivity = false;
}
bool HalTiltSensor::writeReg(uint8_t, uint8_t) const { return false; }
bool HalTiltSensor::readReg(uint8_t, uint8_t*) const { return false; }
bool HalTiltSensor::readGyro(float&, float&, float&) const { return false; }

namespace HalSystem {
void begin() {}
void checkPanic() {}
void clearPanic() {}
std::string getPanicInfo(bool) { return {}; }
bool isRebootFromPanic() { return false; }
}  // namespace HalSystem
