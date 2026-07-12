#pragma once

#include <Stream.h>

#include <string>

namespace freeink {

class SecureHttpClient {
 public:
  void setInsecure() {}
  bool begin(const std::string&) { return false; }
  void addHeader(const std::string&, const std::string&) {}
  int GET() { return -1; }
  int sendRequest(const std::string&, const std::string&) { return -1; }
  std::string getString() const { return {}; }
  void end() {}
};

}  // namespace freeink
