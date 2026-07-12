#pragma once

#include <cstddef>
#include <istream>
#include <ostream>
#include <string>

namespace emulator {

class FramedStream {
 public:
  enum class ReadResult { Message, EndOfStream, Error };

  FramedStream(std::istream& input, std::ostream& output) : input(input), output(output) {}

  ReadResult read(std::string& message, std::string& error);
  bool write(const std::string& message);

 private:
  static constexpr size_t MAX_MESSAGE_SIZE = 1024 * 1024;

  std::istream& input;
  std::ostream& output;
};

}  // namespace emulator
