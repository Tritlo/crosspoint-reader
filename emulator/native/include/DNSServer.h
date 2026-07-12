#pragma once

#include <WiFi.h>

#include <cstdint>

enum class DNSReplyCode { NoError };

class DNSServer {
 public:
  void setErrorReplyCode(DNSReplyCode) {}
  bool start(uint16_t, const char*, IPAddress) { return false; }
  void processNextRequest() {}
  void stop() {}
};
