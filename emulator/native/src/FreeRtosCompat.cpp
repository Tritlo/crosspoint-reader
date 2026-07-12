#include "emulator/FreeRtosCompat.h"

#include <Arduino.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "emulator/DirectoryStorage.h"

struct EmulatorTaskControl {
  emulator::DeterministicScheduler::TaskId id;
  std::string name;
};

struct EmulatorSemaphore {
  explicit EmulatorSemaphore(bool recursive) : recursive(recursive) {}

  bool recursive;
  emulator::DeterministicScheduler::TaskId owner = 0;
  uint32_t depth = 0;
  std::deque<emulator::DeterministicScheduler::TaskId> waiters;
};

struct EmulatorQueue {
  EmulatorQueue(size_t capacity, size_t itemSize) : capacity(capacity), itemSize(itemSize) {}

  size_t capacity;
  size_t itemSize;
  std::deque<std::vector<uint8_t>> items;
};

namespace emulator {
namespace {

struct RuntimeState {
  DeterministicScheduler* scheduler = nullptr;
  SimulatedClock* clock = nullptr;
  DirectoryStorage* storage = nullptr;
  StorageTraceCallback storageTrace;
  PanelTraceCallback panelTrace;
  ApplicationTraceCallback applicationTrace;
  std::vector<std::unique_ptr<EmulatorTaskControl>> tasks;
};

RuntimeState runtime;

RuntimeState& activeRuntime() {
  if (runtime.scheduler == nullptr || runtime.clock == nullptr) {
    throw std::logic_error("Arduino/FreeRTOS operation used without an active emulator runtime");
  }
  return runtime;
}

EmulatorTaskControl* taskFor(DeterministicScheduler::TaskId id) {
  auto& state = activeRuntime();
  const auto found =
      std::find_if(state.tasks.begin(), state.tasks.end(), [id](const auto& task) { return task->id == id; });
  if (found == state.tasks.end()) throw std::logic_error("scheduler task has no FreeRTOS handle");
  return found->get();
}

uint64_t timeoutMicroseconds(TickType_t ticks) {
  if (ticks == portMAX_DELAY) return UINT64_MAX;
  return static_cast<uint64_t>(ticks) * portTICK_PERIOD_MS * 1000;
}

BaseType_t takeSemaphore(EmulatorSemaphore* semaphore, TickType_t ticksToWait, bool recursiveCall) {
  if (semaphore == nullptr) return pdFAIL;
  auto& state = activeRuntime();
  if (state.scheduler->isShuttingDown()) return pdPASS;
  const auto current = state.scheduler->currentTaskId();
  if (semaphore->owner == 0) {
    semaphore->owner = current;
    semaphore->depth = 1;
    return pdPASS;
  }
  if (semaphore->owner == current) {
    if (!semaphore->recursive || !recursiveCall) return pdFAIL;
    ++semaphore->depth;
    return pdPASS;
  }
  if (ticksToWait == 0) return pdFAIL;
  if (ticksToWait != portMAX_DELAY) {
    throw std::logic_error("finite semaphore waits are not supported by emulator-v1");
  }
  semaphore->waiters.push_back(current);
  state.scheduler->block();
  return semaphore->owner == current ? pdPASS : pdFAIL;
}

BaseType_t giveSemaphore(EmulatorSemaphore* semaphore, bool recursiveCall) {
  if (semaphore == nullptr) return pdFAIL;
  auto& state = activeRuntime();
  if (state.scheduler->isShuttingDown()) return pdPASS;
  const auto current = state.scheduler->currentTaskId();
  if (semaphore->owner != current) return pdFAIL;
  if (semaphore->recursive && recursiveCall && semaphore->depth > 1) {
    --semaphore->depth;
    return pdPASS;
  }

  semaphore->owner = 0;
  semaphore->depth = 0;
  if (!semaphore->waiters.empty()) {
    const auto next = semaphore->waiters.front();
    semaphore->waiters.pop_front();
    semaphore->owner = next;
    semaphore->depth = 1;
    state.scheduler->makeReady(next);
  }
  return pdPASS;
}

}  // namespace

FreeRtosRuntime::FreeRtosRuntime(DeterministicScheduler& scheduler, SimulatedClock& clock, DirectoryStorage* storage,
                                 StorageTraceCallback storageTrace, PanelTraceCallback panelTrace,
                                 ApplicationTraceCallback applicationTrace) {
  if (runtime.scheduler != nullptr) throw std::logic_error("only one emulator runtime may be active");
  runtime.scheduler = &scheduler;
  runtime.clock = &clock;
  runtime.storage = storage;
  runtime.storageTrace = std::move(storageTrace);
  runtime.panelTrace = std::move(panelTrace);
  runtime.applicationTrace = std::move(applicationTrace);
}

FreeRtosRuntime::~FreeRtosRuntime() {
  runtime.tasks.clear();
  runtime.scheduler = nullptr;
  runtime.clock = nullptr;
  runtime.storage = nullptr;
  runtime.storageTrace = {};
  runtime.panelTrace = {};
  runtime.applicationTrace = {};
}

uint64_t runtimeMicroseconds() { return activeRuntime().clock->nowMicroseconds(); }

bool runtimeIsActive() { return runtime.scheduler != nullptr && runtime.clock != nullptr; }

void runtimeDelay(uint64_t microseconds) { activeRuntime().scheduler->delay(microseconds); }

void runtimeYield() { activeRuntime().scheduler->yield(); }

DirectoryStorage& runtimeStorage() {
  auto& state = activeRuntime();
  if (state.storage == nullptr) throw std::logic_error("storage operation used without configured run storage");
  return *state.storage;
}

void runtimeTraceStorage(std::string_view operation, std::string_view path, uint64_t bytes, bool success) {
  auto& state = activeRuntime();
  if (state.storageTrace) state.storageTrace(operation, path, bytes, success);
}

void runtimeTracePanel(std::string_view event, std::string_view detail, uint64_t value) {
  auto& state = activeRuntime();
  if (state.panelTrace) state.panelTrace(event, detail, value);
}

void runtimeTraceApplication(std::string_view event, std::string_view detail, uint64_t value) {
  auto& state = activeRuntime();
  if (state.applicationTrace) state.applicationTrace(event, detail, value);
}

}  // namespace emulator

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char* name, uint32_t, void* parameter,
                                   UBaseType_t priority, TaskHandle_t* createdTask, BaseType_t core) {
  if (function == nullptr || createdTask == nullptr || core != 0) return pdFAIL;
  auto& state = emulator::activeRuntime();
  auto task = std::make_unique<EmulatorTaskControl>();
  task->name = name == nullptr ? "unnamed" : name;
  EmulatorTaskControl* handle = task.get();
  handle->id = state.scheduler->createTask(handle->name, [function, parameter] { function(parameter); }, priority);
  state.tasks.push_back(std::move(task));
  *createdTask = handle;
  return pdPASS;
}

TaskHandle_t xTaskGetCurrentTaskHandle() {
  auto& state = emulator::activeRuntime();
  return emulator::taskFor(state.scheduler->currentTaskId());
}

BaseType_t xTaskNotify(TaskHandle_t task, uint32_t, eNotifyAction action) {
  if (task == nullptr || action != eIncrement) return pdFAIL;
  emulator::activeRuntime().scheduler->notify(task->id);
  return pdPASS;
}

uint32_t ulTaskNotifyTake(BaseType_t clearCountOnExit, TickType_t ticksToWait) {
  return emulator::activeRuntime().scheduler->takeNotification(clearCountOnExit == pdTRUE,
                                                               emulator::timeoutMicroseconds(ticksToWait));
}

void vTaskDelay(TickType_t ticks) {
  if (ticks == 0)
    emulator::activeRuntime().scheduler->yield();
  else
    emulator::activeRuntime().scheduler->delay(emulator::timeoutMicroseconds(ticks));
}

SemaphoreHandle_t xSemaphoreCreateMutex() { return new EmulatorSemaphore(false); }

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return new EmulatorSemaphore(true); }

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticksToWait) {
  return emulator::takeSemaphore(semaphore, ticksToWait, false);
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) { return emulator::giveSemaphore(semaphore, false); }

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, TickType_t ticksToWait) {
  return emulator::takeSemaphore(semaphore, ticksToWait, true);
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore) { return emulator::giveSemaphore(semaphore, true); }

TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t semaphore) {
  if (emulator::activeRuntime().scheduler->isShuttingDown()) return nullptr;
  if (semaphore == nullptr || semaphore->owner == 0) return nullptr;
  return emulator::taskFor(semaphore->owner);
}

BaseType_t xQueuePeek(QueueHandle_t queue, void*, TickType_t ticksToWait) {
  if (queue == nullptr || ticksToWait != 0) return pdFAIL;
  const auto* semaphore = static_cast<const EmulatorSemaphore*>(queue);
  return semaphore->owner == 0 ? pdTRUE : pdFALSE;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize) {
  if (length == 0 || itemSize == 0) return nullptr;
  return new EmulatorQueue(length, itemSize);
}

BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t ticksToWait) {
  if (queue == nullptr || item == nullptr || ticksToWait != 0) return pdFAIL;
  auto* nativeQueue = static_cast<EmulatorQueue*>(queue);
  if (nativeQueue->items.size() >= nativeQueue->capacity) return pdFAIL;
  nativeQueue->items.emplace_back(nativeQueue->itemSize);
  std::memcpy(nativeQueue->items.back().data(), item, nativeQueue->itemSize);
  return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t ticksToWait) {
  if (queue == nullptr || item == nullptr || ticksToWait != 0) return pdFAIL;
  auto* nativeQueue = static_cast<EmulatorQueue*>(queue);
  if (nativeQueue->items.empty()) return pdFAIL;
  std::memcpy(item, nativeQueue->items.front().data(), nativeQueue->itemSize);
  nativeQueue->items.pop_front();
  return pdPASS;
}

void emulatorTaskEnterCritical(portMUX_TYPE*) {}

void emulatorTaskExitCritical(portMUX_TYPE*) {}

unsigned long millis() {
  return emulator::runtimeIsActive() ? static_cast<unsigned long>(emulator::runtimeMicroseconds() / 1000) : 0;
}

unsigned long micros() {
  return emulator::runtimeIsActive() ? static_cast<unsigned long>(emulator::runtimeMicroseconds()) : 0;
}

void delay(unsigned long milliseconds) { emulator::runtimeDelay(static_cast<uint64_t>(milliseconds) * 1000); }

void delayMicroseconds(unsigned int microseconds) { emulator::runtimeDelay(microseconds); }

void yield() { emulator::runtimeYield(); }
