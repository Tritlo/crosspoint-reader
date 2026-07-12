#include <EmulatorNative.h>

#include <ArduinoJson.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "emulator/Configuration.h"
#include "emulator/ApplicationLifecycle.h"
#include "emulator/DeterministicScheduler.h"
#include "emulator/FramedStream.h"
#include "emulator/FreeRtosCompat.h"
#include "emulator/RunArtifacts.h"
#include "emulator/SimulatedClock.h"

void setup() __attribute__((weak));
void loop() __attribute__((weak));

namespace emulator {
namespace {

constexpr uint32_t PROTOCOL_VERSION = 1;
constexpr uint32_t TRACE_VERSION = 1;
constexpr uint32_t PANEL_MODEL_VERSION = 0;

class Session {
 public:
  Session(Configuration configuration, FramedStream& stream)
      : configuration(std::move(configuration)),
        stream(stream),
        artifacts(this->configuration),
        scheduler(clock),
        runtime(scheduler, clock),
        application(scheduler, setup, loop) {}

  int serve() {
    std::string error;
    if (!artifacts.begin(error)) {
      std::cerr << "emulator: " << error << '\n';
      return 2;
    }

    while (!shutdownRequested) {
      std::string message;
      switch (stream.read(message, error)) {
        case FramedStream::ReadResult::EndOfStream: return initialized ? 0 : 2;
        case FramedStream::ReadResult::Error:
          std::cerr << "emulator: protocol framing error: " << error << '\n';
          return 2;
        case FramedStream::ReadResult::Message: break;
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

    if (method == "initialize") handleInitialize(id, params);
    else if (method == "clock.now") handleClockNow(id);
    else if (method == "clock.advance") handleClockAdvance(id, params);
    else if (method == "emulator.state") handleState(id);
    else if (method == "shutdown") handleShutdown(id);
    else sendError(id, -32601, "method not found");
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
    result["timingProfile"] = "development-uncalibrated-v0";
    result["schedulerModel"] = "deterministic-single-core-v1";
    result["artifactDirectory"] = std::filesystem::absolute(configuration.artifactDirectory).string();
    result["rtcStart"] = configuration.rtcStart;
    result["randomSeed"] = configuration.randomSeed;
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
    result["scheduler"] = "deterministic-single-core-v1";
    result["application"] = application.state();
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
    if (id == 0) response["id"] = nullptr;
    else response["id"] = id;
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
  SimulatedClock clock;
  DeterministicScheduler scheduler;
  FreeRtosRuntime runtime;
  ApplicationLifecycle application;
  uint64_t sequence = 0;
  bool initialized = false;
  bool shutdownRequested = false;
};

}  // namespace

int run(int argc, char** argv) {
  std::string error;
  auto configuration = parseConfiguration(argc, argv, error);
  if (!configuration) {
    std::cerr << "usage: crosspoint-emulator --device x3|x4 --artifacts DIR [--rtc-start ISO8601] [--seed N]\n";
    std::cerr << "emulator: " << error << '\n';
    return 2;
  }

  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  FramedStream stream(std::cin, std::cout);
  Session session(std::move(*configuration), stream);
  return session.serve();
}

}  // namespace emulator
