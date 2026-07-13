#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "emulator/DeterministicScheduler.h"
#include "emulator/SimulatedClock.h"

namespace emulator {

class DirectoryStorage;
enum class RenderTimingPhase { BeforePrimary, GrayscaleLsb, GrayscaleMsb, Cleanup };
enum class IndexingTimingPhase { Opf, Toc, BookBin };
using StorageTraceCallback =
    std::function<void(std::string_view operation, std::string_view path, uint64_t bytes, bool success)>;
using PanelTraceCallback = std::function<void(std::string_view event, std::string_view detail, uint64_t value)>;
using ApplicationTraceCallback = std::function<void(std::string_view event, std::string_view detail, uint64_t value)>;
using TimingTraceCallback = std::function<void(std::string_view model, std::string_view detail, uint64_t targetUs,
                                               uint64_t elapsedUs, uint64_t remainingUs)>;

// Installs the process-wide Arduino/FreeRTOS compatibility context. The
// emulator intentionally supports one device session per process.
class FreeRtosRuntime {
 public:
  FreeRtosRuntime(DeterministicScheduler& scheduler, SimulatedClock& clock, DirectoryStorage* storage = nullptr,
                  StorageTraceCallback storageTrace = {}, PanelTraceCallback panelTrace = {},
                  ApplicationTraceCallback applicationTrace = {}, TimingTraceCallback timingTrace = {});
  ~FreeRtosRuntime();

  FreeRtosRuntime(const FreeRtosRuntime&) = delete;
  FreeRtosRuntime& operator=(const FreeRtosRuntime&) = delete;
};

uint64_t runtimeMicroseconds();
bool runtimeIsActive();
void runtimeDelay(uint64_t microseconds);
void runtimeTraceTiming(std::string_view model, std::string_view detail, uint64_t targetUs, uint64_t elapsedUs = 0);
void runtimeApplyTiming(std::string_view model, std::string_view detail, uint64_t targetUs, uint64_t elapsedUs = 0);
void runtimeYield();
void runtimeBlock();
void runtimeBeginActivityTiming();
void runtimeFinishActivityTiming(std::string_view activityId);
void runtimeBeginActivityRender(std::string_view activityId);
void runtimeMarkActivityRenderCleared();
void runtimeFinishActivityRender();
void runtimeSetRenderTimingMode(bool halfPrimary);
void runtimeDelayRenderPhase(RenderTimingPhase phase);
void runtimeFinishImageRender(bool cached, uint16_t width, uint16_t height, uint64_t sourceBytes, uint64_t startedUs);
void runtimeBeginImagePreparation();
void runtimeCancelImagePreparation();
bool runtimeImagePreparationActive();
void runtimeFinishImagePreparation(uint64_t sourceBytes, uint64_t startedUs);
void runtimeBeginSectionStreaming();
void runtimeActivateSectionStreaming();
void runtimeCancelSectionStreaming();
void runtimeFinishSectionStreaming();
void runtimeBeginSectionImageDiscovery();
void runtimeCancelSectionImageDiscovery();
void runtimeFinishSectionImageDiscovery();
bool runtimeSectionTimingActive();
void runtimeFinishColdIndexingPhase(std::string_view epubPath, bool largeSpine, IndexingTimingPhase phase,
                                    uint64_t startedUs);
void runtimeBeginColdTocTiming(std::string_view epubPath, bool largeSpine);
void runtimeCancelColdTocTiming();
bool runtimeColdTocTimingActive();
void runtimeFinishColdIndexing(std::string_view epubPath, bool largeSpine, uint64_t startedUs);
void runtimeBeginColdPostIndexTiming(std::string_view epubPath, bool largeSpine);
void runtimeCancelColdPostIndexTiming();
bool runtimeColdPostIndexTimingActive();
void runtimeFinishColdPostIndexLoad(std::string_view epubPath, bool largeSpine, uint64_t startedUs);
void runtimeBeginWarmOpen(std::string_view epubPath);
void runtimeCancelWarmOpen();
bool runtimeWarmOpenTimingActive();
void runtimeFinishCachedMetadataLoad(std::string_view epubPath, bool largeSpine, uint64_t startedUs);
void runtimeFinishWarmOpen();
uint64_t runtimeBeginThumbnailGeneration();
enum class ThumbnailFormat { Jpeg, Png };
enum class ThumbnailTimingPhase { Whole, Preparation, Conversion };
void runtimeConfigureThumbnailGeneration(ThumbnailFormat format, ThumbnailTimingPhase phase, uint64_t sourceBytes,
                                         uint32_t sourceWidth, uint32_t sourceHeight, uint8_t bitDepth,
                                         uint8_t colorType, uint16_t targetWidth, uint16_t targetHeight);
void runtimeFinishThumbnailGeneration(ThumbnailFormat format, ThumbnailTimingPhase phase, uint64_t sourceBytes,
                                      uint32_t sourceWidth, uint32_t sourceHeight, uint8_t bitDepth, uint8_t colorType,
                                      uint16_t targetWidth, uint16_t targetHeight, uint64_t startedUs);
bool runtimeThumbnailTimingActive();
void runtimeBeginSleepTransition();
void runtimeFinishSleepTransition();
DirectoryStorage& runtimeStorage();
void runtimeTraceStorage(std::string_view operation, std::string_view path, uint64_t bytes, bool success);
void runtimeTracePanel(std::string_view event, std::string_view detail, uint64_t value);
void runtimeTraceApplication(std::string_view event, std::string_view detail, uint64_t value);

}  // namespace emulator
