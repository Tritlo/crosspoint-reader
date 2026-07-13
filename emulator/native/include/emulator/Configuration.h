#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace emulator {

enum class Device { X3, X4 };

struct DeviceProfile {
  Device device;
  const char* id;
  uint16_t panelWidth;
  uint16_t panelHeight;
  const char* controller;
  uint16_t reviewRotationDegrees;
};

struct PanelTiming {
  uint64_t fastBusyUs = 0;
  uint64_t halfBusyUs = 0;
  uint64_t fullBusyUs = 0;
  uint64_t grayscaleAfterFastBusyUs = 0;
  uint64_t grayscaleAfterHalfBusyUs = 0;
  uint64_t fastOperationUs = 0;
  uint64_t halfOperationUs = 0;
  uint64_t fullOperationUs = 0;
};

struct OpticalTargetTiming {
  uint64_t onsetUs = 0;
  uint64_t endUs = 0;
};

struct OpticalModeTiming {
  OpticalTargetTiming black;
  OpticalTargetTiming white;
};

struct OpticalGrayscaleTiming {
  bool visibleChangeDetected = false;
  OpticalTargetTiming light;
  OpticalTargetTiming dark;
};

struct OpticalReaderHalfTiming {
  uint64_t invertedTargetUs = 0;
  uint64_t settledTargetUs = 0;
};

struct OpticalReaderFastTiming {
  uint64_t transitionStartUs = 0;
  uint64_t transitionEndUs = 0;
};

struct OpticalReaderImageFastTiming {
  OpticalReaderFastTiming timing;
  uint8_t darkPixelPercentMin = 0;
};

struct OpticalTiming {
  bool measured = false;
  OpticalModeTiming full;
  OpticalGrayscaleTiming grayscaleAfterFast;
  OpticalGrayscaleTiming grayscaleAfterHalf;
  OpticalReaderHalfTiming readerHalf;
  OpticalReaderFastTiming readerFast;
  OpticalReaderImageFastTiming readerImageFast;
  uint64_t fullPulseIntervalUs = 0;
};

struct StorageTiming {
  uint64_t readOpenUs = 0;
  uint64_t writeOpenUs = 0;
  uint64_t directoryRootOpenUs = 0;
  uint64_t directoryNextUs = 0;
  uint64_t closeUs = 0;
  uint64_t transferBasisBytes = 0;
  uint64_t readTransferUs = 0;
  uint64_t writeTransferSetupUs = 0;
  uint64_t writeTransferUs = 0;
};

struct PrimaryRenderTiming {
  uint64_t beforePrimaryUs = 0;
  uint64_t primaryTransferUs = 0;
  uint64_t grayscaleLsbUs = 0;
  uint64_t grayscaleMsbUs = 0;
  uint64_t grayscaleTransferUs = 0;
  uint64_t cleanupUs = 0;
};

struct ImageDecodeTiming {
  uint64_t sourceBytes = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint64_t durationUs = 0;
};

struct ImagePreparationTiming {
  uint64_t sourceBytes = 0;
  uint64_t durationUs = 0;
};

struct RenderTiming {
  PrimaryRenderTiming fast;
  PrimaryRenderTiming half;
  uint64_t cachedSmallImageUs = 0;
  uint64_t cachedLargeImageUs = 0;
  uint64_t decodeImageUs = 0;
  uint64_t decodeMediumImageUs = 0;
  uint64_t decodeLargeImageUs = 0;
  uint64_t decodeSmallMaxBytes = 0;
  uint64_t decodeMediumMaxBytes = 0;
  uint64_t prepareSmallImageUs = 0;
  uint64_t prepareMediumImageUs = 0;
  uint64_t prepareLargeImageUs = 0;
  uint64_t sectionStreamingUs = 0;
  uint64_t sectionImageDiscoveryUs = 0;
  std::vector<ImageDecodeTiming> exactImageDecode;
  std::vector<ImagePreparationTiming> exactImagePreparation;
};

struct IndexingWorkloadTiming {
  std::string path;
  uint64_t totalUs = 0;
  uint64_t opfUs = 0;
  uint64_t tocUs = 0;
  uint64_t bookBinUs = 0;
  uint64_t postIndexLoadUs = 0;
};

struct WarmWorkloadTiming {
  std::string path;
  uint64_t cachedMetadataLoadUs = 0;
  uint64_t metadataStartToPageLoadUs = 0;
};

struct PngThumbnailTiming {
  uint64_t preparationInterceptUs = 0;
  uint64_t preparationNanosecondsPerImageByte = 0;
  uint64_t conversionInterceptUs = 0;
  uint64_t conversionNanosecondsPerSourcePixel = 0;
  uint64_t minImageBytes = 0;
  uint64_t maxImageBytes = 0;
  uint32_t minSourcePixels = 0;
  uint32_t maxSourcePixels = 0;
  uint16_t targetWidth = 0;
  uint16_t targetHeight = 0;
  uint8_t bitDepth = 0;
  uint8_t colorType = 0;
};

struct WorkloadTiming {
  uint64_t coldIndexingUs = 0;
  uint64_t coldOpfUs = 0;
  uint64_t coldTocUs = 0;
  uint64_t coldBookBinUs = 0;
  uint64_t coldPostIndexLoadUs = 0;
  uint64_t cachedMetadataLoadUs = 0;
  uint64_t warmReadyFromMetadataStartUs = 0;
  uint64_t jpegThumbnailInterceptUs = 0;
  uint64_t jpegThumbnailNanosecondsPerByte = 0;
  uint64_t sleepEntryToDeepSleepUs = 0;
  uint64_t fileBrowserActivityToDisplayUs = 0;
  uint64_t fileBrowserRenderUs = 0;
  uint64_t homeFirstRenderUs = 0;
  uint64_t homeSubsequentRenderUs = 0;
  uint64_t settingsFirstRenderUs = 0;
  uint64_t settingsSubsequentRenderUs = 0;
  uint64_t settingsPopupRenderUs = 0;
  uint64_t readerMenuFirstRenderUs = 0;
  uint64_t readerMenuSubsequentRenderUs = 0;
  uint64_t readerMenuPopupRenderUs = 0;
  uint64_t readerPercentFirstRenderUs = 0;
  uint64_t readerPercentSubsequentRenderUs = 0;
  uint64_t cacheClearNoCacheUs = 0;
  uint64_t cacheClearInterceptUs = 0;
  uint64_t cacheClearPerFileUs = 0;
  std::vector<IndexingWorkloadTiming> exactIndexing;
  std::vector<WarmWorkloadTiming> exactWarm;
  PngThumbnailTiming pngThumbnail;
};

struct TimingProfile {
  std::string id = "development-uncalibrated-v0";
  bool calibrated = false;
  PanelTiming panel;
  OpticalTiming optical;
  StorageTiming storage;
  RenderTiming render;
  WorkloadTiming workload;
  std::optional<std::filesystem::path> source;
};

struct Configuration {
  DeviceProfile profile;
  std::filesystem::path artifactDirectory;
  std::optional<std::filesystem::path> sdFixtureDirectory;
  std::string rtcStart = "2000-01-01T00:00:00Z";
  uint64_t randomSeed = 0;
  std::string initialPanel = "white";
  std::optional<std::filesystem::path> initialPanelPng;
  TimingProfile timing;
};

const DeviceProfile& profileFor(Device device);
std::optional<Configuration> parseConfiguration(int argc, char** argv, std::string& error);

}  // namespace emulator
