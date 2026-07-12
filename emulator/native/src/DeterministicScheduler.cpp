#include "emulator/DeterministicScheduler.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace emulator {
namespace {

class StopTask final : public std::exception {};

enum class TaskState { Ready, Running, Blocked, WaitingNotification, Sleeping, Stopped };

const char* stateName(TaskState state) {
  switch (state) {
    case TaskState::Ready:
      return "ready";
    case TaskState::Running:
      return "running";
    case TaskState::Blocked:
      return "blocked";
    case TaskState::WaitingNotification:
      return "waiting-notification";
    case TaskState::Sleeping:
      return "sleeping";
    case TaskState::Stopped:
      return "stopped";
  }
  return "unknown";
}

}  // namespace

struct DeterministicScheduler::Impl {
  struct Task {
    Task(TaskId id, std::string name, TaskFunction function, uint32_t priority)
        : id(id), name(std::move(name)), function(std::move(function)), priority(priority) {}

    TaskId id;
    std::string name;
    TaskFunction function;
    uint32_t priority;
    TaskState state = TaskState::Ready;
    uint32_t notifications = 0;
    uint64_t wakeTimeUs = 0;
    bool notificationTimeout = false;
    std::thread thread;
  };

  Impl(SimulatedClock& clock, std::chrono::milliseconds wallTimeLimit) : clock(clock), wallTimeLimit(wallTimeLimit) {}

  ~Impl() { shutdown(); }

  void shutdown() {
    {
      std::lock_guard lock(mutex);
      if (stopped) return;
      stopped = true;
      stopping = true;
      condition.notify_all();
    }
    for (const auto& task : tasks) {
      if (task->thread.joinable()) task->thread.join();
    }
  }

  TaskId createTask(std::string name, TaskFunction function, uint32_t priority) {
    std::unique_lock lock(mutex);
    const TaskId id = nextTaskId++;
    auto task = std::make_unique<Task>(id, std::move(name), std::move(function), priority);
    Task* taskPointer = task.get();
    tasks.push_back(std::move(task));
    taskPointer->thread = std::thread([this, taskPointer] { taskMain(*taskPointer); });
    return id;
  }

  RunResult run(uint64_t maxDispatches, bool advanceToNextWake) {
    std::unique_lock lock(mutex);
    uint64_t dispatches = 0;
    while (dispatches < maxDispatches) {
      wakeDueTasks();
      Task* task = nextReadyTask();
      if (task == nullptr) {
        if (!advanceToNextWake) return {true, dispatches};
        const auto nextWake = earliestWakeTime();
        if (!nextWake) return {true, dispatches};
        if (*nextWake > clock.nowMicroseconds() && !clock.advance(*nextWake - clock.nowMicroseconds())) {
          throw std::overflow_error("simulated clock overflow while waking a task");
        }
        continue;
      }

      dispatch(*task, lock, dispatches);
    }
    return {false, dispatches};
  }

  RunResult advance(uint64_t microseconds, uint64_t maxDispatches) {
    std::unique_lock lock(mutex);
    if (microseconds > std::numeric_limits<uint64_t>::max() - clock.nowMicroseconds()) {
      throw std::overflow_error("simulated clock overflow");
    }
    const uint64_t targetTime = clock.nowMicroseconds() + microseconds;
    uint64_t dispatches = 0;
    while (dispatches < maxDispatches) {
      wakeDueTasks();
      Task* task = nextReadyTask();
      if (task == nullptr) {
        const auto nextWake = earliestWakeTime();
        if (nextWake && *nextWake <= targetTime) {
          if (*nextWake > clock.nowMicroseconds()) clock.advance(*nextWake - clock.nowMicroseconds());
          continue;
        }
        if (clock.nowMicroseconds() < targetTime) clock.advance(targetTime - clock.nowMicroseconds());
        wakeDueTasks();
        task = nextReadyTask();
        if (task == nullptr) return {true, dispatches};
      }

      dispatch(*task, lock, dispatches);
    }
    return {false, dispatches};
  }

  void notify(TaskId id, uint32_t count) {
    std::lock_guard lock(mutex);
    Task& task = findTask(id);
    const uint64_t updated = static_cast<uint64_t>(task.notifications) + count;
    task.notifications = static_cast<uint32_t>(std::min<uint64_t>(updated, std::numeric_limits<uint32_t>::max()));
    if (task.state == TaskState::WaitingNotification) {
      task.state = TaskState::Ready;
      task.notificationTimeout = false;
      task.wakeTimeUs = 0;
    }
  }

  void dispatch(Task& task, std::unique_lock<std::mutex>& lock, uint64_t& dispatches) {
    runningTask = &task;
    task.state = TaskState::Running;
    condition.notify_all();
    const bool yielded = condition.wait_for(lock, wallTimeLimit, [this, &task] { return runningTask != &task; });
    if (!yielded) watchdog(task);
    ++dispatches;
  }

  void makeReady(TaskId id) {
    std::lock_guard lock(mutex);
    Task& task = findTask(id);
    if (task.state == TaskState::Blocked) task.state = TaskState::Ready;
  }

  void yield() {
    std::unique_lock lock(mutex);
    Task& task = currentTask();
    suspend(lock, task, TaskState::Ready);
  }

  void delay(uint64_t microseconds) {
    std::unique_lock lock(mutex);
    Task& task = currentTask();
    if (microseconds > std::numeric_limits<uint64_t>::max() - clock.nowMicroseconds()) {
      throw std::overflow_error("task delay overflows simulated time");
    }
    task.wakeTimeUs = clock.nowMicroseconds() + microseconds;
    suspend(lock, task, TaskState::Sleeping);
  }

  void block() {
    std::unique_lock lock(mutex);
    Task& task = currentTask();
    suspend(lock, task, TaskState::Blocked);
  }

  uint32_t takeNotification(bool clearOnExit, uint64_t timeoutUs) {
    std::unique_lock lock(mutex);
    Task& task = currentTask();
    if (task.notifications == 0) {
      if (timeoutUs == 0) return 0;
      task.notificationTimeout = timeoutUs != UINT64_MAX;
      if (task.notificationTimeout) {
        if (timeoutUs > std::numeric_limits<uint64_t>::max() - clock.nowMicroseconds()) {
          throw std::overflow_error("notification timeout overflows simulated time");
        }
        task.wakeTimeUs = clock.nowMicroseconds() + timeoutUs;
      }
      suspend(lock, task, TaskState::WaitingNotification);
    }

    const uint32_t result = task.notifications;
    if (clearOnExit)
      task.notifications = 0;
    else if (task.notifications > 0)
      --task.notifications;
    task.notificationTimeout = false;
    task.wakeTimeUs = 0;
    return result;
  }

  std::vector<TaskStatus> taskStatuses() const {
    std::lock_guard lock(mutex);
    std::vector<TaskStatus> statuses;
    statuses.reserve(tasks.size());
    for (const auto& task : tasks) {
      statuses.push_back(
          {task->id, task->name, task->priority, stateName(task->state), task->notifications, task->wakeTimeUs});
    }
    return statuses;
  }

  bool isShuttingDown() const {
    std::lock_guard lock(mutex);
    return stopping;
  }

  TaskId currentTaskId() const {
    std::lock_guard lock(mutex);
    if (threadTask == nullptr || runningTask != threadTask) {
      throw std::logic_error("current scheduler task requested outside the running task");
    }
    return threadTask->id;
  }

  void taskMain(Task& task) {
    std::unique_lock lock(mutex);
    threadTask = &task;
    condition.wait(lock, [this, &task] { return stopping || task.state == TaskState::Running; });
    if (stopping) return;
    lock.unlock();
    try {
      task.function();
    } catch (const StopTask&) {
    } catch (const std::exception& error) {
      std::cerr << "emulator: task '" << task.name << "' failed: " << error.what() << '\n';
    } catch (...) {
      std::cerr << "emulator: task '" << task.name << "' failed with an unknown exception\n";
    }
    lock.lock();
    task.state = TaskState::Stopped;
    task.wakeTimeUs = 0;
    if (runningTask == &task) runningTask = nullptr;
    condition.notify_all();
  }

  void suspend(std::unique_lock<std::mutex>& lock, Task& task, TaskState state) {
    task.state = state;
    runningTask = nullptr;
    condition.notify_all();
    condition.wait(lock, [this, &task] { return stopping || task.state == TaskState::Running; });
    if (stopping) throw StopTask();
  }

  Task& currentTask() {
    if (threadTask == nullptr || runningTask != threadTask) {
      throw std::logic_error("scheduler task operation called outside the running task");
    }
    return *threadTask;
  }

  Task& findTask(TaskId id) {
    const auto found = std::find_if(tasks.begin(), tasks.end(), [id](const auto& task) { return task->id == id; });
    if (found == tasks.end()) throw std::invalid_argument("unknown scheduler task");
    return **found;
  }

  Task* nextReadyTask() {
    Task* selected = nullptr;
    for (const auto& task : tasks) {
      if (task->state == TaskState::Ready && (selected == nullptr || task->priority > selected->priority)) {
        selected = task.get();
      }
    }
    return selected;
  }

  void wakeDueTasks() {
    const uint64_t now = clock.nowMicroseconds();
    for (const auto& task : tasks) {
      const bool sleeping = task->state == TaskState::Sleeping;
      const bool timedNotification = task->state == TaskState::WaitingNotification && task->notificationTimeout;
      if ((sleeping || timedNotification) && task->wakeTimeUs <= now) {
        task->state = TaskState::Ready;
        task->wakeTimeUs = 0;
        task->notificationTimeout = false;
      }
    }
  }

  std::optional<uint64_t> earliestWakeTime() const {
    std::optional<uint64_t> result;
    for (const auto& task : tasks) {
      const bool sleeping = task->state == TaskState::Sleeping;
      const bool timedNotification = task->state == TaskState::WaitingNotification && task->notificationTimeout;
      if ((sleeping || timedNotification) && (!result || task->wakeTimeUs < *result)) result = task->wakeTimeUs;
    }
    return result;
  }

  [[noreturn]] void watchdog(const Task& task) const {
    std::cerr << "emulator: wall-time watchdog: task '" << task.name << "' (id=" << task.id << ") did not yield within "
              << wallTimeLimit.count() << "ms\n";
    std::cerr << "emulator: scheduler diagnostics at simulatedTimeUs=" << clock.nowMicroseconds() << '\n';
    for (const auto& candidate : tasks) {
      std::cerr << "  task id=" << candidate->id << " name='" << candidate->name
                << "' state=" << stateName(candidate->state) << " notifications=" << candidate->notifications
                << " wakeTimeUs=" << candidate->wakeTimeUs << '\n';
    }
    std::cerr.flush();
    std::_Exit(70);
  }

  SimulatedClock& clock;
  std::chrono::milliseconds wallTimeLimit;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::vector<std::unique_ptr<Task>> tasks;
  Task* runningTask = nullptr;
  TaskId nextTaskId = 1;
  bool stopping = false;
  bool stopped = false;
  static thread_local Task* threadTask;
};

thread_local DeterministicScheduler::Impl::Task* DeterministicScheduler::Impl::threadTask = nullptr;

DeterministicScheduler::DeterministicScheduler(SimulatedClock& clock, std::chrono::milliseconds wallTimeLimit)
    : impl(std::make_unique<Impl>(clock, wallTimeLimit)) {}

DeterministicScheduler::~DeterministicScheduler() = default;

void DeterministicScheduler::shutdown() { impl->shutdown(); }

bool DeterministicScheduler::isShuttingDown() const { return impl->isShuttingDown(); }

DeterministicScheduler::TaskId DeterministicScheduler::createTask(std::string name, TaskFunction function,
                                                                  uint32_t priority) {
  return impl->createTask(std::move(name), std::move(function), priority);
}

DeterministicScheduler::RunResult DeterministicScheduler::run(uint64_t maxDispatches) {
  return impl->run(maxDispatches, true);
}

DeterministicScheduler::RunResult DeterministicScheduler::runReady(uint64_t maxDispatches) {
  return impl->run(maxDispatches, false);
}

DeterministicScheduler::RunResult DeterministicScheduler::advance(uint64_t microseconds, uint64_t maxDispatches) {
  return impl->advance(microseconds, maxDispatches);
}

void DeterministicScheduler::notify(TaskId task, uint32_t count) { impl->notify(task, count); }

void DeterministicScheduler::makeReady(TaskId task) { impl->makeReady(task); }

void DeterministicScheduler::yield() { impl->yield(); }

void DeterministicScheduler::delay(uint64_t microseconds) { impl->delay(microseconds); }

void DeterministicScheduler::block() { impl->block(); }

uint32_t DeterministicScheduler::takeNotification(bool clearOnExit, uint64_t timeoutUs) {
  return impl->takeNotification(clearOnExit, timeoutUs);
}

DeterministicScheduler::TaskId DeterministicScheduler::currentTaskId() const { return impl->currentTaskId(); }

std::vector<DeterministicScheduler::TaskStatus> DeterministicScheduler::taskStatuses() const {
  return impl->taskStatuses();
}

}  // namespace emulator
