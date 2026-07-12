#include <unity.h>

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#include <Arduino.h>

#include "emulator/DeterministicScheduler.h"
#include "emulator/ApplicationLifecycle.h"
#include "emulator/FreeRtosCompat.h"
#include "emulator/SimulatedClock.h"

using emulator::DeterministicScheduler;
using emulator::ApplicationLifecycle;
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_scheduler_serializes_tasks_and_advances_time);
  RUN_TEST(test_scheduler_prefers_priority_then_creation_order);
  RUN_TEST(test_notification_timeout_uses_simulated_time);
  RUN_TEST(test_run_ready_stops_at_requested_simulated_time);
  RUN_TEST(test_application_lifecycle_runs_setup_then_bounded_loops);
  RUN_TEST(test_freertos_compat_serializes_mutex_and_notifications);
  RUN_TEST(test_wall_time_watchdog_terminates_stalled_process);
  return UNITY_END();
}
