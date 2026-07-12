#include "emulator/DirectoryStorage.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>

namespace emulator {
namespace {

constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

struct FixtureEntry {
  std::filesystem::path source;
  std::filesystem::path relative;
  bool directory;
};

void hashBytes(uint64_t& hash, const void* bytes, size_t size) {
  const auto* data = static_cast<const uint8_t*>(bytes);
  for (size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= FNV_PRIME;
  }
}

void hashString(uint64_t& hash, std::string_view value) {
  hashBytes(hash, value.data(), value.size());
  const uint8_t separator = 0;
  hashBytes(hash, &separator, 1);
}

std::string identityString(uint64_t hash) {
  std::ostringstream output;
  output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
  return output.str();
}

bool collectFixtureEntries(const std::filesystem::path& fixture, std::vector<FixtureEntry>& entries,
                           std::string& error) {
  std::error_code filesystemError;
  if (!std::filesystem::is_directory(fixture, filesystemError) || filesystemError) {
    error = "SD fixture is not a readable directory: " + fixture.string();
    return false;
  }

  std::filesystem::recursive_directory_iterator iterator(fixture, filesystemError);
  const std::filesystem::recursive_directory_iterator end;
  if (filesystemError) {
    error = "failed to enumerate SD fixture: " + filesystemError.message();
    return false;
  }
  while (iterator != end) {
    const auto status = iterator->symlink_status(filesystemError);
    if (filesystemError) {
      error = "failed to inspect SD fixture entry: " + filesystemError.message();
      return false;
    }
    const auto relative = std::filesystem::relative(iterator->path(), fixture, filesystemError);
    if (filesystemError) {
      error = "failed to resolve SD fixture entry: " + filesystemError.message();
      return false;
    }
    if (std::filesystem::is_symlink(status)) {
      error = "SD fixtures may not contain symlinks: " + relative.generic_string();
      return false;
    }
    if (!std::filesystem::is_directory(status) && !std::filesystem::is_regular_file(status)) {
      error = "SD fixture contains an unsupported entry: " + relative.generic_string();
      return false;
    }
    entries.push_back({iterator->path(), relative, std::filesystem::is_directory(status)});
    iterator.increment(filesystemError);
    if (filesystemError) {
      error = "failed to enumerate SD fixture: " + filesystemError.message();
      return false;
    }
  }
  std::sort(entries.begin(), entries.end(), [](const FixtureEntry& left, const FixtureEntry& right) {
    return left.relative.generic_string() < right.relative.generic_string();
  });
  return true;
}

bool copyAndHashFixture(const std::filesystem::path& fixture, const std::filesystem::path& destination,
                        StorageMetadata& metadata, std::string& error) {
  std::vector<FixtureEntry> entries;
  if (!collectFixtureEntries(fixture, entries, error)) return false;

  uint64_t hash = FNV_OFFSET_BASIS;
  std::array<char, 64 * 1024> buffer;
  for (const auto& entry : entries) {
    const std::string relative = entry.relative.generic_string();
    const char type = entry.directory ? 'D' : 'F';
    hashBytes(hash, &type, 1);
    hashString(hash, relative);

    std::error_code filesystemError;
    const auto outputPath = destination / entry.relative;
    if (entry.directory) {
      std::filesystem::create_directories(outputPath, filesystemError);
      if (filesystemError) {
        error = "failed to create run SD directory: " + filesystemError.message();
        return false;
      }
      ++metadata.fixtureDirectoryCount;
      continue;
    }

    std::filesystem::create_directories(outputPath.parent_path(), filesystemError);
    if (filesystemError) {
      error = "failed to create run SD parent directory: " + filesystemError.message();
      return false;
    }
    std::ifstream input(entry.source, std::ios::binary);
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!input || !output) {
      error = "failed to copy SD fixture file: " + relative;
      return false;
    }
    while (input) {
      input.read(buffer.data(), buffer.size());
      const auto count = input.gcount();
      if (count <= 0) break;
      output.write(buffer.data(), count);
      hashBytes(hash, buffer.data(), static_cast<size_t>(count));
      metadata.fixtureBytes += static_cast<uint64_t>(count);
    }
    if (!input.eof() || !output) {
      error = "failed while copying SD fixture file: " + relative;
      return false;
    }
    ++metadata.fixtureFileCount;
  }
  metadata.fixtureIdentity = identityString(hash);
  return true;
}

}  // namespace

DirectoryStorage::DirectoryStorage(const Configuration& configuration)
    : configuration(configuration), storageRoot(configuration.artifactDirectory / "sd") {}

bool DirectoryStorage::begin(std::string& error) {
  if (initialized) return true;
  std::error_code filesystemError;
  if (std::filesystem::exists(storageRoot, filesystemError)) {
    error = "run SD directory already exists: " + storageRoot.string();
    return false;
  }
  std::filesystem::create_directories(storageRoot, filesystemError);
  if (filesystemError) {
    error = "failed to create run SD directory: " + filesystemError.message();
    return false;
  }

  if (configuration.sdFixtureDirectory) {
    if (!copyAndHashFixture(*configuration.sdFixtureDirectory, storageRoot, storageMetadata, error)) return false;
  } else {
    storageMetadata.fixtureIdentity = identityString(FNV_OFFSET_BASIS);
  }
  initialized = true;
  return true;
}

std::optional<std::filesystem::path> DirectoryStorage::hostPath(std::string_view devicePath, std::string& error) const {
  std::filesystem::path relative;
  for (const auto& component : std::filesystem::path(devicePath)) {
    if (component == "/" || component == "." || component.empty()) continue;
    if (component == "..") {
      error = "device path may not contain '..'";
      return std::nullopt;
    }
    relative /= component;
  }
  return storageRoot / relative;
}

std::vector<std::string> DirectoryStorage::listFiles(std::string_view devicePath, size_t maxFiles,
                                                     std::string& error) const {
  std::vector<std::string> result;
  const auto path = hostPath(devicePath, error);
  if (!path) return result;
  std::error_code filesystemError;
  std::filesystem::directory_iterator iterator(*path, filesystemError);
  const std::filesystem::directory_iterator end;
  if (filesystemError) {
    error = "failed to list device directory: " + filesystemError.message();
    return result;
  }
  while (iterator != end) {
    if (iterator->is_regular_file(filesystemError)) result.push_back(iterator->path().filename().string());
    if (filesystemError) {
      error = "failed to inspect device directory: " + filesystemError.message();
      return {};
    }
    iterator.increment(filesystemError);
    if (filesystemError) {
      error = "failed to list device directory: " + filesystemError.message();
      return {};
    }
  }
  std::sort(result.begin(), result.end());
  if (result.size() > maxFiles) result.resize(maxFiles);
  return result;
}

bool DirectoryStorage::readFile(std::string_view devicePath, std::vector<uint8_t>& contents, std::string& error) const {
  const auto path = hostPath(devicePath, error);
  if (!path) return false;
  std::ifstream input(*path, std::ios::binary);
  if (!input) {
    error = "failed to open device file for reading: " + std::string(devicePath);
    return false;
  }
  contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (input.bad()) {
    error = "failed to read device file: " + std::string(devicePath);
    return false;
  }
  return true;
}

bool DirectoryStorage::writeFile(std::string_view devicePath, const uint8_t* contents, size_t size,
                                 std::string& error) {
  const auto path = hostPath(devicePath, error);
  if (!path) return false;
  std::ofstream output(*path, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "failed to open device file for writing: " + std::string(devicePath);
    return false;
  }
  output.write(reinterpret_cast<const char*>(contents), static_cast<std::streamsize>(size));
  if (!output) {
    error = "failed to write device file: " + std::string(devicePath);
    return false;
  }
  return true;
}

bool DirectoryStorage::mkdir(std::string_view devicePath, bool recursive, std::string& error) {
  const auto path = hostPath(devicePath, error);
  if (!path) return false;
  std::error_code filesystemError;
  const bool created = recursive ? std::filesystem::create_directories(*path, filesystemError)
                                 : std::filesystem::create_directory(*path, filesystemError);
  if (filesystemError) error = "failed to create device directory: " + filesystemError.message();
  return !filesystemError && (created || std::filesystem::is_directory(*path));
}

bool DirectoryStorage::exists(std::string_view devicePath) const {
  std::string error;
  const auto path = hostPath(devicePath, error);
  return path && std::filesystem::exists(*path);
}

bool DirectoryStorage::remove(std::string_view devicePath, std::string& error) {
  const auto path = hostPath(devicePath, error);
  if (!path) return false;
  std::error_code filesystemError;
  const bool removed = std::filesystem::remove(*path, filesystemError);
  if (filesystemError) error = "failed to remove device file: " + filesystemError.message();
  return removed && !filesystemError;
}

bool DirectoryStorage::rename(std::string_view oldDevicePath, std::string_view newDevicePath, std::string& error) {
  const auto oldPath = hostPath(oldDevicePath, error);
  if (!oldPath) return false;
  const auto newPath = hostPath(newDevicePath, error);
  if (!newPath) return false;
  std::error_code filesystemError;
  std::filesystem::rename(*oldPath, *newPath, filesystemError);
  if (filesystemError) error = "failed to rename device path: " + filesystemError.message();
  return !filesystemError;
}

bool DirectoryStorage::rmdir(std::string_view devicePath, bool recursive, std::string& error) {
  const auto path = hostPath(devicePath, error);
  if (!path || *path == storageRoot) {
    if (path) error = "refusing to remove the run SD root";
    return false;
  }
  std::error_code filesystemError;
  const uintmax_t removed = recursive ? std::filesystem::remove_all(*path, filesystemError)
                                      : static_cast<uintmax_t>(std::filesystem::remove(*path, filesystemError));
  if (filesystemError) error = "failed to remove device directory: " + filesystemError.message();
  return removed > 0 && !filesystemError;
}

}  // namespace emulator
