#include <cstdarg>

#include "Logging.h"
#include "network/FirmwareFlasher.h"
#include "network/HttpDownloader.h"
#include "network/OtaUpdater.h"

MySerialImpl MySerialImpl::instance;

size_t MySerialImpl::printf(const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  const size_t written = logSerial.vprintf(format, arguments);
  va_end(arguments);
  return written;
}

size_t MySerialImpl::write(uint8_t byte) { return logSerial.write(byte); }
size_t MySerialImpl::write(const uint8_t* buffer, size_t size) { return logSerial.write(buffer, size); }
void MySerialImpl::flush() { logSerial.flush(); }

bool HttpDownloader::fetchUrl(const std::string&, std::string& output, const std::string&, const std::string&) {
  output.clear();
  return false;
}

bool HttpDownloader::fetchUrl(const std::string&, Stream&, const std::string&, const std::string&) { return false; }

bool HttpDownloader::fetchUrl(const std::string&, const DataCallback&, const std::string&, const std::string&) {
  return false;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string&, const std::string&, ProgressCallback,
                                                             bool*, const std::string&, const std::string&) {
  return HTTP_ERROR;
}

bool OtaUpdater::isUpdateNewer() const { return false; }
const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }
OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() { return HTTP_ERROR; }
OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback, void*) { return HTTP_ERROR; }

namespace firmware_flash {
Result flashFromSdPath(const char*, ProgressCb, void*, bool) { return Result::NO_PARTITION; }
Result validateImageFile(const char*, size_t) { return Result::NO_PARTITION; }

const char* resultName(Result result) {
  switch (result) {
    case Result::OK:
      return "ok";
    case Result::OPEN_FAIL:
      return "open-fail";
    case Result::TOO_SMALL:
      return "too-small";
    case Result::TOO_LARGE:
      return "too-large";
    case Result::BAD_MAGIC:
      return "bad-magic";
    case Result::BAD_SEGMENTS:
      return "bad-segments";
    case Result::BAD_CHECKSUM:
      return "bad-checksum";
    case Result::BAD_SHA:
      return "bad-sha";
    case Result::BAD_SIZE:
      return "bad-size";
    case Result::NO_PARTITION:
      return "unsupported";
    case Result::OOM:
      return "oom";
    case Result::READ_FAIL:
      return "read-fail";
    case Result::ERASE_FAIL:
      return "erase-fail";
    case Result::WRITE_FAIL:
      return "write-fail";
    case Result::OTADATA_FAIL:
      return "otadata-fail";
  }
  return "unknown";
}
}  // namespace firmware_flash

extern "C" uint32_t uzlib_adler32(const void* data, unsigned int length, uint32_t previous) {
  constexpr uint32_t modulus = 65521;
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t a = previous & 0xFFFFU;
  uint32_t b = previous >> 16;
  for (unsigned int index = 0; index < length; ++index) {
    a = (a + bytes[index]) % modulus;
    b = (b + a) % modulus;
  }
  return (b << 16) | a;
}

extern "C" uint32_t uzlib_crc32(const void* data, unsigned int length, uint32_t previous) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t crc = ~previous;
  for (unsigned int index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
