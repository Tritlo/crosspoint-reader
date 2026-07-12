#pragma once

#include <WString.h>

#include <algorithm>
#include <array>
#include <cstdint>

enum wifi_mode_t { WIFI_MODE_NULL = 0, WIFI_MODE_STA = 1, WIFI_MODE_AP = 2, WIFI_MODE_APSTA = 3 };
constexpr wifi_mode_t WIFI_OFF = WIFI_MODE_NULL;
constexpr wifi_mode_t WIFI_STA = WIFI_MODE_STA;
constexpr wifi_mode_t WIFI_AP = WIFI_MODE_AP;

enum wl_status_t {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_DISCONNECTED = 6
};
constexpr int16_t WIFI_SCAN_RUNNING = -1;
constexpr int16_t WIFI_SCAN_FAILED = -2;
constexpr int WIFI_AUTH_OPEN = 0;

class IPAddress {
 public:
  IPAddress() = default;
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : bytes_{a, b, c, d} {}
  uint8_t operator[](size_t index) const { return bytes_[std::min<size_t>(index, bytes_.size() - 1)]; }
  bool operator==(const IPAddress&) const = default;
  String toString() const {
    return String(bytes_[0]) + "." + String(bytes_[1]) + "." + String(bytes_[2]) + "." + String(bytes_[3]);
  }

 private:
  std::array<uint8_t, 4> bytes_{};
};

class WiFiClass {
 public:
  wifi_mode_t getMode() const { return mode_; }
  bool mode(wifi_mode_t mode) {
    mode_ = mode;
    return true;
  }
  bool disconnect(bool = false, bool = false) {
    mode_ = WIFI_MODE_NULL;
    return true;
  }
  wl_status_t status() const { return WL_DISCONNECTED; }
  void macAddress(uint8_t* destination) const {
    if (destination != nullptr) {
      constexpr uint8_t address[6] = {0x02, 0x43, 0x50, 0x00, 0x00, 0x01};
      std::copy(std::begin(address), std::end(address), destination);
    }
  }
  String macAddress() const { return "02:43:50:00:00:01"; }
  void scanDelete() {}
  int16_t scanNetworks(bool = false) { return WIFI_SCAN_FAILED; }
  int16_t scanComplete() const { return WIFI_SCAN_FAILED; }
  String SSID(int) const { return {}; }
  String SSID() const { return {}; }
  int32_t RSSI(int) const { return 0; }
  int32_t RSSI() const { return 0; }
  int encryptionType(int) const { return WIFI_AUTH_OPEN; }
  void persistent(bool) {}
  bool setHostname(const char*) { return false; }
  wl_status_t begin(const char*, const char* = nullptr) { return WL_DISCONNECTED; }
  IPAddress localIP() const { return {}; }
  bool softAP(const char*, const char* = nullptr, int = 1, bool = false, int = 4) { return false; }
  bool softAPdisconnect(bool = false) { return true; }
  IPAddress softAPIP() const { return {}; }

 private:
  wifi_mode_t mode_ = WIFI_MODE_NULL;
};

extern WiFiClass WiFi;
