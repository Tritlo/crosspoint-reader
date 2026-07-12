#include "emulator/FramedStream.h"

#include <charconv>
#include <optional>
#include <string_view>

namespace emulator {

FramedStream::ReadResult FramedStream::read(std::string& message, std::string& error) {
  std::string line;
  std::optional<size_t> contentLength;

  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;

    constexpr std::string_view prefix = "Content-Length:";
    if (!std::string_view(line).starts_with(prefix)) continue;

    std::string_view value(line);
    value.remove_prefix(prefix.size());
    while (!value.empty() && value.front() == ' ') value.remove_prefix(1);

    size_t parsed = 0;
    const auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size()) {
      error = "invalid Content-Length";
      return ReadResult::Error;
    }
    contentLength = parsed;
  }

  if (!input && !contentLength) return input.eof() ? ReadResult::EndOfStream : ReadResult::Error;
  if (!contentLength) {
    error = "missing Content-Length";
    return ReadResult::Error;
  }
  if (*contentLength > MAX_MESSAGE_SIZE) {
    error = "message exceeds 1 MiB limit";
    return ReadResult::Error;
  }

  message.resize(*contentLength);
  input.read(message.data(), static_cast<std::streamsize>(*contentLength));
  if (input.gcount() != static_cast<std::streamsize>(*contentLength)) {
    error = "truncated message body";
    return ReadResult::Error;
  }
  return ReadResult::Message;
}

bool FramedStream::write(const std::string& message) {
  output << "Content-Length: " << message.size() << "\r\n\r\n" << message;
  output.flush();
  return static_cast<bool>(output);
}

}  // namespace emulator
