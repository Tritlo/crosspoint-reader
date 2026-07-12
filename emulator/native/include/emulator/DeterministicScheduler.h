#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "emulator/SimulatedClock.h"

namespace emulator {

class DeterministicScheduler {
 public:
  using TaskId = uint64_t;
  using TaskFunction = std::function<void()>;

  struct TaskStatus {
    TaskId id;
    std::string name;
    uint32_t priority;
    std::string state;
    uint32_t notifications;
    uint64_t wakeTimeUs;
  };

  struct RunResult {
    bool quiescent;
    uint64_t dispatches;
  };

  explicit DeterministicScheduler(SimulatedClock& clock,
                                  std::chrono::milliseconds wallTimeLimit = std::chrono::seconds(5));
  ~DeterministicScheduler();

  DeterministicScheduler(const DeterministicScheduler&) = delete;
  DeterministicScheduler& operator=(const DeterministicScheduler&) = delete;

  TaskId createTask(std::string name, TaskFunction function, uint32_t priority = 0);
  RunResult run(uint64_t maxDispatches = 100000);
  RunResult runReady(uint64_t maxDispatches = 100000);
  RunResult advance(uint64_t microseconds, uint64_t maxDispatches = 100000);

  void notify(TaskId task, uint32_t count = 1);
  void makeReady(TaskId task);

  // These calls are valid only from a scheduler task.
  void yield();
  void delay(uint64_t microseconds);
  void block();
  uint32_t takeNotification(bool clearOnExit, uint64_t timeoutUs = UINT64_MAX);
  TaskId currentTaskId() const;

  std::vector<TaskStatus> taskStatuses() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

}  // namespace emulator
