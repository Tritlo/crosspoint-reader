#pragma once

#include <cstddef>

class Print;

class Printable {
 public:
  virtual ~Printable() = default;
  virtual size_t printTo(Print& print) const = 0;
};
