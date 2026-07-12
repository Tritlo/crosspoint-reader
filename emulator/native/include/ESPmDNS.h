#pragma once

class MDNSResponder {
 public:
  bool begin(const char*) { return false; }
  void end() {}
};

extern MDNSResponder MDNS;
