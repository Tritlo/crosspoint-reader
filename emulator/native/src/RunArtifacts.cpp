#include "emulator/RunArtifacts.h"

#include <utility>

namespace emulator {
namespace {

constexpr uint32_t TRACE_VERSION = 1;
constexpr uint32_t PANEL_MODEL_VERSION = 1;

bool writeJsonFile(const std::filesystem::path& path, const JsonDocument& document, std::string& error) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "failed to open " + path.string();
    return false;
  }
  std::string json;
  serializeJsonPretty(document, json);
  output << json << '\n';
  if (!output) {
    error = "failed to write " + path.string();
    return false;
  }
  return true;
}

}  // namespace

bool RunArtifacts::begin(const StorageMetadata& storage, std::string& error) {
  std::error_code filesystemError;
  std::filesystem::create_directories(configuration.artifactDirectory, filesystemError);
  if (filesystemError) {
    error = "failed to create artifact directory: " + filesystemError.message();
    return false;
  }

  JsonDocument manifest;
  manifest["traceVersion"] = TRACE_VERSION;
  manifest["panelModelVersion"] = PANEL_MODEL_VERSION;
  manifest["deviceProfile"] = configuration.profile.id;
  manifest["panelWidth"] = configuration.profile.panelWidth;
  manifest["panelHeight"] = configuration.profile.panelHeight;
  manifest["controller"] = configuration.profile.controller;
  manifest["reviewRotationDegrees"] = configuration.profile.reviewRotationDegrees;
  manifest["timingProfile"] = "development-uncalibrated-v0";
  manifest["rtcStart"] = configuration.rtcStart;
  manifest["randomSeed"] = configuration.randomSeed;
  manifest["initialPanel"] = configuration.initialPanel;
  if (configuration.initialPanelPng) {
    manifest["initialPanelSource"] = std::filesystem::absolute(*configuration.initialPanelPng).string();
  }
  JsonObject fixture = manifest["fixture"].to<JsonObject>();
  fixture["identity"] = storage.fixtureIdentity;
  fixture["files"] = storage.fixtureFileCount;
  fixture["directories"] = storage.fixtureDirectoryCount;
  fixture["bytes"] = storage.fixtureBytes;
  JsonObject environment = manifest["environment"].to<JsonObject>();
  environment["locale"] = "C";
  environment["timezone"] = "UTC";
  environment["filesystemOrdering"] = "sorted-utf8-device-path-v1";
  if (!writeJsonFile(configuration.artifactDirectory / "manifest.json", manifest, error)) return false;

  events.open(configuration.artifactDirectory / "events.jsonl", std::ios::binary | std::ios::trunc);
  if (!events) {
    error = "failed to open events.jsonl";
    return false;
  }
  return true;
}

void RunArtifacts::record(std::string_view type, uint64_t sequence, uint64_t simulatedTimeUs,
                          const JsonObjectConst& fields) {
  if (!events) return;
  JsonDocument event;
  event["sequence"] = sequence;
  event["simulatedTimeUs"] = simulatedTimeUs;
  event["type"] = type;
  for (JsonPairConst field : fields) event[field.key()] = field.value();
  std::string json;
  serializeJson(event, json);
  events << json << '\n';
  events.flush();
}

}  // namespace emulator
