#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

#include "emulator/DeterministicScheduler.h"

namespace emulator {

class ApplicationLifecycle {
 public:
  using Hook = void (*)();

  ApplicationLifecycle(DeterministicScheduler& scheduler, Hook setup, Hook loop);

  bool available() const;
  DeterministicScheduler::RunResult start();
  DeterministicScheduler::RunResult advance(uint64_t microseconds);
  std::string_view state() const;

 private:
  enum class State { Unavailable, NotStarted, SettingUp, Ready, Running };

  DeterministicScheduler& scheduler;
  Hook setupHook;
  Hook loopHook;
  std::atomic<State> currentState;
  DeterministicScheduler::TaskId mainTask = 0;
};

}  // namespace emulator
