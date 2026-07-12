#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "emulator/Configuration.h"

namespace emulator {

struct StorageMetadata {
  std::string fixtureIdentity;
  uint64_t fixtureFileCount = 0;
  uint64_t fixtureDirectoryCount = 0;
  uint64_t fixtureBytes = 0;
};

class DirectoryStorage {
 public:
  explicit DirectoryStorage(const Configuration& configuration);

  bool begin(std::string& error);
  bool ready() const { return initialized; }
  const std::filesystem::path& root() const { return storageRoot; }
  const Configuration& config() const { return configuration; }
  const StorageMetadata& metadata() const { return storageMetadata; }

  std::vector<std::string> listFiles(std::string_view devicePath, size_t maxFiles, std::string& error) const;
  bool readFile(std::string_view devicePath, std::vector<uint8_t>& contents, std::string& error) const;
  bool writeFile(std::string_view devicePath, const uint8_t* contents, size_t size, std::string& error);
  bool mkdir(std::string_view devicePath, bool recursive, std::string& error);
  bool exists(std::string_view devicePath) const;
  bool remove(std::string_view devicePath, std::string& error);
  bool rename(std::string_view oldDevicePath, std::string_view newDevicePath, std::string& error);
  bool rmdir(std::string_view devicePath, bool recursive, std::string& error);

  std::optional<std::filesystem::path> hostPath(std::string_view devicePath, std::string& error) const;

 private:
  const Configuration& configuration;
  std::filesystem::path storageRoot;
  StorageMetadata storageMetadata;
  bool initialized = false;
};

}  // namespace emulator
