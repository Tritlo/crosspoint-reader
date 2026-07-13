#include <Arduino.h>
#include <HalStorage.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <utility>

#include "emulator/DirectoryStorage.h"
#include "emulator/FreeRtosCompat.h"

namespace {

uint64_t scaledTransferUs(uint64_t bytes, uint64_t measuredUs, uint64_t basisBytes) {
  if (bytes == 0 || measuredUs == 0 || basisBytes == 0) return 0;
  const uint64_t whole = bytes / basisBytes;
  const uint64_t remainder = bytes % basisBytes;
  return whole * measuredUs + (remainder * measuredUs + basisBytes - 1) / basisBytes;
}

bool isDerivedImage(std::string_view path) {
  if (!path.starts_with("/.crosspoint/")) return false;
  return path.ends_with(".jpg") || path.ends_with(".jpeg") || path.ends_with(".png") || path.ends_with(".pxc");
}

void delayStorageOpen(bool writable, std::string_view path, bool directory = false) {
  const auto& timing = emulator::runtimeStorage().config().timing;
  if (!timing.calibrated || isDerivedImage(path) || emulator::runtimeColdPostIndexTimingActive() ||
      emulator::runtimeWarmOpenTimingActive() || emulator::runtimeImagePreparationActive() ||
      emulator::runtimeSectionTimingActive()) {
    return;
  }
  emulator::runtimeDelay(directory  ? timing.storage.directoryRootOpenUs
                         : writable ? timing.storage.writeOpenUs
                                    : timing.storage.readOpenUs);
}

void delayDirectoryNext() {
  const auto& timing = emulator::runtimeStorage().config().timing;
  if (timing.calibrated) emulator::runtimeDelay(timing.storage.directoryNextUs);
}

void delayStorageTransfer(bool writable, std::string_view path, uint64_t bytes, bool firstTransfer = false) {
  const auto& timing = emulator::runtimeStorage().config().timing;
  if (!timing.calibrated || isDerivedImage(path) || emulator::runtimeThumbnailTimingActive() ||
      emulator::runtimeColdTocTimingActive() || emulator::runtimeColdPostIndexTimingActive() ||
      emulator::runtimeWarmOpenTimingActive() || emulator::runtimeImagePreparationActive() ||
      emulator::runtimeSectionTimingActive()) {
    return;
  }
  const uint64_t measuredUs = writable ? timing.storage.writeTransferUs : timing.storage.readTransferUs;
  const uint64_t setupUs = writable && firstTransfer ? timing.storage.writeTransferSetupUs : 0;
  emulator::runtimeDelay(setupUs + scaledTransferUs(bytes, measuredUs, timing.storage.transferBasisBytes));
}

void delayStorageClose(std::string_view path) {
  const auto& timing = emulator::runtimeStorage().config().timing;
  if (timing.calibrated && !isDerivedImage(path) && !emulator::runtimeColdPostIndexTimingActive() &&
      !emulator::runtimeWarmOpenTimingActive() && !emulator::runtimeImagePreparationActive() &&
      !emulator::runtimeSectionTimingActive()) {
    emulator::runtimeDelay(timing.storage.closeUs);
  }
}

bool flagsWritable(oflag_t flags) { return (flags & O_WRONLY) != 0 || (flags & O_RDWR) != 0; }

}  // namespace

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  storageMutex = xSemaphoreCreateRecursiveMutex();
  assert(storageMutex != nullptr);
}

bool HalStorage::begin() {
  initialized = emulator::runtimeStorage().ready();
  return initialized;
}

bool HalStorage::ready() const { return initialized; }

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTakeRecursive(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGiveRecursive(HalStorage::getInstance().storageMutex); }
};

class HalFile::Impl {
 public:
  Impl(std::string devicePath, std::filesystem::path hostPath, oflag_t flags)
      : devicePath(std::move(devicePath)), hostPath(std::move(hostPath)), flags(flags) {
    std::error_code error;
    const bool exists = std::filesystem::exists(this->hostPath, error);
    if (error) return;
    directory = exists && std::filesystem::is_directory(this->hostPath, error);
    if (error) return;
    if (directory) {
      std::filesystem::directory_iterator iterator(this->hostPath, error);
      const std::filesystem::directory_iterator end;
      while (!error && iterator != end) {
        entries.push_back(iterator->path());
        iterator.increment(error);
      }
      if (error) {
        entries.clear();
        directory = false;
        return;
      }
      std::sort(entries.begin(), entries.end());
      opened = true;
      return;
    }

    const bool writable = (flags & O_WRONLY) != 0 || (flags & O_RDWR) != 0;
    const bool readable = !writable || (flags & O_RDWR) != 0;
    if ((flags & O_CREAT) != 0 && !std::filesystem::exists(this->hostPath)) {
      std::ofstream create(this->hostPath, std::ios::binary);
    }
    std::ios::openmode mode = std::ios::binary;
    if (readable) mode |= std::ios::in;
    if (writable) mode |= std::ios::out;
    if ((flags & O_TRUNC) != 0) mode |= std::ios::trunc;
    if ((flags & O_APPEND) != 0) mode |= std::ios::app;
    stream.open(this->hostPath, mode);
    opened = stream.is_open();
    if (!opened) {
      std::cerr << "emulator: failed to open SD file '" << this->hostPath.string() << "' flags=" << flags << '\n';
    }
    if (opened && ((flags & O_APPEND) != 0 || (flags & O_AT_END) != 0)) cursor = size();
  }

  std::string devicePath;
  std::filesystem::path hostPath;
  oflag_t flags;
  std::fstream stream;
  bool directory = false;
  bool opened = false;
  bool transferStarted = false;
  uint64_t cursor = 0;
  std::vector<std::filesystem::path> entries;
  size_t directoryIndex = 0;

  uint64_t size() const {
    std::error_code error;
    const uint64_t result = std::filesystem::file_size(hostPath, error);
    return error ? 0 : result;
  }
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  StorageLock lock;
  std::string error;
  const auto files = emulator::runtimeStorage().listFiles(path, static_cast<size_t>(std::max(0, maxFiles)), error);
  emulator::runtimeTraceStorage("list", path, files.size(), error.empty());
  return std::vector<String>(files.begin(), files.end());
}

String HalStorage::readFile(const char* path) {
  StorageLock lock;
  std::string error;
  std::vector<uint8_t> contents;
  if (!emulator::runtimeStorage().readFile(path, contents, error)) {
    emulator::runtimeTraceStorage("read", path, 0, false);
    return {};
  }
  constexpr size_t maxSize = 50000;
  const size_t size = std::min(contents.size(), maxSize);
  delayStorageOpen(false, path);
  delayStorageTransfer(false, path, size);
  delayStorageClose(path);
  emulator::runtimeTraceStorage("read", path, size, true);
  return String(contents.data(), static_cast<unsigned int>(size));
}

bool HalStorage::readFileToStream(const char* path, Print& output, size_t chunkSize) {
  StorageLock lock;
  HalFile file = open(path, O_RDONLY);
  if (!file) return false;
  std::vector<uint8_t> buffer(std::max<size_t>(1, std::min<size_t>(chunkSize, 256)));
  while (file.available()) {
    const int count = file.read(buffer.data(), buffer.size());
    if (count <= 0) break;
    output.write(buffer.data(), static_cast<size_t>(count));
  }
  return true;
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  StorageLock lock;
  if (buffer == nullptr || bufferSize == 0) return 0;
  HalFile file = open(path, O_RDONLY);
  if (!file) {
    buffer[0] = 0;
    return 0;
  }
  const size_t limit = maxBytes == 0 ? bufferSize - 1 : std::min(maxBytes, bufferSize - 1);
  const int count = file.read(buffer, limit);
  const size_t result = count > 0 ? static_cast<size_t>(count) : 0;
  buffer[result] = 0;
  return result;
}

bool HalStorage::writeFile(const char* path, const String& content) {
  StorageLock lock;
  std::string error;
  const bool success = emulator::runtimeStorage().writeFile(path, reinterpret_cast<const uint8_t*>(content.data()),
                                                            content.size(), error);
  if (success) {
    delayStorageOpen(true, path);
    delayStorageTransfer(true, path, content.size(), true);
    delayStorageClose(path);
  }
  emulator::runtimeTraceStorage("write", path, content.size(), success);
  return success;
}

bool HalStorage::ensureDirectoryExists(const char* path) {
  StorageLock lock;
  std::string error;
  const bool success = emulator::runtimeStorage().exists(path) || emulator::runtimeStorage().mkdir(path, true, error);
  emulator::runtimeTraceStorage("mkdir", path, 0, success);
  return success;
}

HalFile HalStorage::open(const char* path, oflag_t flags) {
  StorageLock lock;
  std::string error;
  const auto hostPath = emulator::runtimeStorage().hostPath(path, error);
  if (!hostPath) {
    emulator::runtimeTraceStorage("open", path, 0, false);
    return {};
  }
  auto impl = std::make_unique<HalFile::Impl>(path, *hostPath, flags);
  if (!impl->opened) {
    emulator::runtimeTraceStorage("open", path, 0, false);
    return {};
  }
  delayStorageOpen(flagsWritable(flags), impl->devicePath, impl->directory);
  emulator::runtimeTraceStorage("open", path, 0, true);
  return HalFile(std::move(impl));
}

bool HalStorage::mkdir(const char* path, bool recursive) {
  StorageLock lock;
  std::string error;
  const bool success = emulator::runtimeStorage().mkdir(path, recursive, error);
  emulator::runtimeTraceStorage("mkdir", path, 0, success);
  return success;
}

bool HalStorage::exists(const char* path) {
  StorageLock lock;
  const bool success = emulator::runtimeStorage().exists(path);
  emulator::runtimeTraceStorage("exists", path, 0, success);
  return success;
}

bool HalStorage::remove(const char* path) {
  StorageLock lock;
  std::string error;
  const bool success = emulator::runtimeStorage().remove(path, error);
  emulator::runtimeTraceStorage("remove", path, 0, success);
  return success;
}

bool HalStorage::rename(const char* oldPath, const char* newPath) {
  StorageLock lock;
  std::string error;
  const bool success = emulator::runtimeStorage().rename(oldPath, newPath, error);
  emulator::runtimeTraceStorage("rename", oldPath, 0, success);
  return success;
}

bool HalStorage::rmdir(const char* path) {
  StorageLock lock;
  std::string error;
  const bool success = emulator::runtimeStorage().rmdir(path, false, error);
  emulator::runtimeTraceStorage("rmdir", path, 0, success);
  return success;
}

bool HalStorage::openFileForRead(const char*, const char* path, HalFile& file) {
  file = open(path, O_RDONLY);
  return static_cast<bool>(file);
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char*, const char* path, HalFile& file) {
  file = open(path, O_WRITE | O_CREAT | O_TRUNC);
  return static_cast<bool>(file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) {
  StorageLock lock;
  std::string error;
  const bool success = emulator::runtimeStorage().rmdir(path, true, error);
  emulator::runtimeTraceStorage("rmdir-recursive", path, 0, success);
  return success;
}

void HalFile::flush() {
  HalStorage::StorageLock lock;
  if (impl) impl->stream.flush();
}

size_t HalFile::getName(char* name, size_t length) {
  HalStorage::StorageLock lock;
  if (!impl || name == nullptr || length == 0) return 0;
  const std::string filename = impl->hostPath.filename().string();
  const size_t count = std::min(filename.size(), length - 1);
  std::memcpy(name, filename.data(), count);
  name[count] = 0;
  return count;
}

size_t HalFile::size() { return impl ? static_cast<size_t>(impl->size()) : 0; }
size_t HalFile::fileSize() { return size(); }
uint64_t HalFile::fileSize64() { return impl ? impl->size() : 0; }

bool HalFile::seek(size_t position) { return seek64(position); }
bool HalFile::seek64(uint64_t position) {
  HalStorage::StorageLock lock;
  if (!impl || impl->directory) return false;
  impl->cursor = position;
  emulator::runtimeTraceStorage("seek", impl->devicePath, position, true);
  return true;
}
bool HalFile::seekCur(int64_t offset) {
  if (!impl || (offset < 0 && static_cast<uint64_t>(-offset) > impl->cursor)) return false;
  return seek64(static_cast<uint64_t>(static_cast<int64_t>(impl->cursor) + offset));
}
bool HalFile::seekSet(size_t offset) { return seek64(offset); }

int HalFile::available() const {
  if (!impl || impl->directory) return 0;
  const uint64_t size = impl->size();
  if (impl->cursor >= size) return 0;
  return static_cast<int>(std::min<uint64_t>(size - impl->cursor, std::numeric_limits<int>::max()));
}
size_t HalFile::position() const { return impl ? static_cast<size_t>(impl->cursor) : 0; }

int HalFile::read(void* buffer, size_t count) {
  HalStorage::StorageLock lock;
  if (!impl || impl->directory || buffer == nullptr) return -1;
  impl->stream.clear();
  impl->stream.seekg(static_cast<std::streamoff>(impl->cursor));
  impl->stream.read(static_cast<char*>(buffer), static_cast<std::streamsize>(count));
  const int result = static_cast<int>(impl->stream.gcount());
  impl->cursor += static_cast<uint64_t>(result);
  delayStorageTransfer(false, impl->devicePath, static_cast<uint64_t>(std::max(0, result)));
  emulator::runtimeTraceStorage("read", impl->devicePath, static_cast<uint64_t>(std::max(0, result)), result >= 0);
  return result;
}

int HalFile::read() {
  uint8_t value = 0;
  return read(&value, 1) == 1 ? value : -1;
}

size_t HalFile::write(const void* buffer, size_t count) {
  HalStorage::StorageLock lock;
  if (!impl || impl->directory || buffer == nullptr) return 0;
  impl->stream.clear();
  if ((impl->flags & O_APPEND) == 0) impl->stream.seekp(static_cast<std::streamoff>(impl->cursor));
  impl->stream.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(count));
  if (!impl->stream) {
    emulator::runtimeTraceStorage("write", impl->devicePath, 0, false);
    return 0;
  }
  impl->cursor += count;
  const bool firstTransfer = !impl->transferStarted && count != 0;
  impl->transferStarted = impl->transferStarted || count != 0;
  delayStorageTransfer(true, impl->devicePath, count, firstTransfer);
  emulator::runtimeTraceStorage("write", impl->devicePath, count, true);
  return count;
}

size_t HalFile::write(uint8_t value) { return write(&value, 1); }

bool HalFile::rename(const char* newPath) {
  HalStorage::StorageLock lock;
  if (!impl) return false;
  impl->stream.close();
  std::string error;
  if (!emulator::runtimeStorage().rename(impl->devicePath, newPath, error)) return false;
  impl->devicePath = newPath;
  const auto path = emulator::runtimeStorage().hostPath(newPath, error);
  if (path) impl->hostPath = *path;
  return true;
}

bool HalFile::isDirectory() const { return impl && impl->directory; }
void HalFile::rewindDirectory() {
  if (impl) impl->directoryIndex = 0;
}
bool HalFile::close() {
  HalStorage::StorageLock lock;
  if (!impl) return false;
  impl->stream.close();
  impl->opened = false;
  delayStorageClose(impl->devicePath);
  emulator::runtimeTraceStorage("close", impl->devicePath, 0, true);
  return true;
}

HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  if (!impl || !impl->directory) return {};
  delayDirectoryNext();
  if (impl->directoryIndex >= impl->entries.size()) return {};
  const auto child = impl->entries[impl->directoryIndex++];
  std::error_code error;
  const auto relative = std::filesystem::relative(child, emulator::runtimeStorage().root(), error);
  if (error) return {};
  const std::string devicePath = "/" + relative.generic_string();
  auto childImpl = std::make_unique<Impl>(devicePath, child, O_RDONLY);
  if (!childImpl->opened) return {};
  emulator::runtimeTraceStorage("open-entry", devicePath, 0, true);
  return HalFile(std::move(childImpl));
}

bool HalFile::isOpen() const { return impl && impl->opened; }
HalFile::operator bool() const { return isOpen(); }
