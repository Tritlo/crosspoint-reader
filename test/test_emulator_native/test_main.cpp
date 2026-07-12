#include <Arduino.h>
#include <DevicePathHash.h>
#include <EInkDisplay.h>
#include <Epub.h>
#include <HalStorage.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unity.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "activities/reader/ProgressFile.h"
#include "emulator/ApplicationLifecycle.h"
#include "emulator/DeterministicScheduler.h"
#include "emulator/DirectoryStorage.h"
#include "emulator/FreeRtosCompat.h"
#include "emulator/PanelModel.h"
#include "emulator/SimulatedClock.h"

using emulator::ApplicationLifecycle;
using emulator::DeterministicScheduler;
using emulator::Device;
using emulator::DirectoryStorage;
using emulator::FreeRtosRuntime;
using emulator::SimulatedClock;

void setUp() {}
void tearDown() {}

namespace {
int lifecycleSetupCalls = 0;
int lifecycleLoopCalls = 0;

void lifecycleSetup() {
  ++lifecycleSetupCalls;
  delay(5);
}

void lifecycleLoop() {
  ++lifecycleLoopCalls;
  delay(10);
}
}  // namespace

void test_scheduler_serializes_tasks_and_advances_time() {
  SimulatedClock clock;
  DeterministicScheduler scheduler(clock, std::chrono::milliseconds(200));
  std::vector<std::string> order;

  DeterministicScheduler::TaskId first = 0;
  first = scheduler.createTask("first", [&] {
    order.emplace_back("first.1");
    scheduler.yield();
    order.emplace_back("first.2");
    scheduler.takeNotification(true);
    order.emplace_back("first.3");
  });
  scheduler.createTask("second", [&] {
    order.emplace_back("second.1");
    scheduler.delay(10);
    order.emplace_back("second.2");
    scheduler.notify(first);
  });

  const auto result = scheduler.run();
  const std::vector<std::string> expected{"first.1", "first.2", "second.1", "second.2", "first.3"};
  TEST_ASSERT_TRUE(result.quiescent);
  TEST_ASSERT_EQUAL_UINT64(5, result.dispatches);
  TEST_ASSERT_EQUAL_UINT64(10, clock.nowMicroseconds());
  TEST_ASSERT_EQUAL(expected.size(), order.size());
  for (size_t index = 0; index < expected.size(); ++index) {
    TEST_ASSERT_EQUAL_STRING(expected[index].c_str(), order[index].c_str());
  }
}

void test_scheduler_prefers_priority_then_creation_order() {
  SimulatedClock clock;
  DeterministicScheduler scheduler(clock, std::chrono::milliseconds(200));
  std::vector<std::string> order;
  scheduler.createTask("low", [&] { order.emplace_back("low"); }, 1);
  scheduler.createTask("high-first", [&] { order.emplace_back("high-first"); }, 2);
  scheduler.createTask("high-second", [&] { order.emplace_back("high-second"); }, 2);

  scheduler.run();
  const std::vector<std::string> expected{"high-first", "high-second", "low"};
  TEST_ASSERT_EQUAL(expected.size(), order.size());
  for (size_t index = 0; index < expected.size(); ++index) {
    TEST_ASSERT_EQUAL_STRING(expected[index].c_str(), order[index].c_str());
  }
}

void test_notification_timeout_uses_simulated_time() {
  SimulatedClock clock;
  DeterministicScheduler scheduler(clock, std::chrono::milliseconds(200));
  uint32_t notification = 1;
  scheduler.createTask("waiter", [&] { notification = scheduler.takeNotification(true, 25); });

  const auto result = scheduler.run();
  TEST_ASSERT_TRUE(result.quiescent);
  TEST_ASSERT_EQUAL_UINT32(0, notification);
  TEST_ASSERT_EQUAL_UINT64(25, clock.nowMicroseconds());
}

void test_run_ready_stops_at_requested_simulated_time() {
  SimulatedClock clock;
  DeterministicScheduler scheduler(clock, std::chrono::milliseconds(200));
  bool finished = false;
  scheduler.createTask("delayed", [&] {
    scheduler.delay(10);
    finished = true;
  });

  TEST_ASSERT_TRUE(scheduler.runReady().quiescent);
  TEST_ASSERT_FALSE(finished);
  TEST_ASSERT_EQUAL_UINT64(0, clock.nowMicroseconds());
  TEST_ASSERT_TRUE(scheduler.advance(9).quiescent);
  TEST_ASSERT_FALSE(finished);
  TEST_ASSERT_EQUAL_UINT64(9, clock.nowMicroseconds());
  TEST_ASSERT_TRUE(scheduler.advance(1).quiescent);
  TEST_ASSERT_TRUE(finished);
  TEST_ASSERT_EQUAL_UINT64(10, clock.nowMicroseconds());
}

void test_application_lifecycle_runs_setup_then_bounded_loops() {
  SimulatedClock clock;
  DeterministicScheduler scheduler(clock, std::chrono::milliseconds(200));
  FreeRtosRuntime runtime(scheduler, clock);
  lifecycleSetupCalls = 0;
  lifecycleLoopCalls = 0;
  ApplicationLifecycle lifecycle(scheduler, lifecycleSetup, lifecycleLoop);

  const auto startResult = lifecycle.start();
  TEST_ASSERT_TRUE(startResult.quiescent);
  TEST_ASSERT_EQUAL_STRING("setup-complete", lifecycle.state().data());
  TEST_ASSERT_EQUAL_INT(1, lifecycleSetupCalls);
  TEST_ASSERT_EQUAL_INT(0, lifecycleLoopCalls);
  TEST_ASSERT_EQUAL_UINT64(5000, clock.nowMicroseconds());

  const auto advanceResult = lifecycle.advance(25000);
  TEST_ASSERT_TRUE(advanceResult.quiescent);
  TEST_ASSERT_EQUAL_STRING("running", lifecycle.state().data());
  TEST_ASSERT_EQUAL_INT(3, lifecycleLoopCalls);
  TEST_ASSERT_EQUAL_UINT64(30000, clock.nowMicroseconds());
}

void test_freertos_compat_serializes_mutex_and_notifications() {
  struct Context {
    SemaphoreHandle_t mutex;
    std::vector<std::string>* order;
    TaskHandle_t first;
  };

  SimulatedClock clock;
  DeterministicScheduler scheduler(clock, std::chrono::milliseconds(200));
  FreeRtosRuntime runtime(scheduler, clock);
  std::vector<std::string> order;
  Context context{xSemaphoreCreateMutex(), &order, nullptr};
  TaskHandle_t second = nullptr;

  TEST_ASSERT_EQUAL(pdPASS, xTaskCreatePinnedToCore(
                                [](void* parameter) {
                                  auto& state = *static_cast<Context*>(parameter);
                                  TEST_ASSERT_EQUAL(pdPASS, xSemaphoreTake(state.mutex, portMAX_DELAY));
                                  state.order->emplace_back("first.locked");
                                  vTaskDelay(2);
                                  state.order->emplace_back("first.unlock");
                                  TEST_ASSERT_EQUAL(pdPASS, xSemaphoreGive(state.mutex));
                                  TEST_ASSERT_EQUAL_UINT32(1, ulTaskNotifyTake(pdTRUE, portMAX_DELAY));
                                },
                                "first", 1024, &context, 1, &context.first, 0));
  TEST_ASSERT_EQUAL(pdPASS, xTaskCreatePinnedToCore(
                                [](void* parameter) {
                                  auto& state = *static_cast<Context*>(parameter);
                                  state.order->emplace_back("second.wait");
                                  TEST_ASSERT_EQUAL(pdPASS, xSemaphoreTake(state.mutex, portMAX_DELAY));
                                  state.order->emplace_back("second.locked");
                                  TEST_ASSERT_EQUAL(pdPASS, xTaskNotify(state.first, 1, eIncrement));
                                  TEST_ASSERT_EQUAL(pdPASS, xSemaphoreGive(state.mutex));
                                },
                                "second", 1024, &context, 1, &second, 0));

  const auto result = scheduler.run();
  const std::vector<std::string> expected{"first.locked", "second.wait", "first.unlock", "second.locked"};
  TEST_ASSERT_TRUE(result.quiescent);
  TEST_ASSERT_EQUAL_UINT64(2000, clock.nowMicroseconds());
  TEST_ASSERT_EQUAL(expected.size(), order.size());
  for (size_t index = 0; index < expected.size(); ++index) {
    TEST_ASSERT_EQUAL_STRING(expected[index].c_str(), order[index].c_str());
  }
}

void test_wall_time_watchdog_terminates_stalled_process() {
  const pid_t child = fork();
  TEST_ASSERT_NOT_EQUAL(-1, child);
  if (child == 0) {
    SimulatedClock clock;
    DeterministicScheduler scheduler(clock, std::chrono::milliseconds(20));
    scheduler.createTask("stalled", [] {
      while (true) {
      }
    });
    scheduler.run();
    std::_Exit(1);
  }
  TEST_ASSERT_GREATER_THAN_INT(0, child);

  int status = 0;
  TEST_ASSERT_EQUAL_INT(child, waitpid(child, &status, 0));
  TEST_ASSERT_TRUE(WIFEXITED(status));
  TEST_ASSERT_EQUAL_INT(70, WEXITSTATUS(status));
}

void test_directory_storage_copies_fixture_and_sorts_entries() {
  const auto base = std::filesystem::temp_directory_path() / ("crosspoint-storage-test-" + std::to_string(getpid()));
  const auto fixture = base / "fixture";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(fixture / "nested");
  std::ofstream(fixture / "z.txt", std::ios::binary) << "zulu";
  std::ofstream(fixture / "a.txt", std::ios::binary) << "alpha";
  std::ofstream(fixture / "nested" / "inside.txt", std::ios::binary) << "inside";

  emulator::Configuration firstConfiguration{emulator::profileFor(Device::X3), base / "run-1", fixture};
  DirectoryStorage first(firstConfiguration);
  std::string error;
  TEST_ASSERT_TRUE_MESSAGE(first.begin(error), error.c_str());
  const auto files = first.listFiles("/", 100, error);
  TEST_ASSERT_TRUE_MESSAGE(error.empty(), error.c_str());
  TEST_ASSERT_EQUAL_UINT64(2, files.size());
  TEST_ASSERT_EQUAL_STRING("a.txt", files[0].c_str());
  TEST_ASSERT_EQUAL_STRING("z.txt", files[1].c_str());
  TEST_ASSERT_EQUAL_UINT64(3, first.metadata().fixtureFileCount);
  TEST_ASSERT_EQUAL_UINT64(1, first.metadata().fixtureDirectoryCount);
  TEST_ASSERT_EQUAL_UINT64(15, first.metadata().fixtureBytes);

  std::vector<uint8_t> contents;
  TEST_ASSERT_TRUE_MESSAGE(first.readFile("/a.txt", contents, error), error.c_str());
  TEST_ASSERT_EQUAL_STRING_LEN("alpha", reinterpret_cast<const char*>(contents.data()), contents.size());
  const std::string changed = "changed";
  TEST_ASSERT_TRUE_MESSAGE(
      first.writeFile("/a.txt", reinterpret_cast<const uint8_t*>(changed.data()), changed.size(), error),
      error.c_str());
  std::ifstream fixtureInput(fixture / "a.txt", std::ios::binary);
  const std::string fixtureContents{std::istreambuf_iterator<char>(fixtureInput), std::istreambuf_iterator<char>()};
  TEST_ASSERT_EQUAL_STRING("alpha", fixtureContents.c_str());

  error.clear();
  TEST_ASSERT_FALSE(first.hostPath("/../escape", error).has_value());
  TEST_ASSERT_FALSE(error.empty());

  emulator::Configuration secondConfiguration{emulator::profileFor(Device::X4), base / "run-2", fixture};
  DirectoryStorage second(secondConfiguration);
  error.clear();
  TEST_ASSERT_TRUE_MESSAGE(second.begin(error), error.c_str());
  TEST_ASSERT_EQUAL_STRING(first.metadata().fixtureIdentity.c_str(), second.metadata().fixtureIdentity.c_str());
  contents.clear();
  TEST_ASSERT_TRUE_MESSAGE(second.readFile("/a.txt", contents, error), error.c_str());
  TEST_ASSERT_EQUAL_STRING_LEN("alpha", reinterpret_cast<const char*>(contents.data()), contents.size());
  std::filesystem::remove_all(base);
}

void test_device_path_hash_matches_32_bit_libstdcpp() {
  TEST_ASSERT_EQUAL_UINT32(3990065800U, device_path::hash(""));
  TEST_ASSERT_EQUAL_UINT32(2160968457U, device_path::hash("/test.epub"));
  TEST_ASSERT_EQUAL_UINT32(2678585205U, device_path::hash("/books/Stormlight Archive.epub"));
  TEST_ASSERT_EQUAL_UINT32(3276079349U, device_path::hash("/read/dune.txt"));
}

void test_native_hal_storage_uses_isolated_run_copy() {
  struct Results {
    bool began = false;
    bool listed = false;
    bool readText = false;
    bool openedBinary = false;
    bool wroteBinary = false;
    bool readBinary = false;
    bool enumerated = false;
    bool resetPreserved = false;
  } results;

  const auto base =
      std::filesystem::temp_directory_path() / ("crosspoint-hal-storage-test-" + std::to_string(getpid()));
  const auto fixture = base / "fixture";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(fixture);
  std::ofstream(fixture / "book.txt", std::ios::binary) << "fixture-book";
  emulator::Configuration configuration{emulator::profileFor(Device::X3), base / "run", fixture};
  DirectoryStorage storage(configuration);
  std::string error;
  TEST_ASSERT_TRUE_MESSAGE(storage.begin(error), error.c_str());

  {
    SimulatedClock clock;
    DeterministicScheduler scheduler(clock, std::chrono::milliseconds(200));
    FreeRtosRuntime runtime(scheduler, clock, &storage);
    scheduler.createTask("hal-storage", [&] {
      results.began = Storage.begin();
      const auto files = Storage.listFiles("/");
      results.listed = files.size() == 1 && files[0] == "book.txt";
      results.readText = Storage.readFile("/book.txt") == "fixture-book";
      if (!Storage.ensureDirectoryExists("/.crosspoint")) return;

      HalFile output;
      const uint32_t value = 0x12345678;
      results.openedBinary = Storage.openFileForWrite("TEST", "/.crosspoint/value.bin", output);
      if (!results.openedBinary) return;
      results.wroteBinary = output.write(&value, sizeof(value)) == sizeof(value) && output.close();

      HalFile input;
      uint32_t loaded = 0;
      if (!Storage.openFileForRead("TEST", "/.crosspoint/value.bin", input)) return;
      results.readBinary = input.read(&loaded, sizeof(loaded)) == sizeof(loaded) && loaded == value;

      auto root = Storage.open("/");
      std::vector<std::string> names;
      for (auto child = root.openNextFile(); child; child = root.openNextFile()) {
        char name[64];
        child.getName(name, sizeof(name));
        names.emplace_back(name);
      }
      results.enumerated = names == std::vector<std::string>({".crosspoint", "book.txt"});
    });
    scheduler.run();
  }

  {
    SimulatedClock clock;
    DeterministicScheduler scheduler(clock, std::chrono::milliseconds(200));
    FreeRtosRuntime runtime(scheduler, clock, &storage);
    scheduler.createTask("after-reset", [&] {
      HalFile input;
      uint32_t loaded = 0;
      results.resetPreserved = Storage.openFileForRead("TEST", "/.crosspoint/value.bin", input) &&
                               input.read(&loaded, sizeof(loaded)) == sizeof(loaded) && loaded == 0x12345678;
    });
    scheduler.run();
  }

  TEST_ASSERT_TRUE(results.began);
  TEST_ASSERT_TRUE(results.listed);
  TEST_ASSERT_TRUE(results.readText);
  TEST_ASSERT_TRUE(results.openedBinary);
  TEST_ASSERT_TRUE(results.wroteBinary);
  TEST_ASSERT_TRUE(results.readBinary);
  TEST_ASSERT_TRUE(results.enumerated);
  TEST_ASSERT_TRUE(results.resetPreserved);
  std::ifstream fixtureInput(fixture / "book.txt", std::ios::binary);
  const std::string fixtureContents{std::istreambuf_iterator<char>(fixtureInput), std::istreambuf_iterator<char>()};
  TEST_ASSERT_EQUAL_STRING("fixture-book", fixtureContents.c_str());
  TEST_ASSERT_FALSE(std::filesystem::exists(fixture / ".crosspoint"));
  std::filesystem::remove_all(base);
}

void test_epub_cache_and_progress_round_trip_through_shared_readers() {
  struct Results {
    bool firstLoad = false;
    bool cacheCreated = false;
    bool secondLoad = false;
    bool metadataMatches = false;
    bool progressRoundTrip = false;
  } results;

  const auto base = std::filesystem::temp_directory_path() / ("crosspoint-epub-cache-test-" + std::to_string(getpid()));
  const auto fixture = base / "fixture";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(fixture);
  std::filesystem::copy_file("test/epubs/test_text_decorations.epub", fixture / "book.epub");
  emulator::Configuration configuration{emulator::profileFor(Device::X3), base / "run", fixture};
  DirectoryStorage storage(configuration);
  std::string error;
  TEST_ASSERT_TRUE_MESSAGE(storage.begin(error), error.c_str());

  SimulatedClock clock;
  DeterministicScheduler scheduler(clock, std::chrono::milliseconds(1000));
  FreeRtosRuntime runtime(scheduler, clock, &storage);
  scheduler.createTask("epub-cache-round-trip", [&] {
    if (!Storage.begin()) return;
    Epub first("/book.epub", "/.crosspoint");
    results.firstLoad = first.load(true, true);
    if (!results.firstLoad) return;
    results.cacheCreated = Storage.exists((first.getCachePath() + "/book.bin").c_str());
    const std::string title = first.getTitle();
    const int spineCount = first.getSpineItemsCount();

    Epub second("/book.epub", "/.crosspoint");
    results.secondLoad = second.load(false, true);
    results.metadataMatches =
        results.secondLoad && second.getTitle() == title && second.getSpineItemsCount() == spineCount;

    constexpr uint8_t progress[] = {0x50, 0x52, 0x47, 0x01, 0x34, 0x12, 0x00, 0x00};
    if (!ProgressFile::writeAtomic(second.getCachePath(), progress, sizeof(progress))) return;
    HalFile progressFile;
    uint8_t loaded[sizeof(progress)]{};
    results.progressRoundTrip =
        Storage.openFileForRead("TEST", second.getCachePath() + "/progress.bin", progressFile) &&
        progressFile.read(loaded, sizeof(loaded)) == sizeof(loaded) &&
        std::memcmp(progress, loaded, sizeof(progress)) == 0;
  });
  scheduler.run();

  TEST_ASSERT_TRUE(results.firstLoad);
  TEST_ASSERT_TRUE(results.cacheCreated);
  TEST_ASSERT_TRUE(results.secondLoad);
  TEST_ASSERT_TRUE(results.metadataMatches);
  TEST_ASSERT_TRUE(results.progressRoundTrip);
  TEST_ASSERT_FALSE(std::filesystem::exists(fixture / ".crosspoint"));
  std::filesystem::remove_all(base);
}

void test_real_x3_x4_drivers_produce_distinct_retained_panel_state() {
  struct Result {
    emulator::PanelSnapshot painted;
    emulator::PanelSnapshot slept;
    emulator::PanelSnapshot reset;
    std::vector<std::string> commands;
  } x3, x4;

  const auto base =
      std::filesystem::temp_directory_path() / ("crosspoint-panel-driver-test-" + std::to_string(getpid()));
  std::filesystem::remove_all(base);

  const auto exercise = [&](Device device, Result& result) {
    emulator::Configuration configuration{emulator::profileFor(device), base / (device == Device::X3 ? "x3" : "x4")};
    DirectoryStorage storage(configuration);
    std::string error;
    TEST_ASSERT_TRUE_MESSAGE(storage.begin(error), error.c_str());
    SimulatedClock clock;
    DeterministicScheduler scheduler(clock, std::chrono::milliseconds(1000));
    FreeRtosRuntime runtime(scheduler, clock, &storage, {},
                            [&](std::string_view event, std::string_view detail, uint64_t) {
                              if (event == "command") result.commands.emplace_back(detail);
                            });
    EInkDisplay panel(8, 10, 21, 4, 5, 6);
    scheduler.createTask("panel-driver", [&] {
      if (device == Device::X3)
        panel.setDisplayX3();
      else
        BoardConfig::selectDevice(BoardConfig::Board::XteinkX4);
      panel.begin();
      panel.clearScreen(0x00);
      panel.displayBuffer(EInkDisplay::FULL_REFRESH);
      result.painted = emulator::panelSnapshot();
      panel.deepSleep();
      result.slept = emulator::panelSnapshot();
      panel.begin();
      result.reset = emulator::panelSnapshot();
    });
    scheduler.run();
  };

  exercise(Device::X3, x3);
  exercise(Device::X4, x4);

  TEST_ASSERT_EQUAL_UINT16(792, x3.painted.width);
  TEST_ASSERT_EQUAL_UINT16(528, x3.painted.height);
  TEST_ASSERT_EQUAL_STRING("uc8253", x3.painted.controller.c_str());
  TEST_ASSERT_EQUAL_UINT16(800, x4.painted.width);
  TEST_ASSERT_EQUAL_UINT16(480, x4.painted.height);
  TEST_ASSERT_EQUAL_STRING("ssd1677", x4.painted.controller.c_str());
  TEST_ASSERT_TRUE(
      std::all_of(x3.painted.pixels.begin(), x3.painted.pixels.end(), [](uint8_t value) { return value == 0; }));
  TEST_ASSERT_TRUE(
      std::all_of(x4.painted.pixels.begin(), x4.painted.pixels.end(), [](uint8_t value) { return value == 0; }));
  TEST_ASSERT_EQUAL_MEMORY(x3.painted.pixels.data(), x3.slept.pixels.data(), x3.painted.pixels.size());
  TEST_ASSERT_EQUAL_MEMORY(x3.painted.pixels.data(), x3.reset.pixels.data(), x3.painted.pixels.size());
  TEST_ASSERT_EQUAL_MEMORY(x4.painted.pixels.data(), x4.slept.pixels.data(), x4.painted.pixels.size());
  TEST_ASSERT_EQUAL_MEMORY(x4.painted.pixels.data(), x4.reset.pixels.data(), x4.painted.pixels.size());
  TEST_ASSERT_TRUE(std::find(x3.commands.begin(), x3.commands.end(), "uc8253:0x13") != x3.commands.end());
  TEST_ASSERT_TRUE(std::find(x4.commands.begin(), x4.commands.end(), "ssd1677:0x24") != x4.commands.end());
  std::filesystem::remove_all(base);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_scheduler_serializes_tasks_and_advances_time);
  RUN_TEST(test_scheduler_prefers_priority_then_creation_order);
  RUN_TEST(test_notification_timeout_uses_simulated_time);
  RUN_TEST(test_run_ready_stops_at_requested_simulated_time);
  RUN_TEST(test_application_lifecycle_runs_setup_then_bounded_loops);
  RUN_TEST(test_freertos_compat_serializes_mutex_and_notifications);
  RUN_TEST(test_wall_time_watchdog_terminates_stalled_process);
  RUN_TEST(test_directory_storage_copies_fixture_and_sorts_entries);
  RUN_TEST(test_device_path_hash_matches_32_bit_libstdcpp);
  RUN_TEST(test_native_hal_storage_uses_isolated_run_copy);
  RUN_TEST(test_epub_cache_and_progress_round_trip_through_shared_readers);
  RUN_TEST(test_real_x3_x4_drivers_produce_distinct_retained_panel_state);
  return UNITY_END();
}
