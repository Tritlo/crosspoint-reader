#include "emulator/ApplicationLifecycle.h"

#include <stdexcept>

namespace emulator {

ApplicationLifecycle::ApplicationLifecycle(DeterministicScheduler& scheduler, Hook setup, Hook loop)
    : scheduler(scheduler),
      setupHook(setup),
      loopHook(loop),
      currentState(setup != nullptr && loop != nullptr ? State::NotStarted : State::Unavailable) {}

bool ApplicationLifecycle::available() const { return currentState.load() != State::Unavailable; }

DeterministicScheduler::RunResult ApplicationLifecycle::start() {
  if (!available()) return {true, 0};
  State expected = State::NotStarted;
  if (!currentState.compare_exchange_strong(expected, State::SettingUp)) {
    throw std::logic_error("application lifecycle already started");
  }

  mainTask = scheduler.createTask("app-main", [this] {
    setupHook();
    currentState = State::Ready;
    scheduler.block();
    currentState = State::Running;
    while (true) {
      loopHook();
      scheduler.yield();
    }
  });
  return scheduler.run();
}

DeterministicScheduler::RunResult ApplicationLifecycle::advance(uint64_t microseconds) {
  if (currentState.load() == State::Ready) scheduler.makeReady(mainTask);
  return scheduler.advance(microseconds);
}

std::string_view ApplicationLifecycle::state() const {
  switch (currentState.load()) {
    case State::Unavailable: return "unavailable";
    case State::NotStarted: return "not-started";
    case State::SettingUp: return "setting-up";
    case State::Ready: return "setup-complete";
    case State::Running: return "running";
  }
  return "unknown";
}

}  // namespace emulator
