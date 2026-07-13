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
  TimingTraceCallback timingTrace;
  std::vector<std::unique_ptr<EmulatorTaskControl>> tasks;
  uint64_t warmLoadStartedUs = 0;
  uint64_t warmPageLoadTargetUs = 0;
  bool warmOpenTimingActive = false;
  bool halfPrimaryRender = false;
  bool thumbnailTimingActive = false;
  bool coldTocTimingActive = false;
  bool coldPostIndexTimingActive = false;
  bool imagePreparationActive = false;
  uint64_t sectionStreamingStartedUs = 0;
  bool sectionStreamingStorageActive = false;
  uint64_t sectionImageDiscoveryStartedUs = 0;
  uint64_t sleepTransitionStartedUs = 0;
  uint64_t activityTimingStartedUs = 0;
  uint64_t activityRenderStartedUs = 0;
  std::string activityRenderId;
  uint32_t activityRenderCount = 0;
  bool activityRenderActive = false;
  bool activityRenderCleared = false;
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

const IndexingWorkloadTiming* exactIndexingTiming(std::string_view epubPath) {
  const auto& workloads = runtimeStorage().config().timing.workload.exactIndexing;
  const auto found = std::find_if(workloads.begin(), workloads.end(),
                                  [epubPath](const auto& timing) { return timing.path == epubPath; });
  return found == workloads.end() ? nullptr : &*found;
}

const WarmWorkloadTiming* exactWarmTiming(std::string_view epubPath) {
  const auto& workloads = runtimeStorage().config().timing.workload.exactWarm;
  const auto found = std::find_if(workloads.begin(), workloads.end(),
                                  [epubPath](const auto& timing) { return timing.path == epubPath; });
  return found == workloads.end() ? nullptr : &*found;
}

bool hasIndexingTiming(std::string_view epubPath, bool largeSpine) {
  const auto& timing = runtimeStorage().config().timing;
  return timing.calibrated && (largeSpine || exactIndexingTiming(epubPath) != nullptr);
}

bool hasPngThumbnailPreparationTiming(uint64_t sourceBytes) {
  const auto& timing = runtimeStorage().config().timing;
  const auto& model = timing.workload.pngThumbnail;
  return timing.calibrated && sourceBytes >= model.minImageBytes && sourceBytes <= model.maxImageBytes;
}

bool hasPngThumbnailConversionTiming(uint32_t sourceWidth, uint32_t sourceHeight, uint8_t bitDepth, uint8_t colorType,
                                     uint16_t targetWidth, uint16_t targetHeight) {
  const auto& timing = runtimeStorage().config().timing;
  const auto& model = timing.workload.pngThumbnail;
  const uint64_t sourcePixels = static_cast<uint64_t>(sourceWidth) * sourceHeight;
  return timing.calibrated && bitDepth == model.bitDepth && colorType == model.colorType &&
         targetWidth == model.targetWidth && targetHeight == model.targetHeight &&
         sourcePixels >= model.minSourcePixels && sourcePixels <= model.maxSourcePixels;
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
                                 ApplicationTraceCallback applicationTrace, TimingTraceCallback timingTrace) {
  if (runtime.scheduler != nullptr) throw std::logic_error("only one emulator runtime may be active");
  runtime.scheduler = &scheduler;
  runtime.clock = &clock;
  runtime.storage = storage;
  runtime.storageTrace = std::move(storageTrace);
  runtime.panelTrace = std::move(panelTrace);
  runtime.applicationTrace = std::move(applicationTrace);
  runtime.timingTrace = std::move(timingTrace);
  runtime.warmLoadStartedUs = 0;
  runtime.warmPageLoadTargetUs = 0;
  runtime.warmOpenTimingActive = false;
  runtime.halfPrimaryRender = false;
  runtime.thumbnailTimingActive = false;
  runtime.coldTocTimingActive = false;
  runtime.coldPostIndexTimingActive = false;
  runtime.imagePreparationActive = false;
  runtime.sectionStreamingStartedUs = 0;
  runtime.sectionStreamingStorageActive = false;
  runtime.sectionImageDiscoveryStartedUs = 0;
  runtime.sleepTransitionStartedUs = 0;
  runtime.activityTimingStartedUs = 0;
  runtime.activityRenderStartedUs = 0;
  runtime.activityRenderId.clear();
  runtime.activityRenderCount = 0;
  runtime.activityRenderActive = false;
  runtime.activityRenderCleared = false;
}

FreeRtosRuntime::~FreeRtosRuntime() {
  runtime.tasks.clear();
  runtime.scheduler = nullptr;
  runtime.clock = nullptr;
  runtime.storage = nullptr;
  runtime.storageTrace = {};
  runtime.panelTrace = {};
  runtime.applicationTrace = {};
  runtime.timingTrace = {};
  runtime.warmLoadStartedUs = 0;
  runtime.warmPageLoadTargetUs = 0;
  runtime.warmOpenTimingActive = false;
  runtime.halfPrimaryRender = false;
  runtime.thumbnailTimingActive = false;
  runtime.coldTocTimingActive = false;
  runtime.coldPostIndexTimingActive = false;
  runtime.imagePreparationActive = false;
  runtime.sectionStreamingStartedUs = 0;
  runtime.sectionStreamingStorageActive = false;
  runtime.sectionImageDiscoveryStartedUs = 0;
  runtime.sleepTransitionStartedUs = 0;
  runtime.activityTimingStartedUs = 0;
  runtime.activityRenderStartedUs = 0;
  runtime.activityRenderId.clear();
  runtime.activityRenderCount = 0;
  runtime.activityRenderActive = false;
  runtime.activityRenderCleared = false;
}

uint64_t runtimeMicroseconds() { return activeRuntime().clock->nowMicroseconds(); }

bool runtimeIsActive() { return runtime.scheduler != nullptr && runtime.clock != nullptr; }

void runtimeDelay(uint64_t microseconds) { activeRuntime().scheduler->delay(microseconds); }

void runtimeTraceTiming(std::string_view model, std::string_view detail, uint64_t targetUs, uint64_t elapsedUs) {
  auto& state = activeRuntime();
  const uint64_t remainingUs = targetUs > elapsedUs ? targetUs - elapsedUs : 0;
  if (state.timingTrace) state.timingTrace(model, detail, targetUs, elapsedUs, remainingUs);
}

void runtimeApplyTiming(std::string_view model, std::string_view detail, uint64_t targetUs, uint64_t elapsedUs) {
  runtimeTraceTiming(model, detail, targetUs, elapsedUs);
  const uint64_t remainingUs = targetUs > elapsedUs ? targetUs - elapsedUs : 0;
  if (remainingUs != 0) runtimeDelay(remainingUs);
}

void runtimeYield() { activeRuntime().scheduler->yield(); }

void runtimeBlock() { activeRuntime().scheduler->block(); }

void runtimeBeginActivityTiming() {
  auto& state = activeRuntime();
  state.activityTimingStartedUs = runtimeMicroseconds();
  state.activityRenderCount = 0;
  state.activityRenderActive = false;
  state.activityRenderCleared = false;
}

void runtimeFinishActivityTiming(std::string_view activityId) {
  auto& state = activeRuntime();
  const uint64_t startedUs = state.activityTimingStartedUs;
  state.activityTimingStartedUs = 0;
  const auto& timing = runtimeStorage().config().timing;
  if (startedUs == 0 || !timing.calibrated || activityId != "file_browser") return;
  const uint64_t elapsedUs = runtimeMicroseconds() - startedUs;
  const uint64_t targetUs = timing.workload.fileBrowserActivityToDisplayUs - timing.workload.fileBrowserRenderUs;
  runtimeApplyTiming("workload.activity.file-browser.pre-render", activityId, targetUs, elapsedUs);
}

void runtimeBeginActivityRender(std::string_view activityId) {
  auto& state = activeRuntime();
  state.activityRenderStartedUs = runtimeMicroseconds();
  state.activityRenderId = activityId;
  state.activityRenderActive = true;
  state.activityRenderCleared = false;
}

void runtimeMarkActivityRenderCleared() { activeRuntime().activityRenderCleared = true; }

void runtimeFinishActivityRender() {
  auto& state = activeRuntime();
  if (!state.activityRenderActive) return;
  state.activityRenderActive = false;

  const auto& timing = runtimeStorage().config().timing;
  uint64_t targetUs = 0;
  std::string_view model;
  if (timing.calibrated && state.activityRenderId == "home") {
    if (state.activityRenderCount == 0) {
      targetUs = timing.workload.homeFirstRenderUs;
      model = "workload.activity.home.first-render";
    } else {
      targetUs = timing.workload.homeSubsequentRenderUs;
      model = "workload.activity.home.subsequent-render";
    }
  } else if (timing.calibrated && state.activityRenderId == "file_browser") {
    targetUs = timing.workload.fileBrowserRenderUs;
    model = "workload.activity.file-browser.render";
  } else if (timing.calibrated && state.activityRenderId == "settings" && state.activityRenderCleared) {
    if (state.activityRenderCount == 0) {
      targetUs = timing.workload.settingsFirstRenderUs;
      model = "workload.activity.settings.first-render";
    } else {
      targetUs = timing.workload.settingsSubsequentRenderUs;
      model = "workload.activity.settings.subsequent-render";
    }
  } else if (timing.calibrated && state.activityRenderId == "settings") {
    targetUs = timing.workload.settingsPopupRenderUs;
    model = "workload.activity.settings.popup-render";
  } else if (timing.calibrated && state.activityRenderId == "reader.epub.menu" && state.activityRenderCleared) {
    if (state.activityRenderCount == 0) {
      targetUs = timing.workload.readerMenuFirstRenderUs;
      model = "workload.activity.reader-menu.first-render";
    } else {
      targetUs = timing.workload.readerMenuSubsequentRenderUs;
      model = "workload.activity.reader-menu.subsequent-render";
    }
  } else if (timing.calibrated && state.activityRenderId == "reader.epub.menu") {
    targetUs = timing.workload.readerMenuPopupRenderUs;
    model = "workload.activity.reader-menu.popup-render";
  } else if (timing.calibrated && state.activityRenderId == "reader.epub.percent" && state.activityRenderCleared) {
    if (state.activityRenderCount == 0) {
      targetUs = timing.workload.readerPercentFirstRenderUs;
      model = "workload.activity.reader-percent.first-render";
    } else {
      targetUs = timing.workload.readerPercentSubsequentRenderUs;
      model = "workload.activity.reader-percent.subsequent-render";
    }
  }
  ++state.activityRenderCount;
  const uint64_t elapsedUs = runtimeMicroseconds() - state.activityRenderStartedUs;
  if (targetUs != 0) runtimeApplyTiming(model, state.activityRenderId, targetUs, elapsedUs);
}

void runtimeSetRenderTimingMode(bool halfPrimary) { activeRuntime().halfPrimaryRender = halfPrimary; }

void runtimeDelayRenderPhase(RenderTimingPhase phase) {
  const auto& timing = runtimeStorage().config().timing;
  if (!timing.calibrated) return;
  const bool halfPrimary = activeRuntime().halfPrimaryRender;
  const auto& render = halfPrimary ? timing.render.half : timing.render.fast;
  uint64_t delayUs = 0;
  std::string_view model;
  switch (phase) {
    case RenderTimingPhase::BeforePrimary:
      delayUs = render.beforePrimaryUs;
      model = halfPrimary ? "render.half.before-primary" : "render.fast.before-primary";
      break;
    case RenderTimingPhase::GrayscaleLsb:
      delayUs = render.grayscaleLsbUs;
      model = halfPrimary ? "render.half.grayscale-lsb" : "render.fast.grayscale-lsb";
      break;
    case RenderTimingPhase::GrayscaleMsb:
      delayUs = render.grayscaleMsbUs;
      model = halfPrimary ? "render.half.grayscale-msb" : "render.fast.grayscale-msb";
      break;
    case RenderTimingPhase::Cleanup:
      delayUs = render.cleanupUs;
      model = halfPrimary ? "render.half.cleanup" : "render.fast.cleanup";
      break;
  }
  runtimeApplyTiming(model, {}, delayUs);
}

void runtimeFinishImageRender(bool cached, uint16_t width, uint16_t height, uint64_t sourceBytes, uint64_t startedUs) {
  const auto& configuration = runtimeStorage().config();
  const auto& timing = configuration.timing;
  if (!timing.calibrated) return;
  const bool large = static_cast<uint32_t>(width) * height >=
                     static_cast<uint32_t>(configuration.profile.panelWidth) * configuration.profile.panelHeight / 2;
  uint64_t targetUs;
  std::string_view model;
  if (cached) {
    targetUs = large ? timing.render.cachedLargeImageUs : timing.render.cachedSmallImageUs;
    model = large ? "render.image.cached.large" : "render.image.cached.small";
  } else {
    const auto exact = std::find_if(timing.render.exactImageDecode.begin(), timing.render.exactImageDecode.end(),
                                    [sourceBytes, width, height](const auto& candidate) {
                                      return candidate.sourceBytes == sourceBytes && candidate.width == width &&
                                             candidate.height == height;
                                    });
    if (exact != timing.render.exactImageDecode.end()) {
      targetUs = exact->durationUs;
      model = "render.image.decode.exact";
    } else if (sourceBytes <= timing.render.decodeSmallMaxBytes) {
      targetUs = timing.render.decodeImageUs;
      model = "render.image.decode.small";
    } else if (sourceBytes <= timing.render.decodeMediumMaxBytes) {
      targetUs = timing.render.decodeMediumImageUs;
      model = "render.image.decode.medium";
    } else {
      targetUs = timing.render.decodeLargeImageUs;
      model = "render.image.decode.large";
    }
  }
  const uint64_t elapsedUs = runtimeMicroseconds() - startedUs;
  runtimeApplyTiming(model, {}, targetUs, elapsedUs);
}

void runtimeBeginImagePreparation() { activeRuntime().imagePreparationActive = true; }

void runtimeCancelImagePreparation() { activeRuntime().imagePreparationActive = false; }

bool runtimeImagePreparationActive() { return activeRuntime().imagePreparationActive; }

void runtimeFinishImagePreparation(uint64_t sourceBytes, uint64_t startedUs) {
  activeRuntime().imagePreparationActive = false;
  const auto& timing = runtimeStorage().config().timing;
  if (!timing.calibrated) return;
  uint64_t targetUs;
  std::string_view model;
  const auto exact =
      std::find_if(timing.render.exactImagePreparation.begin(), timing.render.exactImagePreparation.end(),
                   [sourceBytes](const auto& candidate) { return candidate.sourceBytes == sourceBytes; });
  if (exact != timing.render.exactImagePreparation.end()) {
    targetUs = exact->durationUs;
    model = "render.image.prepare.exact";
  } else if (sourceBytes <= timing.render.decodeSmallMaxBytes) {
    targetUs = timing.render.prepareSmallImageUs;
    model = "render.image.prepare.small";
  } else if (sourceBytes <= timing.render.decodeMediumMaxBytes) {
    targetUs = timing.render.prepareMediumImageUs;
    model = "render.image.prepare.medium";
  } else {
    targetUs = timing.render.prepareLargeImageUs;
    model = "render.image.prepare.large";
  }
  const uint64_t elapsedUs = runtimeMicroseconds() - startedUs;
  runtimeApplyTiming(model, {}, targetUs, elapsedUs);
}

void runtimeBeginSectionStreaming() { activeRuntime().sectionStreamingStartedUs = runtimeMicroseconds(); }

void runtimeActivateSectionStreaming() { activeRuntime().sectionStreamingStorageActive = true; }

void runtimeCancelSectionStreaming() {
  auto& state = activeRuntime();
  state.sectionStreamingStartedUs = 0;
  state.sectionStreamingStorageActive = false;
}

void runtimeFinishSectionStreaming() {
  auto& state = activeRuntime();
  if (state.sectionStreamingStartedUs == 0) return;
  const uint64_t startedUs = state.sectionStreamingStartedUs;
  state.sectionStreamingStartedUs = 0;
  state.sectionStreamingStorageActive = false;
  const auto& timing = runtimeStorage().config().timing;
  if (!timing.calibrated) return;
  const uint64_t elapsedUs = runtimeMicroseconds() - startedUs;
  runtimeApplyTiming("render.section.streaming", {}, timing.render.sectionStreamingUs, elapsedUs);
}

void runtimeBeginSectionImageDiscovery() { activeRuntime().sectionImageDiscoveryStartedUs = runtimeMicroseconds(); }

void runtimeCancelSectionImageDiscovery() { activeRuntime().sectionImageDiscoveryStartedUs = 0; }

void runtimeFinishSectionImageDiscovery() {
  auto& state = activeRuntime();
  if (state.sectionImageDiscoveryStartedUs == 0) return;
  const uint64_t startedUs = state.sectionImageDiscoveryStartedUs;
  state.sectionImageDiscoveryStartedUs = 0;
  const auto& timing = runtimeStorage().config().timing;
  if (!timing.calibrated) return;
  const uint64_t elapsedUs = runtimeMicroseconds() - startedUs;
  runtimeApplyTiming("render.section.image-discovery", {}, timing.render.sectionImageDiscoveryUs, elapsedUs);
}

bool runtimeSectionTimingActive() {
  const auto& state = activeRuntime();
  return state.sectionStreamingStorageActive || state.sectionImageDiscoveryStartedUs != 0;
}

void runtimeFinishColdIndexingPhase(std::string_view epubPath, bool largeSpine, IndexingTimingPhase phase,
                                    uint64_t startedUs) {
  auto& state = activeRuntime();
  if (phase == IndexingTimingPhase::Toc) state.coldTocTimingActive = false;
  const auto& timing = runtimeStorage().config().timing;
  if (!hasIndexingTiming(epubPath, largeSpine)) return;
  const auto* exact = exactIndexingTiming(epubPath);
  const uint64_t targetUs =
      phase == IndexingTimingPhase::Opf   ? (exact != nullptr ? exact->opfUs : timing.workload.coldOpfUs)
      : phase == IndexingTimingPhase::Toc ? (exact != nullptr ? exact->tocUs : timing.workload.coldTocUs)
                                          : (exact != nullptr ? exact->bookBinUs : timing.workload.coldBookBinUs);
  const uint64_t elapsedUs = runtimeMicroseconds() - startedUs;
  const std::string_view model =
      phase == IndexingTimingPhase::Opf
          ? (exact != nullptr ? "workload.indexing.exact.opf" : "workload.indexing.large-spine.opf")
      : phase == IndexingTimingPhase::Toc
          ? (exact != nullptr ? "workload.indexing.exact.toc" : "workload.indexing.large-spine.toc")
          : (exact != nullptr ? "workload.indexing.exact.book-bin" : "workload.indexing.large-spine.book-bin");
  runtimeApplyTiming(model, epubPath, targetUs, elapsedUs);
}

void runtimeBeginColdTocTiming(std::string_view epubPath, bool largeSpine) {
  activeRuntime().coldTocTimingActive = hasIndexingTiming(epubPath, largeSpine);
}

void runtimeCancelColdTocTiming() { activeRuntime().coldTocTimingActive = false; }

bool runtimeColdTocTimingActive() { return activeRuntime().coldTocTimingActive; }

void runtimeFinishColdIndexing(std::string_view epubPath, bool largeSpine, uint64_t startedUs) {
  const auto& timing = runtimeStorage().config().timing;
  if (!hasIndexingTiming(epubPath, largeSpine)) return;
  const auto* exact = exactIndexingTiming(epubPath);
  const uint64_t targetUs = exact != nullptr ? exact->totalUs : timing.workload.coldIndexingUs;
  const uint64_t elapsedUs = runtimeMicroseconds() - startedUs;
  runtimeApplyTiming(exact != nullptr ? "workload.indexing.exact.total" : "workload.indexing.large-spine.total",
                     epubPath, targetUs, elapsedUs);
}

void runtimeBeginColdPostIndexTiming(std::string_view epubPath, bool largeSpine) {
  activeRuntime().coldPostIndexTimingActive = hasIndexingTiming(epubPath, largeSpine);
}

void runtimeCancelColdPostIndexTiming() { activeRuntime().coldPostIndexTimingActive = false; }

bool runtimeColdPostIndexTimingActive() { return activeRuntime().coldPostIndexTimingActive; }

void runtimeFinishColdPostIndexLoad(std::string_view epubPath, bool largeSpine, uint64_t startedUs) {
  activeRuntime().coldPostIndexTimingActive = false;
  const auto& timing = runtimeStorage().config().timing;
  if (!hasIndexingTiming(epubPath, largeSpine)) return;
  const auto* exact = exactIndexingTiming(epubPath);
  const uint64_t targetUs = exact != nullptr ? exact->postIndexLoadUs : timing.workload.coldPostIndexLoadUs;
  const uint64_t elapsedUs = runtimeMicroseconds() - startedUs;
  runtimeApplyTiming(
      exact != nullptr ? "workload.indexing.exact.post-index-load" : "workload.indexing.large-spine.post-index-load",
      epubPath, targetUs, elapsedUs);
}

void runtimeBeginWarmOpen(std::string_view epubPath) {
  auto& state = activeRuntime();
  state.warmLoadStartedUs = 0;
  state.warmPageLoadTargetUs = 0;
  state.warmOpenTimingActive = runtimeStorage().config().timing.calibrated && exactWarmTiming(epubPath) != nullptr;
}

void runtimeCancelWarmOpen() {
  auto& state = activeRuntime();
  state.warmLoadStartedUs = 0;
  state.warmPageLoadTargetUs = 0;
  state.warmOpenTimingActive = false;
}

bool runtimeWarmOpenTimingActive() { return activeRuntime().warmOpenTimingActive; }

void runtimeFinishCachedMetadataLoad(std::string_view epubPath, bool largeSpine, uint64_t startedUs) {
  auto& state = activeRuntime();
  state.warmLoadStartedUs = 0;
  state.warmPageLoadTargetUs = 0;
  const auto& timing = runtimeStorage().config().timing;
  const auto* exact = exactWarmTiming(epubPath);
  if (!timing.calibrated || (!largeSpine && exact == nullptr)) return;
  const uint64_t metadataTargetUs =
      exact != nullptr ? exact->cachedMetadataLoadUs : timing.workload.cachedMetadataLoadUs;
  const uint64_t elapsedUs = runtimeMicroseconds() - startedUs;
  runtimeApplyTiming(
      exact != nullptr ? "workload.warm.exact.cached-metadata" : "workload.warm.large-spine.cached-metadata", epubPath,
      metadataTargetUs, elapsedUs);
  state.warmLoadStartedUs = startedUs;
  state.warmPageLoadTargetUs =
      exact != nullptr ? exact->metadataStartToPageLoadUs : timing.workload.warmReadyFromMetadataStartUs;
}

void runtimeFinishWarmOpen() {
  auto& state = activeRuntime();
  state.warmOpenTimingActive = false;
  if (state.warmLoadStartedUs == 0) return;
  const uint64_t elapsedUs = runtimeMicroseconds() - state.warmLoadStartedUs;
  const uint64_t targetUs = state.warmPageLoadTargetUs;
  runtimeApplyTiming("workload.warm.metadata-to-page-load", {}, targetUs, elapsedUs);
  state.warmLoadStartedUs = 0;
  state.warmPageLoadTargetUs = 0;
}

uint64_t runtimeBeginThumbnailGeneration() {
  auto& state = activeRuntime();
  state.thumbnailTimingActive = false;
  return runtimeMicroseconds();
}

void runtimeConfigureThumbnailGeneration(ThumbnailFormat format, ThumbnailTimingPhase phase, uint64_t sourceBytes,
                                         uint32_t sourceWidth, uint32_t sourceHeight, uint8_t bitDepth,
                                         uint8_t colorType, uint16_t targetWidth, uint16_t targetHeight) {
  auto& state = activeRuntime();
  if (format == ThumbnailFormat::Jpeg && phase == ThumbnailTimingPhase::Whole) {
    state.thumbnailTimingActive = true;
    return;
  }
  state.thumbnailTimingActive =
      format == ThumbnailFormat::Png &&
      ((phase == ThumbnailTimingPhase::Preparation && hasPngThumbnailPreparationTiming(sourceBytes)) ||
       (phase == ThumbnailTimingPhase::Conversion &&
        hasPngThumbnailConversionTiming(sourceWidth, sourceHeight, bitDepth, colorType, targetWidth, targetHeight)));
}

void runtimeFinishThumbnailGeneration(ThumbnailFormat format, ThumbnailTimingPhase phase, uint64_t sourceBytes,
                                      uint32_t sourceWidth, uint32_t sourceHeight, uint8_t bitDepth, uint8_t colorType,
                                      uint16_t targetWidth, uint16_t targetHeight, uint64_t startedUs) {
  auto& state = activeRuntime();
  state.thumbnailTimingActive = false;
  const auto& timing = runtimeStorage().config().timing;
  if (!timing.calibrated) return;
  uint64_t targetUs = 0;
  std::string_view model;
  if (format == ThumbnailFormat::Png && phase == ThumbnailTimingPhase::Preparation) {
    if (!hasPngThumbnailPreparationTiming(sourceBytes)) return;
    const auto& pngModel = timing.workload.pngThumbnail;
    const uint64_t scaledNanoseconds = sourceBytes * pngModel.preparationNanosecondsPerImageByte;
    targetUs = pngModel.preparationInterceptUs + (scaledNanoseconds + 999) / 1000;
    model = "workload.thumbnail.png.preparation";
  } else if (format == ThumbnailFormat::Png && phase == ThumbnailTimingPhase::Conversion) {
    if (!hasPngThumbnailConversionTiming(sourceWidth, sourceHeight, bitDepth, colorType, targetWidth, targetHeight)) {
      return;
    }
    const auto& pngModel = timing.workload.pngThumbnail;
    const uint64_t sourcePixels = static_cast<uint64_t>(sourceWidth) * sourceHeight;
    const uint64_t scaledNanoseconds = sourcePixels * pngModel.conversionNanosecondsPerSourcePixel;
    targetUs = pngModel.conversionInterceptUs + (scaledNanoseconds + 999) / 1000;
    model = "workload.thumbnail.png.conversion";
  } else if (format == ThumbnailFormat::Jpeg && phase == ThumbnailTimingPhase::Whole) {
    const uint64_t scaledNanoseconds = sourceBytes * timing.workload.jpegThumbnailNanosecondsPerByte;
    targetUs = timing.workload.jpegThumbnailInterceptUs + (scaledNanoseconds + 999) / 1000;
    model = "workload.thumbnail.jpeg.whole";
  } else {
    return;
  }
  const uint64_t elapsedUs = runtimeMicroseconds() - startedUs;
  runtimeApplyTiming(model, {}, targetUs, elapsedUs);
}

bool runtimeThumbnailTimingActive() { return activeRuntime().thumbnailTimingActive; }

void runtimeBeginSleepTransition() { activeRuntime().sleepTransitionStartedUs = runtimeMicroseconds(); }

void runtimeFinishSleepTransition() {
  auto& state = activeRuntime();
  if (state.sleepTransitionStartedUs == 0) return;
  const uint64_t startedUs = state.sleepTransitionStartedUs;
  state.sleepTransitionStartedUs = 0;
  const auto& timing = runtimeStorage().config().timing;
  if (!timing.calibrated) return;
  const uint64_t elapsedUs = runtimeMicroseconds() - startedUs;
  runtimeApplyTiming("workload.sleep.entry", {}, timing.workload.sleepEntryToDeepSleepUs, elapsedUs);
}

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
