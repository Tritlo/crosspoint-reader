#include <BoardConfig.h>
#include <ESPmDNS.h>
#include <HardwareSerial.h>
#include <Print.h>
#include <SPI.h>
#include <WString.h>
#include <WiFi.h>
#include <base64.h>
#include <esp_mac.h>
#include <mbedtls/base64.h>

#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

#include "emulator/PhysicalControls.h"

namespace {
uint32_t randomState = 1;

uint32_t nextRandom() {
  randomState ^= randomState << 13;
  randomState ^= randomState >> 17;
  randomState ^= randomState << 5;
  return randomState;
}
}  // namespace

HWCDC Serial;
SPIClass SPI;
WiFiClass WiFi;
MDNSResponder MDNS;
EspClass ESP;

std::string String::decimal(double value, unsigned int decimals) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(decimals) << value;
  return output.str();
}

size_t Print::printf(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  const size_t result = vprintf(format, arguments);
  va_end(arguments);
  return result;
}

size_t Print::vprintf(const char* format, va_list arguments) {
  va_list copy;
  va_copy(copy, arguments);
  const int size = std::vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  if (size <= 0) return 0;
  std::vector<char> buffer(static_cast<size_t>(size) + 1);
  std::vsnprintf(buffer.data(), buffer.size(), format, arguments);
  return write(reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<size_t>(size));
}

size_t HWCDC::write(uint8_t value) { return write(&value, 1); }

size_t HWCDC::write(const uint8_t* buffer, size_t size) {
  std::cerr.write(reinterpret_cast<const char*>(buffer), static_cast<std::streamsize>(size));
  return std::cerr ? size : 0;
}

void HWCDC::flush() { std::cerr.flush(); }

void EspClass::restart() { std::cerr << "emulator: ESP.restart requested but reset is not implemented yet\n"; }

void randomSeed(unsigned long seed) { randomState = static_cast<uint32_t>(seed == 0 ? 1 : seed); }

long random(long upperBound) { return upperBound <= 0 ? 0 : static_cast<long>(nextRandom() % upperBound); }

long random(long lowerBound, long upperBound) {
  return upperBound <= lowerBound ? lowerBound : lowerBound + random(upperBound - lowerBound);
}

uint32_t esp_random() { return nextRandom(); }

void pinMode(int8_t, uint8_t) {}
void digitalWrite(int8_t, uint8_t) {}
int digitalRead(int8_t pin) {
  if (pin == BoardConfig::ACTIVE.input.power) {
    const bool pressed = emulator::physicalControlPressed(6);
    return pressed == BoardConfig::ACTIVE.input.powerActiveHigh ? HIGH : LOW;
  }
  return HIGH;
}

int analogRead(int8_t pin) {
  static constexpr int group1Values[] = {3512, 2694, 1493, 5};
  static constexpr int group2Values[] = {2242, 5};
  if (pin == 1) {
    for (uint8_t button = 0; button < 4; ++button) {
      if (emulator::physicalControlPressed(button)) return group1Values[button];
    }
  }
  if (pin == 2) {
    for (uint8_t button = 4; button < 6; ++button) {
      if (emulator::physicalControlPressed(button)) return group2Values[button - 4];
    }
  }
  return 4095;
}

void analogSetAttenuation(int) {}

int esp_efuse_mac_get_default(uint8_t* mac) {
  if (mac == nullptr) return -1;
  constexpr uint8_t deterministicMac[6] = {0x02, 0x43, 0x50, 0x00, 0x00, 0x01};
  std::memcpy(mac, deterministicMac, sizeof(deterministicMac));
  return 0;
}

String base64::encode(const uint8_t* data, size_t size) {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String output;
  output.reserve(static_cast<unsigned int>(((size + 2) / 3) * 4));
  for (size_t index = 0; index < size; index += 3) {
    const uint32_t value = static_cast<uint32_t>(data[index]) << 16 |
                           (index + 1 < size ? static_cast<uint32_t>(data[index + 1]) << 8 : 0) |
                           (index + 2 < size ? data[index + 2] : 0);
    output += alphabet[(value >> 18) & 0x3F];
    output += alphabet[(value >> 12) & 0x3F];
    output += index + 1 < size ? alphabet[(value >> 6) & 0x3F] : '=';
    output += index + 2 < size ? alphabet[value & 0x3F] : '=';
  }
  return output;
}

int mbedtls_base64_decode(unsigned char* destination, size_t destinationSize, size_t* written,
                          const unsigned char* source, size_t sourceSize) {
  if (written == nullptr || source == nullptr) return -1;
  auto decode = [](unsigned char value) -> int {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
  };
  if (sourceSize % 4 != 0) return -1;
  size_t required = sourceSize / 4 * 3;
  if (sourceSize > 0 && source[sourceSize - 1] == '=') --required;
  if (sourceSize > 1 && source[sourceSize - 2] == '=') --required;
  *written = required;
  if (destination == nullptr || destinationSize < required) return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
  size_t output = 0;
  for (size_t index = 0; index < sourceSize; index += 4) {
    const int a = decode(source[index]);
    const int b = decode(source[index + 1]);
    const int c = source[index + 2] == '=' ? 0 : decode(source[index + 2]);
    const int d = source[index + 3] == '=' ? 0 : decode(source[index + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return -1;
    const uint32_t value = static_cast<uint32_t>(a) << 18 | static_cast<uint32_t>(b) << 12 |
                           static_cast<uint32_t>(c) << 6 | static_cast<uint32_t>(d);
    if (output < required) destination[output++] = static_cast<unsigned char>(value >> 16);
    if (output < required) destination[output++] = static_cast<unsigned char>(value >> 8);
    if (output < required) destination[output++] = static_cast<unsigned char>(value);
  }
  *written = output;
  return 0;
}
