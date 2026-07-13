#include <ArduinoJson.h>
#include <EmulatorNative.h>
#include <Epub.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "emulator/ApplicationLifecycle.h"
#include "emulator/Configuration.h"
#include "emulator/DeterministicScheduler.h"
#include "emulator/DirectoryStorage.h"
#include "emulator/FramedStream.h"
#include "emulator/FreeRtosCompat.h"
#include "emulator/PanelModel.h"
#include "emulator/PhysicalControls.h"
#include "emulator/PngWriter.h"
#include "emulator/RunArtifacts.h"
#include "emulator/SimulatedClock.h"

void setup() __attribute__((weak));
void loop() __attribute__((weak));
extern MappedInputManager mappedInputManager;

namespace emulator {
namespace {

constexpr uint32_t PROTOCOL_VERSION = 1;
constexpr std::string_view SCHEDULER_MODEL = "deterministic-single-core-v1";
constexpr std::string_view PHYSICAL_BUTTON_NAMES[] = {"back", "confirm", "left", "right", "up", "down", "power"};

int physicalButtonIndex(std::string_view name) {
  for (size_t index = 0; index < std::size(PHYSICAL_BUTTON_NAMES); ++index) {
    if (PHYSICAL_BUTTON_NAMES[index] == name) return static_cast<int>(index);
  }
  return -1;
}

std::string_view physicalButtonName(uint8_t index) {
  return index < std::size(PHYSICAL_BUTTON_NAMES) ? PHYSICAL_BUTTON_NAMES[index] : std::string_view{};
}

std::optional<MappedInputManager::Button> protocolAction(std::string_view name) {
  if (name == "back") return MappedInputManager::Button::Back;
  if (name == "confirm") return MappedInputManager::Button::Confirm;
  if (name == "left") return MappedInputManager::Button::Left;
  if (name == "right") return MappedInputManager::Button::Right;
  if (name == "up") return MappedInputManager::Button::Up;
  if (name == "down") return MappedInputManager::Button::Down;
  if (name == "power") return MappedInputManager::Button::Power;
  if (name == "page_back") return MappedInputManager::Button::PageBack;
  if (name == "page_forward") return MappedInputManager::Button::PageForward;
  return std::nullopt;
}

enum class ProtocolActivityId {
  Unknown,
  Boot,
  Home,
  FileBrowser,
  Settings,
  Sleep,
  ReaderEpub,
  ReaderEpubMenu,
  ReaderEpubChapters,
  ReaderEpubPercent
};

ProtocolActivityId protocolActivityId(std::string_view name) {
  if (name == "boot") return ProtocolActivityId::Boot;
  if (name == "home") return ProtocolActivityId::Home;
  if (name == "file_browser") return ProtocolActivityId::FileBrowser;
  if (name == "settings") return ProtocolActivityId::Settings;
  if (name == "sleep") return ProtocolActivityId::Sleep;
  if (name == "reader.epub") return ProtocolActivityId::ReaderEpub;
  if (name == "reader.epub.menu") return ProtocolActivityId::ReaderEpubMenu;
  if (name == "reader.epub.chapters") return ProtocolActivityId::ReaderEpubChapters;
  if (name == "reader.epub.percent") return ProtocolActivityId::ReaderEpubPercent;
  return ProtocolActivityId::Unknown;
}

uint64_t panelFrameHash(uint16_t width, uint16_t height, const std::vector<uint8_t>& pixels) {
  uint64_t hash = 14695981039346656037ULL;
  auto add = [&hash](uint8_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
  };
  add(static_cast<uint8_t>(width));
  add(static_cast<uint8_t>(width >> 8));
  add(static_cast<uint8_t>(height));
  add(static_cast<uint8_t>(height >> 8));
  for (const uint8_t pixel : pixels) add(pixel);
  return hash;
}

std::string panelFrameName(uint64_t hash) {
  std::ostringstream name;
  name << "frame-" << std::hex << std::setfill('0') << std::setw(16) << hash << ".png";
  return name.str();
}

class Session {
 public:
  Session(Configuration configuration, FramedStream& stream)
      : configuration(std::move(configuration)),
        stream(stream),
        artifacts(this->configuration),
        storage(this->configuration),
        scheduler(clock),
        runtime(
            scheduler, clock, &storage,
            [this](std::string_view operation, std::string_view path, uint64_t bytes, bool success) {
              JsonDocument document;
              JsonObject fields = document.to<JsonObject>();
              fields["operation"] = operation;
              fields["path"] = path;
              fields["bytes"] = bytes;
              fields["success"] = success;
              record("storage.operation", fields);
            },
            [this](std::string_view event, std::string_view detail, uint64_t value) {
              JsonDocument document;
              JsonObject fields = document.to<JsonObject>();
              fields["detail"] = detail;
              fields["value"] = value;
              record(std::string("panel.") + std::string(event), fields);
            },
            [this](std::string_view event, std::string_view detail, uint64_t value) {
              if (event == "activity") observedActivity.store(protocolActivityId(detail));
              JsonDocument document;
              JsonObject fields = document.to<JsonObject>();
              if (event.starts_with("input-")) {
                fields["control"] = detail;
                fields["heldMs"] = value;
              } else {
                if (!detail.empty()) fields["activityId"] = detail;
                fields["generation"] = value;
              }
              record(std::string("application.") + std::string(event), fields);
            }),
        application(scheduler, setup, loop) {}

  ~Session() { scheduler.shutdown(); }

  int serve() {
    std::string error;
    if (!storage.begin(error) || !artifacts.begin(storage.metadata(), error)) {
      std::cerr << "emulator: " << error << '\n';
      return 2;
    }

    while (!shutdownRequested) {
      std::string message;
      switch (stream.read(message, error)) {
        case FramedStream::ReadResult::EndOfStream:
          return initialized ? 0 : 2;
        case FramedStream::ReadResult::Error:
          std::cerr << "emulator: protocol framing error: " << error << '\n';
          return 2;
        case FramedStream::ReadResult::Message:
          break;
      }
      handle(message);
    }
    return 0;
  }

 private:
  void handle(const std::string& message) {
    JsonDocument request;
    const DeserializationError parseError = deserializeJson(request, message);
    if (parseError) {
      sendError(0, -32700, "parse error");
      return;
    }

    const JsonVariantConst idValue = request["id"];
    const uint64_t id = idValue.is<uint64_t>() ? idValue.as<uint64_t>() : 0;
    const char* jsonrpc = request["jsonrpc"] | "";
    const char* methodValue = request["method"] | "";
    if (std::string_view(jsonrpc) != "2.0" || id == 0 || methodValue[0] == '\0') {
      sendError(id, -32600, "invalid request");
      return;
    }

    const std::string_view method(methodValue);
    const JsonObjectConst params = request["params"].as<JsonObjectConst>();
    if (!initialized && method != "initialize") {
      sendError(id, -32002, "initialize must be the first request");
      return;
    }

    if (method == "initialize")
      handleInitialize(id, params);
    else if (method == "clock.now")
      handleClockNow(id);
    else if (method == "clock.advance")
      handleClockAdvance(id, params);
    else if (method == "emulator.state")
      handleState(id);
    else if (method == "input.press")
      handleInput(id, params, true);
    else if (method == "input.release")
      handleInput(id, params, false);
    else if (method == "input.pressAction")
      handleInputAction(id, params, true);
    else if (method == "input.releaseAction")
      handleInputAction(id, params, false);
    else if (method == "wait.activity")
      handleWaitActivity(id, params);
    else if (method == "wait.render")
      handleWaitRender(id, params);
    else if (method == "wait.panelIdle")
      handleWaitPanelIdle(id, params);
    else if (method == "storage.clearEpubCache")
      handleClearEpubCache(id, params);
    else if (method == "capture.panel")
      handleCapture(id, params, true, false);
    else if (method == "capture.framebuffer")
      handleCapture(id, params, false, false);
    else if (method == "capture.screenshot")
      handleCapture(id, params, true, true);
    else if (method == "input.reset")
      handleReset(id);
    else if (method == "shutdown")
      handleShutdown(id);
    else
      sendError(id, -32601, "method not found");
    flushPanelTransitions();
  }

  void handleInitialize(uint64_t id, const JsonObjectConst& params) {
    if (initialized) {
      sendError(id, -32002, "session is already initialized");
      return;
    }
    const uint32_t requestedVersion = params["protocolVersion"] | 0;
    if (requestedVersion != PROTOCOL_VERSION) {
      sendError(id, -32001, "unsupported protocol version");
      return;
    }

    initialized = true;
    JsonDocument fieldsDocument;
    JsonObject fields = fieldsDocument.to<JsonObject>();
    fields["deviceProfile"] = configuration.profile.id;
    fields["protocolVersion"] = PROTOCOL_VERSION;
    record("session.initialized", fields);

    JsonDocument resultDocument;
    JsonObject result = resultDocument.to<JsonObject>();
    result["protocolVersion"] = PROTOCOL_VERSION;
    result["traceVersion"] = TRACE_VERSION;
    result["panelModelVersion"] = PANEL_MODEL_VERSION;
    result["deviceProfile"] = configuration.profile.id;
    result["panelWidth"] = configuration.profile.panelWidth;
    result["panelHeight"] = configuration.profile.panelHeight;
    result["controller"] = configuration.profile.controller;
    result["reviewRotationDegrees"] = configuration.profile.reviewRotationDegrees;
    result["timingProfile"] = configuration.timing.id;
    result["timingCalibrated"] = configuration.timing.calibrated;
    result["schedulerModel"] = SCHEDULER_MODEL;
    result["artifactDirectory"] = std::filesystem::absolute(configuration.artifactDirectory).string();
    result["rtcStart"] = configuration.rtcStart;
    result["randomSeed"] = configuration.randomSeed;
    result["initialPanel"] = configuration.initialPanel;
    result["fixtureIdentity"] = storage.metadata().fixtureIdentity;
    result["storageDirectory"] = std::filesystem::absolute(storage.root()).string();
    sendResult(id, result);

    if (application.available()) {
      const auto runResult = application.start();
      JsonDocument applicationDocument;
      JsonObject applicationFields = applicationDocument.to<JsonObject>();
      applicationFields["state"] = application.state();
      applicationFields["schedulerDispatches"] = runResult.dispatches;
      record("application.started", applicationFields);
    }
  }

  void handleClockNow(uint64_t id) {
    JsonDocument resultDocument;
    JsonObject result = resultDocument.to<JsonObject>();
    result["simulatedTimeUs"] = clock.nowMicroseconds();
    sendResult(id, result);
  }

  void handleClockAdvance(uint64_t id, const JsonObjectConst& params) {
    if (!params["microseconds"].is<uint64_t>()) {
      sendError(id, -32602, "microseconds must be an unsigned integer");
      return;
    }
    const uint64_t amount = params["microseconds"].as<uint64_t>();
    DeterministicScheduler::RunResult runResult;
    try {
      runResult = application.available() ? application.advance(amount) : scheduler.advance(amount);
    } catch (const std::overflow_error&) {
      sendError(id, -32602, "simulated clock overflow");
      return;
    }

    JsonDocument fieldsDocument;
    JsonObject fields = fieldsDocument.to<JsonObject>();
    fields["microseconds"] = amount;
    fields["schedulerDispatches"] = runResult.dispatches;
    record("clock.advanced", fields);

    JsonDocument resultDocument;
    JsonObject result = resultDocument.to<JsonObject>();
    result["simulatedTimeUs"] = clock.nowMicroseconds();
    result["schedulerDispatches"] = runResult.dispatches;
    result["schedulerQuiescent"] = runResult.quiescent;
    sendResult(id, result);
  }

  void handleState(uint64_t id) {
    JsonDocument resultDocument;
    JsonObject result = resultDocument.to<JsonObject>();
    result["initialized"] = initialized;
    result["deviceProfile"] = configuration.profile.id;
    result["simulatedTimeUs"] = clock.nowMicroseconds();
    result["scheduler"] = SCHEDULER_MODEL;
    result["application"] = application.state();
    result["physicalControls"] = physicalControlMask();
    uint8_t debouncedControls = 0;
    for (uint8_t button = 0; button < 7; ++button) {
      if (gpio.isPressed(button)) debouncedControls |= static_cast<uint8_t>(1U << button);
    }
    result["debouncedControls"] = debouncedControls;
    result["heldTimeMs"] = gpio.getHeldTime();
    result["powerHeldTimeMs"] = gpio.getPowerButtonHeldTime();
    result["activityId"] = activityManager.getProtocolActivityId();
    result["renderGeneration"] = activityManager.getRenderGeneration();
    const PanelSnapshot panel = panelSnapshot();
    result["panelGeneration"] = panel.generation;
    result["panelBusy"] = panel.busy;
    result["panelOpticalBusy"] = panel.opticalBusy;
    JsonArray tasks = result["tasks"].to<JsonArray>();
    for (const auto& status : scheduler.taskStatuses()) {
      JsonObject task = tasks.add<JsonObject>();
      task["id"] = status.id;
      task["name"] = status.name;
      task["priority"] = status.priority;
      task["state"] = status.state;
      task["notifications"] = status.notifications;
      task["wakeTimeUs"] = status.wakeTimeUs;
    }
    sendResult(id, result);
  }

  void handleInput(uint64_t id, const JsonObjectConst& params, bool pressed) {
    const char* controlValue = params["control"] | "";
    const std::string_view control(controlValue);
    const int buttonIndex = physicalButtonIndex(control);
    if (buttonIndex < 0) {
      sendError(id, -32602, "control must be back, confirm, left, right, up, down, or power");
      return;
    }
    setPhysicalControl(static_cast<uint8_t>(buttonIndex), pressed);

    JsonDocument fieldsDocument;
    JsonObject fields = fieldsDocument.to<JsonObject>();
    fields["control"] = control;
    fields["pressed"] = pressed;
    fields["mask"] = physicalControlMask();
    record("input.physical", fields);
    sendResult(id, fields);
  }

  void handleInputAction(uint64_t id, const JsonObjectConst& params, bool pressed) {
    const char* actionValue = params["action"] | "";
    const std::string_view action(actionValue);
    const auto logical = protocolAction(action);
    if (!logical) {
      sendError(id, -32602, "action must be back, confirm, left, right, up, down, power, page_back, or page_forward");
      return;
    }
    const auto physical = mappedInputManager.resolvePhysicalButton(*logical);
    if (!physical) {
      sendError(id, -32011, "action is disabled by the current input mapping");
      return;
    }
    setPhysicalControl(*physical, pressed);

    JsonDocument fieldsDocument;
    JsonObject fields = fieldsDocument.to<JsonObject>();
    fields["action"] = action;
    fields["physicalControl"] = physicalButtonName(*physical);
    fields["pressed"] = pressed;
    fields["mask"] = physicalControlMask();
    record("input.action", fields);
    sendResult(id, fields);
  }

  enum class WaitOutcome { Ready, SimulatedTimeout, WallTimeout };

  template <typename Predicate>
  WaitOutcome waitFor(const JsonObjectConst& params, Predicate predicate, uint64_t& dispatches) {
    const uint64_t timeoutUs = params["timeoutUs"] | 10000000ULL;
    const uint64_t wallTimeoutMs = params["wallTimeoutMs"] | 5000ULL;
    const uint64_t start = clock.nowMicroseconds();
    const uint64_t deadline = timeoutUs > UINT64_MAX - start ? UINT64_MAX : start + timeoutUs;
    const auto wallDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wallTimeoutMs);
    while (!predicate()) {
      if (clock.nowMicroseconds() >= deadline) return WaitOutcome::SimulatedTimeout;
      if (std::chrono::steady_clock::now() >= wallDeadline) return WaitOutcome::WallTimeout;
      const uint64_t remaining = deadline - clock.nowMicroseconds();
      const uint64_t step = std::min<uint64_t>(10000, remaining);
      const auto runResult = application.available() ? application.advance(step) : scheduler.advance(step);
      dispatches += runResult.dispatches;
    }
    return WaitOutcome::Ready;
  }

  template <typename Predicate>
  bool completeWait(uint64_t id, const JsonObjectConst& params, Predicate predicate) {
    if ((!params["timeoutUs"].isNull() && !params["timeoutUs"].is<uint64_t>()) ||
        (!params["wallTimeoutMs"].isNull() && !params["wallTimeoutMs"].is<uint64_t>())) {
      sendError(id, -32602, "timeouts must be unsigned integers");
      return false;
    }
    uint64_t dispatches = 0;
    const WaitOutcome outcome = waitFor(params, predicate, dispatches);
    if (outcome == WaitOutcome::SimulatedTimeout) {
      sendError(id, -32020, "wait exceeded simulated-time timeout");
      return false;
    }
    if (outcome == WaitOutcome::WallTimeout) {
      sendError(id, -32021, "wait exceeded wall-time watchdog");
      return false;
    }
    JsonDocument resultDocument;
    JsonObject result = resultDocument.to<JsonObject>();
    result["simulatedTimeUs"] = clock.nowMicroseconds();
    result["schedulerDispatches"] = dispatches;
    result["activityId"] = activityManager.getProtocolActivityId();
    result["renderGeneration"] = activityManager.getRenderGeneration();
    const PanelSnapshot panel = panelSnapshot();
    result["panelGeneration"] = panel.generation;
    result["panelBusy"] = panel.busy;
    result["panelOpticalBusy"] = panel.opticalBusy;
    sendResult(id, result);
    return true;
  }

  void handleWaitActivity(uint64_t id, const JsonObjectConst& params) {
    const char* requested = params["activityId"] | "";
    const ProtocolActivityId expected = protocolActivityId(requested);
    if (expected == ProtocolActivityId::Unknown) {
      sendError(id, -32602,
                "activityId must be boot, home, file_browser, settings, sleep, reader.epub, reader.epub.menu, "
                "reader.epub.chapters, or reader.epub.percent");
      return;
    }
    completeWait(id, params, [this, expected] { return observedActivity.load() == expected; });
  }

  void handleWaitRender(uint64_t id, const JsonObjectConst& params) {
    if (!params["after"].is<uint64_t>()) {
      sendError(id, -32602, "after must be an unsigned render generation");
      return;
    }
    const uint64_t after = params["after"].as<uint64_t>();
    completeWait(id, params, [after] { return activityManager.getRenderGeneration() > after; });
  }

  bool panelIsIdle() const {
    const PanelSnapshot panel = panelSnapshot();
    if (panel.busy || panel.opticalBusy || activityManager.hasPendingRender()) return false;
    for (const auto& task : scheduler.taskStatuses()) {
      if (task.name == "ActivityManagerRender" && (task.state != "waiting-notification" || task.notifications != 0)) {
        return false;
      }
    }
    return true;
  }

  void handleWaitPanelIdle(uint64_t id, const JsonObjectConst& params) {
    completeWait(id, params, [this] { return panelIsIdle(); });
  }

  void handleClearEpubCache(uint64_t id, const JsonObjectConst& params) {
    if (observedActivity.load() != ProtocolActivityId::FileBrowser || !panelIsIdle()) {
      sendError(id, -32012, "EPUB cache can only be cleared from an idle File Browser");
      return;
    }
    const char* pathValue = params["path"] | "";
    const std::string path(pathValue);
    if (path.empty() || path.front() != '/' || path.find("..") != std::string::npos || !path.ends_with(".epub")) {
      sendError(id, -32602, "path must be an absolute .epub device path without '..'");
      return;
    }

    Epub epub(path, "/.crosspoint");
    std::string hostPathError;
    const auto hostCachePath = storage.hostPath(epub.getCachePath(), hostPathError);
    if (!hostCachePath) {
      sendError(id, -32013, hostPathError.c_str());
      return;
    }
    const bool existed = storage.exists(epub.getCachePath());
    std::error_code filesystemError;
    uint64_t fileCount = 0;
    if (existed) {
      if (!std::filesystem::is_directory(*hostCachePath, filesystemError) || filesystemError) {
        sendError(id, -32013, "EPUB cache is not a readable directory");
        return;
      }
      std::filesystem::recursive_directory_iterator iterator(*hostCachePath, filesystemError);
      const std::filesystem::recursive_directory_iterator end;
      while (!filesystemError && iterator != end) {
        if (iterator->is_regular_file(filesystemError)) {
          if (fileCount == std::numeric_limits<uint64_t>::max()) {
            sendError(id, -32013, "EPUB cache contains too many files");
            return;
          }
          ++fileCount;
        }
        iterator.increment(filesystemError);
      }
      if (filesystemError) {
        sendError(id, -32013, "failed to enumerate EPUB cache");
        return;
      }
    }
    const auto& workload = configuration.timing.workload;
    uint64_t targetDurationUs = configuration.timing.calibrated ? workload.cacheClearNoCacheUs : 0;
    if (configuration.timing.calibrated && existed) {
      if (fileCount >
          (std::numeric_limits<uint64_t>::max() - workload.cacheClearInterceptUs) / workload.cacheClearPerFileUs) {
        sendError(id, -32013, "EPUB cache-clear timing overflow");
        return;
      }
      targetDurationUs = workload.cacheClearInterceptUs + fileCount * workload.cacheClearPerFileUs;
    }

    bool cleared = false;
    const uint64_t startedUs = clock.nowMicroseconds();
    scheduler.createTask("emulator-cache-clear", [&] { cleared = epub.clearCache(); }, UINT32_MAX);
    scheduler.runReady(1);
    if (!cleared) {
      sendError(id, -32013, "failed to clear EPUB cache");
      return;
    }

    uint64_t durationUs = clock.nowMicroseconds() - startedUs;
    if (durationUs < targetDurationUs) {
      if (!clock.advance(targetDurationUs - durationUs)) {
        sendError(id, -32013, "EPUB cache-clear time overflow");
        return;
      }
      durationUs = targetDurationUs;
    }

    JsonDocument fieldsDocument;
    JsonObject fields = fieldsDocument.to<JsonObject>();
    fields["path"] = path;
    fields["existed"] = existed;
    fields["fileCount"] = fileCount;
    fields["targetDurationUs"] = targetDurationUs;
    fields["durationUs"] = durationUs;
    record("storage.epubCacheCleared", fields);
    sendResult(id, fields);
  }

  void handleReset(uint64_t id) {
    const PanelSnapshot panel = panelSnapshot();
    const auto panelPath = configuration.artifactDirectory / "captures" / "reset-panel.png";
    std::string error;
    if (!writeGrayscalePng(panelPath, panel.width, panel.height, panel.pixels, error)) {
      sendError(id, -32010, error.c_str());
      return;
    }

    JsonDocument fieldsDocument;
    JsonObject fields = fieldsDocument.to<JsonObject>();
    fields["panelPath"] = std::filesystem::absolute(panelPath).string();
    fields["storageDirectory"] = std::filesystem::absolute(storage.root()).string();
    fields["panelGeneration"] = panel.generation;
    record("input.reset", fields);

    JsonDocument resultDocument;
    JsonObject result = resultDocument.to<JsonObject>();
    result["accepted"] = true;
    result["relaunchRequired"] = true;
    result["panelPath"] = std::filesystem::absolute(panelPath).string();
    result["storageDirectory"] = std::filesystem::absolute(storage.root()).string();
    result["panelGeneration"] = panel.generation;
    sendResult(id, result);
    shutdownRequested = true;
  }

  void handleCapture(uint64_t id, const JsonObjectConst& params, bool panel, bool reviewOrientation) {
    const char* defaultName = reviewOrientation ? "screenshot.png" : (panel ? "panel.png" : "framebuffer.png");
    const char* requestedName = params["name"] | defaultName;
    const std::string name(requestedName);
    if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
        name.find("..") != std::string::npos || !name.ends_with(".png")) {
      sendError(id, -32602, "capture name must be a simple .png filename");
      return;
    }

    uint16_t width = 0;
    uint16_t height = 0;
    uint64_t generation = 0;
    std::vector<uint8_t> pixels;
    if (panel) {
      PanelSnapshot snapshot = panelSnapshot();
      width = snapshot.width;
      height = snapshot.height;
      generation = snapshot.generation;
      pixels = std::move(snapshot.pixels);
    } else {
      width = display.getDisplayWidth();
      height = display.getDisplayHeight();
      const uint16_t widthBytes = display.getDisplayWidthBytes();
      const uint8_t* frameBuffer = display.getFrameBuffer();
      if (frameBuffer != nullptr) {
        pixels.resize(static_cast<size_t>(width) * height);
        for (uint16_t y = 0; y < height; ++y) {
          for (uint16_t x = 0; x < width; ++x) {
            pixels[static_cast<size_t>(y) * width + x] =
                (frameBuffer[static_cast<size_t>(y) * widthBytes + x / 8] & (0x80U >> (x % 8))) ? 0xFF : 0x00;
          }
        }
      }
    }

    if (reviewOrientation) {
      pixels = rotateGrayscaleClockwise(width, height, pixels);
      std::swap(width, height);
    }

    const auto path = configuration.artifactDirectory / (reviewOrientation ? "presentation" : "captures") / name;
    std::string error;
    if (!writeGrayscalePng(path, width, height, pixels, error)) {
      sendError(id, -32010, error.c_str());
      return;
    }

    JsonDocument fieldsDocument;
    JsonObject fields = fieldsDocument.to<JsonObject>();
    fields["kind"] = reviewOrientation ? "screenshot" : (panel ? "panel" : "framebuffer");
    fields["path"] = std::filesystem::absolute(path).string();
    fields["generation"] = generation;
    fields["rotationDegrees"] = reviewOrientation ? configuration.profile.reviewRotationDegrees : 0;
    record("capture.written", fields);

    JsonDocument resultDocument;
    JsonObject result = resultDocument.to<JsonObject>();
    result["path"] = std::filesystem::absolute(path).string();
    result["width"] = width;
    result["height"] = height;
    result["generation"] = generation;
    result["rotationDegrees"] = reviewOrientation ? configuration.profile.reviewRotationDegrees : 0;
    sendResult(id, result);
  }

  void handleShutdown(uint64_t id) {
    JsonDocument fieldsDocument;
    JsonObject fields = fieldsDocument.to<JsonObject>();
    record("session.shutdown", fields);

    JsonDocument resultDocument;
    JsonObject result = resultDocument.to<JsonObject>();
    result["accepted"] = true;
    sendResult(id, result);
    shutdownRequested = true;
  }

  struct StoredPanelFrame {
    uint64_t hash;
    std::vector<uint8_t> pixels;
    std::string relativePath;
  };

  void flushPanelTransitions() {
    if (framePersistenceFailed) return;
    const auto& transitions = panelTransitions();
    const auto& profile = configuration.profile;
    while (nextPanelTransition < transitions.size()) {
      const PanelTransition& transition = transitions[nextPanelTransition++];
      const uint64_t hash = panelFrameHash(profile.panelWidth, profile.panelHeight, transition.pixels);
      const StoredPanelFrame* stored = nullptr;
      for (const auto& candidate : storedPanelFrames) {
        if (candidate.hash == hash && candidate.pixels == transition.pixels) {
          stored = &candidate;
          break;
        }
      }
      if (stored == nullptr) {
        const std::string relativePath = "frames/" + panelFrameName(hash);
        std::string error;
        if (!writeGrayscalePng(configuration.artifactDirectory / relativePath, profile.panelWidth, profile.panelHeight,
                               transition.pixels, error)) {
          std::cerr << "emulator: failed to persist panel transition: " << error << '\n';
          framePersistenceFailed = true;
          return;
        }
        storedPanelFrames.push_back({hash, transition.pixels, relativePath});
        stored = &storedPanelFrames.back();
      }

      JsonDocument fieldsDocument;
      JsonObject fields = fieldsDocument.to<JsonObject>();
      fields["phase"] = transition.phase;
      fields["generation"] = transition.generation;
      fields["path"] = stored->relativePath;
      std::ostringstream identity;
      identity << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
      fields["identity"] = identity.str();
      artifacts.record("panel.frame", ++sequence, transition.simulatedTimeUs, fields);
    }
  }

  void sendResult(uint64_t id, const JsonObjectConst& result) {
    JsonDocument response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["sequence"] = ++sequence;
    response["result"] = result;
    write(response);
  }

  void sendError(uint64_t id, int code, const char* message) {
    JsonDocument response;
    response["jsonrpc"] = "2.0";
    if (id == 0)
      response["id"] = nullptr;
    else
      response["id"] = id;
    response["sequence"] = ++sequence;
    JsonObject error = response["error"].to<JsonObject>();
    error["code"] = code;
    error["message"] = message;
    write(response);
  }

  void write(const JsonDocument& document) {
    std::string json;
    serializeJson(document, json);
    if (!stream.write(json)) std::cerr << "emulator: failed to write protocol response\n";
  }

  void record(std::string_view type, const JsonObjectConst& fields) {
    artifacts.record(type, ++sequence, clock.nowMicroseconds(), fields);
  }

  Configuration configuration;
  FramedStream& stream;
  RunArtifacts artifacts;
  DirectoryStorage storage;
  SimulatedClock clock;
  DeterministicScheduler scheduler;
  FreeRtosRuntime runtime;
  ApplicationLifecycle application;
  uint64_t sequence = 0;
  size_t nextPanelTransition = 0;
  std::vector<StoredPanelFrame> storedPanelFrames;
  bool framePersistenceFailed = false;
  std::atomic<ProtocolActivityId> observedActivity{ProtocolActivityId::Unknown};
  bool initialized = false;
  bool shutdownRequested = false;
};

}  // namespace

int run(int argc, char** argv) {
  std::string error;
  auto configuration = parseConfiguration(argc, argv, error);
  if (!configuration) {
    std::cerr << "usage: crosspoint-emulator --device x3|x4 --artifacts DIR [--sd FIXTURE] "
                 "[--rtc-start ISO8601] [--seed N] [--panel-initial white|black] [--panel-initial-png PNG] "
                 "[--timing-profile JSON]\n";
    std::cerr << "emulator: " << error << '\n';
    return 2;
  }

  std::locale::global(std::locale::classic());
  setenv("TZ", "UTC", 1);
  tzset();
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  FramedStream stream(std::cin, std::cout);
  Session session(std::move(*configuration), stream);
  return session.serve();
}

}  // namespace emulator
