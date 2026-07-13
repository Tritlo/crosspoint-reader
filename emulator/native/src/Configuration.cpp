#include "emulator/Configuration.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <string_view>
#include <utility>

namespace emulator {
namespace {

constexpr DeviceProfile X3_PROFILE{Device::X3, "x3", 792, 528, "uc8253", 90};
constexpr DeviceProfile X4_PROFILE{Device::X4, "x4", 800, 480, "ssd1677", 90};

bool parseUnsigned(std::string_view value, uint64_t& result) {
  const auto conversion = std::from_chars(value.data(), value.data() + value.size(), result);
  return conversion.ec == std::errc{} && conversion.ptr == value.data() + value.size();
}

bool p50Microseconds(const JsonVariantConst& value, uint64_t scale, uint64_t& result) {
  const JsonVariantConst p50 = value["p50"];
  if (!p50.is<double>()) return false;
  const double scaled = p50.as<double>() * static_cast<double>(scale);
  if (!std::isfinite(scaled) || scaled < 0.0) return false;
  result = static_cast<uint64_t>(std::llround(scaled));
  return true;
}

bool milliseconds(const JsonVariantConst& value, uint64_t& result) {
  if (!value.is<uint64_t>()) return false;
  const uint64_t milliseconds = value.as<uint64_t>();
  if (milliseconds > std::numeric_limits<uint64_t>::max() / 1000) return false;
  result = milliseconds * 1000;
  return true;
}

bool loadOpticalMode(const JsonObjectConst& mode, OpticalModeTiming& timing) {
  return p50Microseconds(mode["byTarget"]["black"]["interiorChangeStartMs"], 1000, timing.black.onsetUs) &&
         p50Microseconds(mode["byTarget"]["black"]["interiorChangeEndMs"], 1000, timing.black.endUs) &&
         p50Microseconds(mode["byTarget"]["white"]["interiorChangeStartMs"], 1000, timing.white.onsetUs) &&
         p50Microseconds(mode["byTarget"]["white"]["interiorChangeEndMs"], 1000, timing.white.endUs) &&
         timing.black.endUs >= timing.black.onsetUs && timing.white.endUs >= timing.white.onsetUs;
}

bool loadOpticalGrayscale(const JsonObjectConst& primary, OpticalGrayscaleTiming& timing) {
  const JsonVariantConst detected = primary["visibleChangeDetected"];
  if (detected.is<bool>() && !detected.as<bool>()) {
    timing.visibleChangeDetected = false;
    return primary["byTarget"].as<JsonObjectConst>().size() == 0;
  }
  timing.visibleChangeDetected = true;
  return p50Microseconds(primary["byTarget"]["light"]["visibleChangeStartMs"], 1000, timing.light.onsetUs) &&
         p50Microseconds(primary["byTarget"]["light"]["visibleChangeEndMs"], 1000, timing.light.endUs) &&
         p50Microseconds(primary["byTarget"]["dark"]["visibleChangeStartMs"], 1000, timing.dark.onsetUs) &&
         p50Microseconds(primary["byTarget"]["dark"]["visibleChangeEndMs"], 1000, timing.dark.endUs) &&
         timing.light.endUs >= timing.light.onsetUs && timing.dark.endUs >= timing.dark.onsetUs;
}

bool loadPrimaryRenderTiming(const JsonObjectConst& source, uint64_t primaryBusyUs, uint64_t grayscaleBusyUs,
                             PrimaryRenderTiming& timing) {
  uint64_t prewarmUs = 0;
  uint64_t bwRenderUs = 0;
  uint64_t displayUs = 0;
  uint64_t grayscaleDisplayUs = 0;
  if (!p50Microseconds(source["prewarm"], 1000, prewarmUs) || !p50Microseconds(source["bwRender"], 1000, bwRenderUs) ||
      !p50Microseconds(source["display"], 1000, displayUs) ||
      !p50Microseconds(source["grayLsb"], 1000, timing.grayscaleLsbUs) ||
      !p50Microseconds(source["grayMsb"], 1000, timing.grayscaleMsbUs) ||
      !p50Microseconds(source["grayDisplay"], 1000, grayscaleDisplayUs) ||
      !p50Microseconds(source["cleanup"], 1000, timing.cleanupUs) || displayUs < primaryBusyUs ||
      grayscaleDisplayUs < grayscaleBusyUs) {
    return false;
  }
  timing.beforePrimaryUs = prewarmUs + bwRenderUs;
  timing.primaryTransferUs = displayUs - primaryBusyUs;
  timing.grayscaleTransferUs = grayscaleDisplayUs - grayscaleBusyUs;
  return true;
}

std::optional<TimingProfile> loadTimingProfile(const std::filesystem::path& path, Device device, std::string& error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "failed to open timing profile: " + path.string();
    return std::nullopt;
  }
  JsonDocument document;
  const DeserializationError parseError = deserializeJson(document, input);
  if (parseError) {
    error = "failed to parse timing profile " + path.string() + ": " + parseError.c_str();
    return std::nullopt;
  }

  const char* id = document["id"] | "";
  const char* deviceProfile = document["deviceProfile"] | "";
  const char* expectedDevice = profileFor(device).id;
  if ((document["schemaVersion"] | 0) != 1 || id[0] == '\0') {
    error = "timing profile must have schemaVersion 1 and a non-empty id";
    return std::nullopt;
  }
  if (std::string_view(deviceProfile) != expectedDevice) {
    error = "timing profile deviceProfile must match --device " + std::string(expectedDevice);
    return std::nullopt;
  }

  TimingProfile profile;
  profile.id = id;
  profile.calibrated = true;
  profile.source = std::filesystem::absolute(path);
  const JsonObjectConst panel = document["panel"];
  const JsonObjectConst optical = document["opticalWaveform"]["modes"];
  const JsonObjectConst opticalGrayscale = document["opticalWaveform"]["grayscale"]["primaries"];
  const JsonObjectConst opticalReaderHalf = document["opticalWaveform"]["readerHalf"];
  const JsonObjectConst opticalReaderFast = document["opticalWaveform"]["readerFast"];
  const JsonObjectConst opticalReaderImageFast = document["opticalWaveform"]["readerImageFast"];
  const JsonObjectConst storage = document["storage"];
  const JsonObjectConst directory = document["directory"];
  const JsonObjectConst fastRender = document["renderMs"]["fastPrimary"];
  const JsonObjectConst halfRender = document["renderMs"]["halfPrimary"];
  const JsonObjectConst jpegThumbnailModel = document["models"]["jpegThumbnail"];
  const JsonObjectConst pngThumbnailModel = document["models"]["pngThumbnail"];
  JsonObjectConst imageTimings = document["models"]["imageFallback"];
  const JsonArrayConst exactImageDecodeModel = document["models"]["exactImageDecode"];
  const JsonArrayConst exactImagePreparationModel = document["models"]["exactImagePreparation"];
  const JsonArrayConst exactIndexingModel = document["models"]["exactIndexingByPath"];
  const JsonArrayConst exactWarmModel = document["models"]["exactWarmByPath"];
  const JsonObjectConst sectionStreamingModel = document["models"]["sectionStreamingMs"];
  const JsonObjectConst sectionImageDiscoveryModel = document["models"]["sectionImageDiscoveryMs"];
  const JsonObjectConst fileBrowserActivityModel = document["models"]["fileBrowserActivityToDisplayMs"];
  const JsonObjectConst fileBrowserRenderModel = document["models"]["fileBrowserRenderMs"];
  const JsonObjectConst homeFirstRenderModel = document["models"]["homeFirstRenderMs"];
  const JsonObjectConst homeSubsequentRenderModel = document["models"]["homeSubsequentRenderMs"];
  const JsonObjectConst settingsFirstRenderModel = document["models"]["settingsFirstRenderMs"];
  const JsonObjectConst settingsSubsequentRenderModel = document["models"]["settingsSubsequentRenderMs"];
  const JsonObjectConst settingsPopupRenderModel = document["models"]["settingsPopupRenderMs"];
  const JsonObjectConst readerMenuFirstRenderModel = document["models"]["readerMenuFirstRenderMs"];
  const JsonObjectConst readerMenuSubsequentRenderModel = document["models"]["readerMenuSubsequentRenderMs"];
  const JsonObjectConst readerMenuPopupRenderModel = document["models"]["readerMenuPopupRenderMs"];
  const JsonObjectConst readerPercentFirstRenderModel = document["models"]["readerPercentFirstRenderMs"];
  const JsonObjectConst readerPercentSubsequentRenderModel = document["models"]["readerPercentSubsequentRenderMs"];
  const JsonObjectConst cacheClearModel = document["models"]["cacheClear"];
  const JsonObjectConst populatedCacheClearModel = cacheClearModel["populated"];
  constexpr uint64_t STORAGE_BASIS = 524288;
  constexpr uint64_t SMALL_STORAGE_BASIS = 4096;
  profile.storage.transferBasisBytes = STORAGE_BASIS;
  uint64_t smallWriteTransferUs = 0;
  JsonObjectConst indexingTimings;
  JsonObjectConst warmTimings;
  JsonObjectConst sleepTimings;
  for (JsonPairConst workload : document["workloads"].as<JsonObjectConst>()) {
    if (imageTimings.isNull()) imageTimings = workload.value()["images"];
    if (indexingTimings.isNull()) indexingTimings = workload.value()["indexing"];
    if (!workload.value()["cachedMetadataLoadMs"].isNull()) warmTimings = workload.value();
    if (!workload.value()["sleep"].isNull()) sleepTimings = workload.value()["sleep"];
  }
  if (!p50Microseconds(panel["fast"]["controllerBusyMs"], 1000, profile.panel.fastBusyUs) ||
      !p50Microseconds(panel["half"]["controllerBusyMs"], 1000, profile.panel.halfBusyUs) ||
      !p50Microseconds(panel["full"]["controllerBusyMs"], 1000, profile.panel.fullBusyUs) ||
      !p50Microseconds(panel["full"]["operationUs"], 1, profile.panel.fullOperationUs) ||
      profile.panel.fullOperationUs < profile.panel.fullBusyUs ||
      !p50Microseconds(fastRender["grayscaleBusy"], 1000, profile.panel.grayscaleAfterFastBusyUs) ||
      !p50Microseconds(halfRender["grayscaleBusy"], 1000, profile.panel.grayscaleAfterHalfBusyUs) ||
      !loadPrimaryRenderTiming(fastRender, profile.panel.fastBusyUs, profile.panel.grayscaleAfterFastBusyUs,
                               profile.render.fast) ||
      !loadPrimaryRenderTiming(halfRender, profile.panel.halfBusyUs, profile.panel.grayscaleAfterHalfBusyUs,
                               profile.render.half) ||
      !p50Microseconds(imageTimings["cachedRenderSmallMs"], 1000, profile.render.cachedSmallImageUs) ||
      !p50Microseconds(imageTimings["cachedRenderLargeMs"], 1000, profile.render.cachedLargeImageUs) ||
      !p50Microseconds(imageTimings["decodeAndCacheSmallMs"], 1000, profile.render.decodeImageUs) ||
      !p50Microseconds(imageTimings["decodeAndCacheMediumMs"], 1000, profile.render.decodeMediumImageUs) ||
      !p50Microseconds(imageTimings["decodeAndCacheLargeMs"], 1000, profile.render.decodeLargeImageUs) ||
      !p50Microseconds(imageTimings["prepareSmallMs"], 1000, profile.render.prepareSmallImageUs) ||
      !p50Microseconds(imageTimings["prepareMediumMs"], 1000, profile.render.prepareMediumImageUs) ||
      !p50Microseconds(imageTimings["prepareLargeMs"], 1000, profile.render.prepareLargeImageUs) ||
      !p50Microseconds(sectionStreamingModel, 1000, profile.render.sectionStreamingUs) ||
      !p50Microseconds(sectionImageDiscoveryModel, 1000, profile.render.sectionImageDiscoveryUs) ||
      !p50Microseconds(fileBrowserActivityModel, 1000, profile.workload.fileBrowserActivityToDisplayUs) ||
      !p50Microseconds(cacheClearModel["noCacheUs"], 1, profile.workload.cacheClearNoCacheUs) ||
      !populatedCacheClearModel["interceptUs"].is<uint64_t>() ||
      !populatedCacheClearModel["perFileUs"].is<uint64_t>() ||
      !p50Microseconds(warmTimings["cachedMetadataLoadMs"], 1000, profile.workload.cachedMetadataLoadUs) ||
      !p50Microseconds(storage["524288"]["read"]["openUs"], 1, profile.storage.readOpenUs) ||
      !p50Microseconds(storage["524288"]["write"]["openUs"], 1, profile.storage.writeOpenUs) ||
      !p50Microseconds(directory["rootOpenUs"], 1, profile.storage.directoryRootOpenUs) ||
      !p50Microseconds(directory["perNextUs"], 1, profile.storage.directoryNextUs) ||
      !p50Microseconds(storage["524288"]["read"]["closeUs"], 1, profile.storage.closeUs) ||
      !p50Microseconds(storage["524288"]["read"]["ioUs"], 1, profile.storage.readTransferUs) ||
      !p50Microseconds(storage["524288"]["write"]["ioUs"], 1, profile.storage.writeTransferUs) ||
      !p50Microseconds(storage["4096"]["write"]["ioUs"], 1, smallWriteTransferUs) ||
      !loadOpticalMode(optical["full"], profile.optical.full) ||
      !loadOpticalGrayscale(opticalGrayscale["fast"], profile.optical.grayscaleAfterFast) ||
      !loadOpticalGrayscale(opticalGrayscale["half"], profile.optical.grayscaleAfterHalf) ||
      !p50Microseconds(opticalReaderHalf["invertedTargetStartMs"], 1000, profile.optical.readerHalf.invertedTargetUs) ||
      !p50Microseconds(opticalReaderHalf["targetSettleMs"], 1000, profile.optical.readerHalf.settledTargetUs) ||
      profile.optical.readerHalf.settledTargetUs < profile.optical.readerHalf.invertedTargetUs ||
      !p50Microseconds(opticalReaderFast["transitionStartMs"], 1000, profile.optical.readerFast.transitionStartUs) ||
      !p50Microseconds(opticalReaderFast["transitionEndMs"], 1000, profile.optical.readerFast.transitionEndUs) ||
      profile.optical.readerFast.transitionEndUs < profile.optical.readerFast.transitionStartUs ||
      !p50Microseconds(optical["full"]["pulseIntervalMs"], 1000, profile.optical.fullPulseIntervalUs) ||
      profile.optical.fullPulseIntervalUs == 0) {
    error = "timing profile is missing required X4 panel/optical/render/storage/workload p50 measurements";
    return std::nullopt;
  }
  if ((!homeFirstRenderModel.isNull() &&
       !p50Microseconds(homeFirstRenderModel, 1000, profile.workload.homeFirstRenderUs)) ||
      (!homeSubsequentRenderModel.isNull() &&
       !p50Microseconds(homeSubsequentRenderModel, 1000, profile.workload.homeSubsequentRenderUs)) ||
      (!fileBrowserRenderModel.isNull() &&
       !p50Microseconds(fileBrowserRenderModel, 1000, profile.workload.fileBrowserRenderUs)) ||
      (!settingsFirstRenderModel.isNull() &&
       !p50Microseconds(settingsFirstRenderModel, 1000, profile.workload.settingsFirstRenderUs)) ||
      (!settingsSubsequentRenderModel.isNull() &&
       !p50Microseconds(settingsSubsequentRenderModel, 1000, profile.workload.settingsSubsequentRenderUs)) ||
      (!settingsPopupRenderModel.isNull() &&
       !p50Microseconds(settingsPopupRenderModel, 1000, profile.workload.settingsPopupRenderUs)) ||
      (!readerMenuFirstRenderModel.isNull() &&
       !p50Microseconds(readerMenuFirstRenderModel, 1000, profile.workload.readerMenuFirstRenderUs)) ||
      (!readerMenuSubsequentRenderModel.isNull() &&
       !p50Microseconds(readerMenuSubsequentRenderModel, 1000, profile.workload.readerMenuSubsequentRenderUs)) ||
      (!readerMenuPopupRenderModel.isNull() &&
       !p50Microseconds(readerMenuPopupRenderModel, 1000, profile.workload.readerMenuPopupRenderUs)) ||
      (!readerPercentFirstRenderModel.isNull() &&
       !p50Microseconds(readerPercentFirstRenderModel, 1000, profile.workload.readerPercentFirstRenderUs)) ||
      (!readerPercentSubsequentRenderModel.isNull() &&
       !p50Microseconds(readerPercentSubsequentRenderModel, 1000, profile.workload.readerPercentSubsequentRenderUs)) ||
      profile.workload.fileBrowserRenderUs > profile.workload.fileBrowserActivityToDisplayUs) {
    error = "timing profile contains an invalid activity render model";
    return std::nullopt;
  }
  if (opticalReaderImageFast.isNull()) {
    profile.optical.readerImageFast.timing = profile.optical.readerFast;
    profile.optical.readerImageFast.darkPixelPercentMin = 101;
  } else {
    const int imageDarkPixelPercentMin = opticalReaderImageFast["targetDarkPixelPercentMin"] | 0;
    if (!p50Microseconds(opticalReaderImageFast["transitionStartMs"], 1000,
                         profile.optical.readerImageFast.timing.transitionStartUs) ||
        !p50Microseconds(opticalReaderImageFast["transitionEndMs"], 1000,
                         profile.optical.readerImageFast.timing.transitionEndUs) ||
        profile.optical.readerImageFast.timing.transitionEndUs <
            profile.optical.readerImageFast.timing.transitionStartUs ||
        imageDarkPixelPercentMin < 1 || imageDarkPixelPercentMin > 100) {
      error = "timing profile contains an invalid reader image-fast waveform";
      return std::nullopt;
    }
    profile.optical.readerImageFast.darkPixelPercentMin = static_cast<uint8_t>(imageDarkPixelPercentMin);
  }
  profile.workload.cacheClearInterceptUs = populatedCacheClearModel["interceptUs"].as<uint64_t>();
  profile.workload.cacheClearPerFileUs = populatedCacheClearModel["perFileUs"].as<uint64_t>();
  if (profile.workload.cacheClearNoCacheUs == 0 || profile.workload.cacheClearInterceptUs == 0 ||
      profile.workload.cacheClearPerFileUs == 0) {
    error = "timing profile contains an invalid cache-clear model";
    return std::nullopt;
  }
  const long double writeSetupUs = (static_cast<long double>(smallWriteTransferUs) * STORAGE_BASIS -
                                    static_cast<long double>(profile.storage.writeTransferUs) * SMALL_STORAGE_BASIS) /
                                   (STORAGE_BASIS - SMALL_STORAGE_BASIS);
  if (writeSetupUs < 0.0L || writeSetupUs > static_cast<long double>(smallWriteTransferUs)) {
    error = "timing profile contains an invalid first-write transfer model";
    return std::nullopt;
  }
  profile.storage.writeTransferSetupUs = static_cast<uint64_t>(std::llround(writeSetupUs));
  profile.storage.writeTransferUs -= profile.storage.writeTransferSetupUs;
  const JsonVariantConst thumbnailInterceptUs = jpegThumbnailModel["interceptUs"];
  const JsonVariantConst thumbnailNanosecondsPerByte = jpegThumbnailModel["nanosecondsPerImageByte"];
  if (!thumbnailInterceptUs.is<uint64_t>() || !thumbnailNanosecondsPerByte.is<uint64_t>() ||
      thumbnailNanosecondsPerByte.as<uint64_t>() == 0) {
    error = "timing profile is missing the JPEG thumbnail scaling model";
    return std::nullopt;
  }
  profile.workload.jpegThumbnailInterceptUs = thumbnailInterceptUs.as<uint64_t>();
  profile.workload.jpegThumbnailNanosecondsPerByte = thumbnailNanosecondsPerByte.as<uint64_t>();
  const JsonVariantConst pngPreparationInterceptUs = pngThumbnailModel["preparationInterceptUs"];
  const JsonVariantConst pngPreparationNanosecondsPerByte = pngThumbnailModel["preparationNanosecondsPerImageByte"];
  const JsonVariantConst pngConversionInterceptUs = pngThumbnailModel["conversionInterceptUs"];
  const JsonVariantConst pngConversionNanosecondsPerPixel = pngThumbnailModel["conversionNanosecondsPerSourcePixel"];
  const JsonVariantConst pngMinImageBytes = pngThumbnailModel["minImageBytes"];
  const JsonVariantConst pngMaxImageBytes = pngThumbnailModel["maxImageBytes"];
  const JsonVariantConst pngMinPixels = pngThumbnailModel["minSourcePixels"];
  const JsonVariantConst pngMaxPixels = pngThumbnailModel["maxSourcePixels"];
  const JsonVariantConst pngTargetWidth = pngThumbnailModel["targetWidth"];
  const JsonVariantConst pngTargetHeight = pngThumbnailModel["targetHeight"];
  const JsonVariantConst pngBitDepth = pngThumbnailModel["bitDepth"];
  const JsonVariantConst pngColorType = pngThumbnailModel["colorType"];
  if (!pngPreparationInterceptUs.is<uint64_t>() || !pngPreparationNanosecondsPerByte.is<uint64_t>() ||
      !pngConversionInterceptUs.is<uint64_t>() || !pngConversionNanosecondsPerPixel.is<uint64_t>() ||
      !pngMinImageBytes.is<uint64_t>() || !pngMaxImageBytes.is<uint64_t>() || !pngMinPixels.is<uint64_t>() ||
      !pngMaxPixels.is<uint64_t>() || !pngTargetWidth.is<uint64_t>() || !pngTargetHeight.is<uint64_t>() ||
      !pngBitDepth.is<uint64_t>() || !pngColorType.is<uint64_t>() ||
      pngPreparationNanosecondsPerByte.as<uint64_t>() == 0 || pngConversionNanosecondsPerPixel.as<uint64_t>() == 0 ||
      pngMinImageBytes.as<uint64_t>() == 0 || pngMaxImageBytes.as<uint64_t>() < pngMinImageBytes.as<uint64_t>() ||
      pngMinPixels.as<uint64_t>() == 0 || pngMaxPixels.as<uint64_t>() < pngMinPixels.as<uint64_t>() ||
      pngMaxPixels.as<uint64_t>() > std::numeric_limits<uint32_t>::max() || pngTargetWidth.as<uint64_t>() == 0 ||
      pngTargetWidth.as<uint64_t>() > std::numeric_limits<uint16_t>::max() || pngTargetHeight.as<uint64_t>() == 0 ||
      pngTargetHeight.as<uint64_t>() > std::numeric_limits<uint16_t>::max() ||
      pngBitDepth.as<uint64_t>() > std::numeric_limits<uint8_t>::max() ||
      pngColorType.as<uint64_t>() > std::numeric_limits<uint8_t>::max()) {
    error = "timing profile contains an invalid PNG thumbnail scaling model";
    return std::nullopt;
  }
  profile.workload.pngThumbnail.preparationInterceptUs = pngPreparationInterceptUs.as<uint64_t>();
  profile.workload.pngThumbnail.preparationNanosecondsPerImageByte = pngPreparationNanosecondsPerByte.as<uint64_t>();
  profile.workload.pngThumbnail.conversionInterceptUs = pngConversionInterceptUs.as<uint64_t>();
  profile.workload.pngThumbnail.conversionNanosecondsPerSourcePixel = pngConversionNanosecondsPerPixel.as<uint64_t>();
  profile.workload.pngThumbnail.minImageBytes = pngMinImageBytes.as<uint64_t>();
  profile.workload.pngThumbnail.maxImageBytes = pngMaxImageBytes.as<uint64_t>();
  profile.workload.pngThumbnail.minSourcePixels = static_cast<uint32_t>(pngMinPixels.as<uint64_t>());
  profile.workload.pngThumbnail.maxSourcePixels = static_cast<uint32_t>(pngMaxPixels.as<uint64_t>());
  profile.workload.pngThumbnail.targetWidth = static_cast<uint16_t>(pngTargetWidth.as<uint64_t>());
  profile.workload.pngThumbnail.targetHeight = static_cast<uint16_t>(pngTargetHeight.as<uint64_t>());
  profile.workload.pngThumbnail.bitDepth = static_cast<uint8_t>(pngBitDepth.as<uint64_t>());
  profile.workload.pngThumbnail.colorType = static_cast<uint8_t>(pngColorType.as<uint64_t>());
  const JsonVariantConst sleepEntryToDeepSleepMs = sleepTimings["activityToDeepSleepMs"];
  if (!sleepEntryToDeepSleepMs.is<uint64_t>()) {
    error = "timing profile is missing the sleep-entry workload boundary";
    return std::nullopt;
  }
  profile.workload.sleepEntryToDeepSleepUs = sleepEntryToDeepSleepMs.as<uint64_t>() * 1000;
  const JsonVariantConst coldIndexingMs = indexingTimings["totalIndexingMs"];
  const JsonVariantConst coldOpfMs = indexingTimings["opfPassMs"];
  const JsonVariantConst coldTocMs = indexingTimings["tocPassMs"];
  const JsonVariantConst coldBookBinMs = indexingTimings["bookBinMs"];
  const JsonVariantConst coldPostIndexLoadMs = indexingTimings["postIndexLoadMs"];
  if (!coldIndexingMs.is<uint64_t>() || !coldOpfMs.is<uint64_t>() || !coldTocMs.is<uint64_t>() ||
      !coldBookBinMs.is<uint64_t>() || !coldPostIndexLoadMs.is<uint64_t>()) {
    error = "timing profile is missing cold-indexing workload phases";
    return std::nullopt;
  }
  profile.workload.coldIndexingUs = coldIndexingMs.as<uint64_t>() * 1000;
  profile.workload.coldOpfUs = coldOpfMs.as<uint64_t>() * 1000;
  profile.workload.coldTocUs = coldTocMs.as<uint64_t>() * 1000;
  profile.workload.coldBookBinUs = coldBookBinMs.as<uint64_t>() * 1000;
  profile.workload.coldPostIndexLoadUs = coldPostIndexLoadMs.as<uint64_t>() * 1000;
  if (exactIndexingModel.isNull() || exactIndexingModel.size() == 0) {
    error = "timing profile is missing exact indexing workloads";
    return std::nullopt;
  }
  for (JsonObjectConst value : exactIndexingModel) {
    const char* pathValue = value["path"] | "";
    IndexingWorkloadTiming exact;
    exact.path = pathValue;
    if (exact.path.empty() || exact.path.front() != '/' ||
        std::any_of(profile.workload.exactIndexing.begin(), profile.workload.exactIndexing.end(),
                    [&exact](const auto& existing) { return existing.path == exact.path; }) ||
        !milliseconds(value["totalIndexingMs"], exact.totalUs) || !milliseconds(value["opfPassMs"], exact.opfUs) ||
        !milliseconds(value["tocPassMs"], exact.tocUs) || !milliseconds(value["bookBinMs"], exact.bookBinUs) ||
        !milliseconds(value["postIndexLoadMs"], exact.postIndexLoadUs)) {
      error = "timing profile contains an invalid exact indexing workload";
      return std::nullopt;
    }
    profile.workload.exactIndexing.push_back(std::move(exact));
  }
  if (exactWarmModel.isNull() || exactWarmModel.size() == 0) {
    error = "timing profile is missing exact warm-open workloads";
    return std::nullopt;
  }
  for (JsonObjectConst value : exactWarmModel) {
    const char* pathValue = value["path"] | "";
    WarmWorkloadTiming exact;
    exact.path = pathValue;
    if (exact.path.empty() || exact.path.front() != '/' ||
        std::any_of(profile.workload.exactWarm.begin(), profile.workload.exactWarm.end(),
                    [&exact](const auto& existing) { return existing.path == exact.path; }) ||
        !p50Microseconds(value["cachedMetadataLoadMs"], 1000, exact.cachedMetadataLoadUs) ||
        !p50Microseconds(value["metadataStartToPageLoadMs"], 1000, exact.metadataStartToPageLoadUs)) {
      error = "timing profile contains an invalid exact warm-open workload";
      return std::nullopt;
    }
    profile.workload.exactWarm.push_back(std::move(exact));
  }
  const JsonVariantConst warmReadyMs = warmTimings["loads"]["warmMetadataStartToReadyMs"];
  if (!warmReadyMs.is<uint64_t>()) {
    error = "timing profile is missing the quiescent warm-open phase duration";
    return std::nullopt;
  }
  profile.workload.warmReadyFromMetadataStartUs = warmReadyMs.as<uint64_t>() * 1000;
  const JsonVariantConst decodeSmallMax = imageTimings["decodeSourceBytes"]["smallMax"];
  const JsonVariantConst decodeMediumMax = imageTimings["decodeSourceBytes"]["mediumMax"];
  if (!decodeSmallMax.is<uint64_t>() || !decodeMediumMax.is<uint64_t>() || decodeSmallMax.as<uint64_t>() == 0 ||
      decodeMediumMax.as<uint64_t>() <= decodeSmallMax.as<uint64_t>()) {
    error = "timing profile is missing ordered image decode source-byte thresholds";
    return std::nullopt;
  }
  profile.render.decodeSmallMaxBytes = decodeSmallMax.as<uint64_t>();
  profile.render.decodeMediumMaxBytes = decodeMediumMax.as<uint64_t>();
  if (exactImageDecodeModel.isNull() || exactImageDecodeModel.size() == 0) {
    error = "timing profile is missing exact image decode workloads";
    return std::nullopt;
  }
  for (JsonObjectConst value : exactImageDecodeModel) {
    const JsonVariantConst sourceBytes = value["sourceBytes"];
    const JsonVariantConst width = value["width"];
    const JsonVariantConst height = value["height"];
    ImageDecodeTiming exact;
    if (!sourceBytes.is<uint64_t>() || !width.is<uint16_t>() || !height.is<uint16_t>() ||
        sourceBytes.as<uint64_t>() == 0 || width.as<uint16_t>() == 0 || height.as<uint16_t>() == 0 ||
        !milliseconds(value["durationMs"], exact.durationUs)) {
      error = "timing profile contains an invalid exact image decode workload";
      return std::nullopt;
    }
    exact.sourceBytes = sourceBytes.as<uint64_t>();
    exact.width = width.as<uint16_t>();
    exact.height = height.as<uint16_t>();
    profile.render.exactImageDecode.push_back(exact);
  }
  if (exactImagePreparationModel.isNull() || exactImagePreparationModel.size() == 0) {
    error = "timing profile is missing exact image preparation workloads";
    return std::nullopt;
  }
  for (JsonObjectConst value : exactImagePreparationModel) {
    const JsonVariantConst sourceBytes = value["sourceBytes"];
    ImagePreparationTiming exact;
    if (!sourceBytes.is<uint64_t>() || sourceBytes.as<uint64_t>() == 0 ||
        !milliseconds(value["durationMs"], exact.durationUs)) {
      error = "timing profile contains an invalid exact image preparation workload";
      return std::nullopt;
    }
    exact.sourceBytes = sourceBytes.as<uint64_t>();
    profile.render.exactImagePreparation.push_back(exact);
  }
  profile.optical.measured = true;
  return profile;
}

}  // namespace

const DeviceProfile& profileFor(Device device) { return device == Device::X3 ? X3_PROFILE : X4_PROFILE; }

std::optional<Configuration> parseConfiguration(int argc, char** argv, std::string& error) {
  std::optional<Device> device;
  std::optional<std::filesystem::path> artifacts;
  std::optional<std::filesystem::path> sdFixture;
  std::string rtcStart = "2000-01-01T00:00:00Z";
  uint64_t randomSeed = 0;
  std::string initialPanel = "white";
  std::optional<std::filesystem::path> initialPanelPng;
  std::optional<std::filesystem::path> timingProfilePath;

  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument == "--device") {
      if (++i >= argc) {
        error = "--device requires x3 or x4";
        return std::nullopt;
      }
      const std::string_view value(argv[i]);
      if (value == "x3")
        device = Device::X3;
      else if (value == "x4")
        device = Device::X4;
      else {
        error = "--device must be x3 or x4";
        return std::nullopt;
      }
    } else if (argument == "--artifacts") {
      if (++i >= argc) {
        error = "--artifacts requires a directory";
        return std::nullopt;
      }
      artifacts = std::filesystem::path(argv[i]);
    } else if (argument == "--rtc-start") {
      if (++i >= argc) {
        error = "--rtc-start requires an ISO-8601 value";
        return std::nullopt;
      }
      rtcStart = argv[i];
    } else if (argument == "--sd") {
      if (++i >= argc) {
        error = "--sd requires a fixture directory";
        return std::nullopt;
      }
      sdFixture = std::filesystem::path(argv[i]);
    } else if (argument == "--seed") {
      if (++i >= argc || !parseUnsigned(argv[i], randomSeed)) {
        error = "--seed requires an unsigned integer";
        return std::nullopt;
      }
    } else if (argument == "--panel-initial") {
      if (++i >= argc || (std::string_view(argv[i]) != "white" && std::string_view(argv[i]) != "black")) {
        error = "--panel-initial must be white or black";
        return std::nullopt;
      }
      initialPanel = argv[i];
      initialPanelPng.reset();
    } else if (argument == "--panel-initial-png") {
      if (++i >= argc) {
        error = "--panel-initial-png requires a PNG path";
        return std::nullopt;
      }
      initialPanel = "png";
      initialPanelPng = std::filesystem::path(argv[i]);
    } else if (argument == "--timing-profile") {
      if (++i >= argc) {
        error = "--timing-profile requires a JSON path";
        return std::nullopt;
      }
      timingProfilePath = std::filesystem::path(argv[i]);
    } else {
      error = "unknown argument: " + std::string(argument);
      return std::nullopt;
    }
  }

  if (!device) {
    error = "--device is required";
    return std::nullopt;
  }
  if (!artifacts) {
    error = "--artifacts is required";
    return std::nullopt;
  }

  TimingProfile timing;
  if (timingProfilePath) {
    auto loaded = loadTimingProfile(*timingProfilePath, *device, error);
    if (!loaded) return std::nullopt;
    timing = std::move(*loaded);
  }

  return Configuration{profileFor(*device),        *artifacts,       sdFixture,
                       std::move(rtcStart),        randomSeed,       std::move(initialPanel),
                       std::move(initialPanelPng), std::move(timing)};
}

}  // namespace emulator
