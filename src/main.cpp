#include <Arduino.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <WiFi.h>
#include <builtinFonts/all.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <new>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;

// Fonts
EpdFont notoserif14RegularFont(&notoserif_14_regular);
EpdFont notoserif14BoldFont(&notoserif_14_bold);
EpdFont notoserif14ItalicFont(&notoserif_14_italic);
EpdFont notoserif14BoldItalicFont(&notoserif_14_bolditalic);
EpdFontFamily notoserif14FontFamily(&notoserif14RegularFont, &notoserif14BoldFont, &notoserif14ItalicFont,
                                    &notoserif14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notoserif12RegularFont(&notoserif_12_regular);
EpdFont notoserif12BoldFont(&notoserif_12_bold);
EpdFont notoserif12ItalicFont(&notoserif_12_italic);
EpdFont notoserif12BoldItalicFont(&notoserif_12_bolditalic);
EpdFontFamily notoserif12FontFamily(&notoserif12RegularFont, &notoserif12BoldFont, &notoserif12ItalicFont,
                                    &notoserif12BoldItalicFont);
EpdFont notoserif16RegularFont(&notoserif_16_regular);
EpdFont notoserif16BoldFont(&notoserif_16_bold);
EpdFont notoserif16ItalicFont(&notoserif_16_italic);
EpdFont notoserif16BoldItalicFont(&notoserif_16_bolditalic);
EpdFontFamily notoserif16FontFamily(&notoserif16RegularFont, &notoserif16BoldFont, &notoserif16ItalicFont,
                                    &notoserif16BoldItalicFont);
EpdFont notoserif18RegularFont(&notoserif_18_regular);
EpdFont notoserif18BoldFont(&notoserif_18_bold);
EpdFont notoserif18ItalicFont(&notoserif_18_italic);
EpdFont notoserif18BoldItalicFont(&notoserif_18_bolditalic);
EpdFontFamily notoserif18FontFamily(&notoserif18RegularFont, &notoserif18BoldFont, &notoserif18ItalicFont,
                                    &notoserif18BoldItalicFont);

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

#if defined(CROSSPOINT_CALIBRATION)
namespace {
constexpr uint8_t CALIBRATION_PROTOCOL_VERSION = 1;
constexpr uint64_t CALIBRATION_FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t CALIBRATION_FNV_PRIME = 1099511628211ULL;
bool calibrationActivity = false;

void beginCalibrationActivity() {
  calibrationActivity = true;
  powerManager.setPowerSaving(false);
}

int calibrationButtonIndex(const String& name) {
  if (name == "back") return HalGPIO::BTN_BACK;
  if (name == "confirm") return HalGPIO::BTN_CONFIRM;
  if (name == "left") return HalGPIO::BTN_LEFT;
  if (name == "right") return HalGPIO::BTN_RIGHT;
  if (name == "up") return HalGPIO::BTN_UP;
  if (name == "down") return HalGPIO::BTN_DOWN;
  if (name == "power") return HalGPIO::BTN_POWER;
  return -1;
}

bool validCalibrationMarker(const String& marker) {
  if (marker.length() == 0 || marker.length() > 64) return false;
  for (size_t index = 0; index < marker.length(); ++index) {
    const char value = marker[index];
    if (!(std::isalnum(static_cast<unsigned char>(value)) || value == '-' || value == '_' || value == '.'))
      return false;
  }
  return true;
}

void calibrationReply(const char* kind, const String& value) {
  logSerial.printf("CAL:%s:%s:%lu\n", kind, value.c_str(), millis());
}

struct CalibrationIoResult {
  uint32_t openUs = 0;
  uint32_t ioUs = 0;
  uint32_t closeUs = 0;
  uint32_t bytes = 0;
  bool ok = false;
};

bool parseCalibrationCount(const String& value, uint32_t minimum, uint32_t maximum, uint32_t& parsed) {
  if (value.length() == 0) return false;
  for (size_t index = 0; index < value.length(); ++index) {
    if (!std::isdigit(static_cast<unsigned char>(value[index]))) return false;
  }
  const unsigned long result = value.toInt();
  if (result < minimum || result > maximum) return false;
  parsed = static_cast<uint32_t>(result);
  return true;
}

bool validCalibrationEpubPath(const String& path) {
  if (!path.startsWith("/") || path.indexOf("..") >= 0 || path.length() > 240) return false;
  String lowercase = path;
  lowercase.toLowerCase();
  return lowercase.endsWith(".epub") && Storage.exists(path.c_str());
}

bool validCalibrationUploadPath(const String& path) {
  if (!path.startsWith("/") || path.indexOf("..") >= 0 || path.indexOf(':') >= 0 || path.length() > 240) return false;
  String lowercase = path;
  lowercase.toLowerCase();
  return lowercase.endsWith(".epub");
}

int calibrationHexNibble(char digit) {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  return -1;
}

bool parseCalibrationHash(const String& value, uint64_t& parsed) {
  if (value.length() != 16) return false;
  parsed = 0;
  for (size_t index = 0; index < value.length(); ++index) {
    const int nibble = calibrationHexNibble(value[index]);
    if (nibble < 0) return false;
    parsed = (parsed << 4) | static_cast<uint8_t>(nibble);
  }
  return true;
}

constexpr char CALIBRATION_UPLOAD_TEMP_PATH[] = "/.crosspoint/calibration-upload.tmp";

struct CalibrationUploadState {
  bool active = false;
  String path;
  uint32_t expectedBytes = 0;
  uint32_t receivedBytes = 0;
  uint64_t expectedHash = 0;
  uint64_t actualHash = CALIBRATION_FNV_OFFSET_BASIS;
  HalFile file;
};

CalibrationUploadState calibrationUpload;

void resetCalibrationUpload(bool removeTemporaryFile) {
  if (calibrationUpload.file) calibrationUpload.file.close();
  calibrationUpload = CalibrationUploadState{};
  if (removeTemporaryFile) Storage.remove(CALIBRATION_UPLOAD_TEMP_PATH);
}

void startCalibrationUpload(const String& path, uint32_t byteCount, uint64_t expectedHash) {
  resetCalibrationUpload(true);
  if (!Storage.ensureDirectoryExists("/.crosspoint")) {
    calibrationReply("ERROR", "upload-directory-failed");
    return;
  }
  if (!Storage.openFileForWrite("CAL", CALIBRATION_UPLOAD_TEMP_PATH, calibrationUpload.file)) {
    calibrationReply("ERROR", "upload-open-failed");
    return;
  }
  calibrationUpload.active = true;
  calibrationUpload.path = path;
  calibrationUpload.expectedBytes = byteCount;
  calibrationUpload.expectedHash = expectedHash;
  beginCalibrationActivity();
  logSerial.printf("CAL:UPLOAD:READY:%s:%lu:%016llx:%lu\n", path.c_str(), byteCount,
                   static_cast<unsigned long long>(expectedHash), millis());
}

void receiveCalibrationUploadChunk(uint32_t offset, const String& encoded) {
  constexpr size_t CHUNK_SIZE = 512;
  const size_t byteCount = encoded.length() / 2;
  if (!calibrationUpload.active || offset != calibrationUpload.receivedBytes || encoded.length() == 0 ||
      encoded.length() % 2 != 0 || byteCount > CHUNK_SIZE ||
      byteCount > calibrationUpload.expectedBytes - calibrationUpload.receivedBytes) {
    calibrationReply("ERROR", "invalid-upload-chunk");
    return;
  }
  beginCalibrationActivity();
  std::array<uint8_t, CHUNK_SIZE> buffer;
  for (size_t index = 0; index < byteCount; ++index) {
    const int high = calibrationHexNibble(encoded[index * 2]);
    const int low = calibrationHexNibble(encoded[index * 2 + 1]);
    if (high < 0 || low < 0) {
      calibrationReply("ERROR", "invalid-upload-chunk");
      return;
    }
    buffer[index] = static_cast<uint8_t>((high << 4) | low);
  }
  if (calibrationUpload.file.write(buffer.data(), byteCount) != byteCount) {
    resetCalibrationUpload(true);
    calibrationReply("ERROR", "upload-write-failed");
    return;
  }
  for (size_t index = 0; index < byteCount; ++index) {
    calibrationUpload.actualHash ^= buffer[index];
    calibrationUpload.actualHash *= CALIBRATION_FNV_PRIME;
  }
  calibrationUpload.receivedBytes += byteCount;
  if (calibrationUpload.receivedBytes < calibrationUpload.expectedBytes) {
    logSerial.printf("CAL:UPLOAD:CHUNK:ACK:%lu:%lu\n", calibrationUpload.receivedBytes, millis());
    return;
  }

  calibrationUpload.file.flush();
  const String path = calibrationUpload.path;
  const uint32_t expectedBytes = calibrationUpload.expectedBytes;
  const uint64_t actualHash = calibrationUpload.actualHash;
  const bool valid = calibrationUpload.file.close() && actualHash == calibrationUpload.expectedHash;
  calibrationUpload.active = false;
  if (!valid) {
    resetCalibrationUpload(true);
    calibrationReply("ERROR", "upload-validation-failed");
    return;
  }
  if ((Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) ||
      !Storage.rename(CALIBRATION_UPLOAD_TEMP_PATH, path.c_str())) {
    resetCalibrationUpload(true);
    calibrationReply("ERROR", "upload-commit-failed");
    return;
  }
  resetCalibrationUpload(false);
  logSerial.printf("CAL:UPLOADED:%s:%lu:%016llx:%lu\n", path.c_str(), expectedBytes,
                   static_cast<unsigned long long>(actualHash), millis());
}

struct CalibrationCachePopulation {
  uint32_t files = 0;
  uint32_t directories = 0;
  uint64_t bytes = 0;
};

bool measureCalibrationCacheDirectory(const String& path, CalibrationCachePopulation& population, uint8_t depth = 0) {
  if (depth > 16) return false;
  HalFile directory = Storage.open(path.c_str());
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return false;
  }
  ++population.directories;
  char name[128];
  for (HalFile entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    const size_t nameLength = entry.getName(name, sizeof(name));
    const bool isDirectory = entry.isDirectory();
    const uint64_t fileBytes = isDirectory ? 0 : entry.fileSize64();
    entry.close();
    if (nameLength == 0 || nameLength >= sizeof(name)) {
      directory.close();
      return false;
    }
    if (isDirectory) {
      String child = path;
      if (!child.endsWith("/")) child += "/";
      child += name;
      if (!measureCalibrationCacheDirectory(child, population, static_cast<uint8_t>(depth + 1))) {
        directory.close();
        return false;
      }
    } else {
      ++population.files;
      population.bytes += fileBytes;
    }
  }
  return directory.close();
}

void runCalibrationSdBenchmark(uint32_t byteCount, uint32_t iterations) {
  constexpr size_t CHUNK_SIZE = 4096;
  constexpr char DIRECTORY[] = "/.crosspoint";
  constexpr char PATH[] = "/.crosspoint/calibration-timing.bin";
  std::array<uint8_t, CHUNK_SIZE> buffer{};
  std::array<CalibrationIoResult, 32> writes{};
  std::array<CalibrationIoResult, 32> reads{};
  for (size_t index = 0; index < buffer.size(); ++index) buffer[index] = static_cast<uint8_t>(index * 37U + 11U);

  Storage.ensureDirectoryExists(DIRECTORY);
  for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
    CalibrationIoResult& writeResult = writes[iteration];
    HalFile writeFile;
    uint32_t startUs = micros();
    const bool openedForWrite = Storage.openFileForWrite("CAL", PATH, writeFile);
    writeResult.openUs = micros() - startUs;
    startUs = micros();
    size_t written = 0;
    while (openedForWrite && written < byteCount) {
      const size_t chunk = std::min(CHUNK_SIZE, static_cast<size_t>(byteCount - written));
      const size_t count = writeFile.write(buffer.data(), chunk);
      written += count;
      if (count != chunk) break;
    }
    if (openedForWrite) writeFile.flush();
    writeResult.ioUs = micros() - startUs;
    startUs = micros();
    const bool writeClosed = openedForWrite && writeFile.close();
    writeResult.closeUs = micros() - startUs;
    writeResult.bytes = written;
    writeResult.ok = openedForWrite && writeClosed && written == byteCount;

    CalibrationIoResult& readResult = reads[iteration];
    HalFile readFile;
    startUs = micros();
    const bool openedForRead = Storage.openFileForRead("CAL", PATH, readFile);
    readResult.openUs = micros() - startUs;
    startUs = micros();
    size_t read = 0;
    while (openedForRead && read < byteCount) {
      const size_t chunk = std::min(CHUNK_SIZE, static_cast<size_t>(byteCount - read));
      const int count = readFile.read(buffer.data(), chunk);
      if (count <= 0) break;
      read += static_cast<size_t>(count);
    }
    readResult.ioUs = micros() - startUs;
    startUs = micros();
    const bool readClosed = openedForRead && readFile.close();
    readResult.closeUs = micros() - startUs;
    readResult.bytes = read;
    readResult.ok = openedForRead && readClosed && read == byteCount;
  }
  Storage.remove(PATH);

  for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
    const CalibrationIoResult& writeResult = writes[iteration];
    logSerial.printf("CAL:BENCH:SD:WRITE:%lu:%lu:%lu:%lu:%lu:%d\n", iteration, writeResult.bytes, writeResult.openUs,
                     writeResult.ioUs, writeResult.closeUs, writeResult.ok);
    const CalibrationIoResult& readResult = reads[iteration];
    logSerial.printf("CAL:BENCH:SD:READ:%lu:%lu:%lu:%lu:%lu:%d\n", iteration, readResult.bytes, readResult.openUs,
                     readResult.ioUs, readResult.closeUs, readResult.ok);
  }
  logSerial.printf("CAL:BENCH-END:SD:%lu:%lu:%lu\n", byteCount, iterations, millis());
}

void runCalibrationDirectoryBenchmark(uint32_t iterations) {
  std::array<uint32_t, 10> entryCounts{};
  std::array<uint32_t, 10> rootOpenUs{};
  std::array<uint32_t, 10> enumerationUs{};
  std::array<uint32_t, 10> entryCloseUs{};
  std::array<uint32_t, 10> rootCloseUs{};
  std::array<uint32_t, 10> durationsUs{};
  std::array<bool, 10> successes{};
  for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
    const uint32_t totalStartUs = micros();
    uint32_t startUs = micros();
    HalFile root = Storage.open("/");
    rootOpenUs[iteration] = micros() - startUs;
    bool success = root && root.isDirectory();
    uint32_t entryCount = 0;
    if (success) {
      root.rewindDirectory();
      while (true) {
        startUs = micros();
        HalFile entry = root.openNextFile();
        enumerationUs[iteration] += micros() - startUs;
        if (!entry) break;
        ++entryCount;
        startUs = micros();
        success = entry.close() && success;
        entryCloseUs[iteration] += micros() - startUs;
      }
      startUs = micros();
      success = root.close() && success;
      rootCloseUs[iteration] = micros() - startUs;
    }
    entryCounts[iteration] = entryCount;
    durationsUs[iteration] = micros() - totalStartUs;
    successes[iteration] = success;
  }
  for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
    logSerial.printf("CAL:BENCH:DIR:%lu:%lu:%lu:%lu:%lu:%lu:%lu:%d\n", iteration, entryCounts[iteration],
                     rootOpenUs[iteration], enumerationUs[iteration], entryCloseUs[iteration], rootCloseUs[iteration],
                     durationsUs[iteration], successes[iteration]);
  }
  logSerial.printf("CAL:BENCH-END:DIR:%lu:%lu\n", iterations, millis());
}

void runCalibrationPanelBenchmark(const String& modeName, HalDisplay::RefreshMode mode, uint32_t iterations) {
  std::unique_ptr<uint8_t[]> savedBuffer(new (std::nothrow) uint8_t[display.getBufferSize()]);
  if (!savedBuffer) {
    calibrationReply("ERROR", "panel-buffer-allocation");
    return;
  }
  memcpy(savedBuffer.get(), display.getFrameBuffer(), display.getBufferSize());
  std::array<uint32_t, 10> durationsUs{};
  {
    RenderLock lock;
    for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
      display.clearScreen(iteration % 2 == 0 ? 0x00 : 0xFF);
      const uint32_t startUs = micros();
      display.refreshDisplay(mode);
      durationsUs[iteration] = micros() - startUs;
    }
    memcpy(display.getFrameBuffer(), savedBuffer.get(), display.getBufferSize());
    display.refreshDisplay(HalDisplay::HALF_REFRESH);
  }
  for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
    logSerial.printf("CAL:BENCH:PANEL:%s:%lu:%s:%lu\n", modeName.c_str(), iteration,
                     iteration % 2 == 0 ? "black" : "white", durationsUs[iteration]);
  }
  logSerial.printf("CAL:BENCH-END:PANEL:%s:%lu:%lu\n", modeName.c_str(), iterations, millis());
}

void runCalibrationGrayscaleBenchmark(const String& primaryName, HalDisplay::RefreshMode primaryMode,
                                      uint32_t iterations) {
  const size_t bufferSize = display.getBufferSize();
  std::unique_ptr<uint8_t[]> savedBuffer(new (std::nothrow) uint8_t[bufferSize]);
  std::unique_ptr<uint8_t[]> plane(new (std::nothrow) uint8_t[bufferSize]);
  if (!savedBuffer || !plane) {
    calibrationReply("ERROR", "grayscale-buffer-allocation");
    return;
  }
  memcpy(savedBuffer.get(), display.getFrameBuffer(), bufferSize);
  std::array<uint32_t, 10> startsUs{};
  std::array<uint32_t, 10> durationsUs{};
  {
    RenderLock lock;
    const uint32_t batchStartUs = micros();
    logSerial.printf("CAL:BENCH-START:GRAY:%s:%lu\n", primaryName.c_str(), millis());
    for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
      const bool dark = iteration % 2 != 0;
      // The production renderer paints gray candidates black in the BW base;
      // the custom LUT then lightens those pixels according to the two planes.
      display.clearScreen(0x00);
      display.refreshDisplay(primaryMode);
      memset(plane.get(), dark ? 0xFF : 0x00, bufferSize);
      display.copyGrayscaleLsbBuffers(plane.get());
      memset(plane.get(), 0xFF, bufferSize);
      display.copyGrayscaleMsbBuffers(plane.get());
      const uint32_t startUs = micros();
      startsUs[iteration] = startUs - batchStartUs;
      display.displayGrayBuffer();
      durationsUs[iteration] = micros() - startUs;
      display.cleanupGrayscaleBuffers(display.getFrameBuffer());
    }
    memcpy(display.getFrameBuffer(), savedBuffer.get(), bufferSize);
    display.refreshDisplay(HalDisplay::HALF_REFRESH);
  }
  for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
    logSerial.printf("CAL:BENCH:GRAY:%s:%lu:%s:%lu:%lu\n", primaryName.c_str(), iteration,
                     iteration % 2 == 0 ? "light" : "dark", startsUs[iteration], durationsUs[iteration]);
  }
  logSerial.printf("CAL:BENCH-END:GRAY:%s:%lu:%lu\n", primaryName.c_str(), iterations, millis());
}

void handleCalibrationCommand(const String& command) {
  if (command == "HELLO") {
    logSerial.printf("CAL:HELLO:%u:%s:%s:%lu\n", CALIBRATION_PROTOCOL_VERSION, gpio.deviceIsX3() ? "x3" : "x4",
                     CROSSPOINT_VERSION, millis());
    return;
  }
  if (command == "STATE") {
    String state = String(gpio.getCalibrationButtonState());
    calibrationReply("STATE", state);
    return;
  }
  if (command.startsWith("MARK:")) {
    const String marker = command.substring(5);
    if (!validCalibrationMarker(marker)) {
      calibrationReply("ERROR", "invalid-marker");
      return;
    }
    calibrationReply("MARK", marker);
    return;
  }
  if (command.startsWith("BUTTON:")) {
    const int separator = command.indexOf(':', 7);
    if (separator < 0) {
      calibrationReply("ERROR", "invalid-button-command");
      return;
    }
    String name = command.substring(7, separator);
    name.toLowerCase();
    String action = command.substring(separator + 1);
    action.toUpperCase();
    const int button = calibrationButtonIndex(name);
    const bool pulse = action.startsWith("PULSE:");
    const uint32_t pulseMs = pulse ? action.substring(6).toInt() : 0;
    if (button < 0 || (action != "DOWN" && action != "UP" && (!pulse || pulseMs < 10 || pulseMs > 10000))) {
      calibrationReply("ERROR", "invalid-button-command");
      return;
    }
    uint8_t state = gpio.getCalibrationButtonState();
    if (action == "DOWN" || pulse) {
      state |= static_cast<uint8_t>(1U << button);
    } else {
      state &= static_cast<uint8_t>(~(1U << button));
    }
    gpio.setCalibrationButtonState(state);
    if (pulse) gpio.setCalibrationButtonAutoRelease(static_cast<uint8_t>(button), pulseMs);
    calibrationReply("BUTTON", name + ":" + action);
    return;
  }
  if (command.startsWith("UPLOAD:BEGIN:")) {
    const int hashSeparator = command.lastIndexOf(':');
    const int sizeSeparator = hashSeparator > 0 ? command.lastIndexOf(':', hashSeparator - 1) : -1;
    const String path = sizeSeparator > 13 ? command.substring(13, sizeSeparator) : String();
    uint32_t byteCount = 0;
    uint64_t expectedHash = 0;
    if (!validCalibrationUploadPath(path) || sizeSeparator < 0 || hashSeparator < 0 ||
        !parseCalibrationCount(command.substring(sizeSeparator + 1, hashSeparator), 1, 64 * 1024 * 1024, byteCount) ||
        !parseCalibrationHash(command.substring(hashSeparator + 1), expectedHash)) {
      calibrationReply("ERROR", "invalid-upload");
      return;
    }
    startCalibrationUpload(path, byteCount, expectedHash);
    return;
  }
  if (command.startsWith("UPLOAD:CHUNK:")) {
    const int separator = command.indexOf(':', 13);
    uint32_t offset = 0;
    const String encoded = separator >= 0 ? command.substring(separator + 1) : String();
    if (separator < 0 || !parseCalibrationCount(command.substring(13, separator), 0, 64 * 1024 * 1024, offset)) {
      calibrationReply("ERROR", "invalid-upload-chunk");
      return;
    }
    receiveCalibrationUploadChunk(offset, encoded);
    return;
  }
  if (command.startsWith("CACHE:CLEAR:")) {
    const String path = command.substring(12);
    if (!validCalibrationEpubPath(path)) {
      calibrationReply("ERROR", "invalid-epub-path");
      return;
    }
    Epub epub(std::string(path.c_str()), "/.crosspoint");
    CalibrationCachePopulation population;
    const bool cacheExists = Storage.exists(epub.getCachePath().c_str());
    if (cacheExists && !measureCalibrationCacheDirectory(epub.getCachePath().c_str(), population)) {
      calibrationReply("ERROR", "cache-population-failed");
      return;
    }
    beginCalibrationActivity();
    const uint32_t startedUs = micros();
    if (!epub.clearCache()) {
      calibrationReply("ERROR", "cache-clear-failed");
      return;
    }
    const uint32_t durationUs = micros() - startedUs;
    logSerial.printf("CAL:CACHE:CLEARED:%s:%lu:%lu:%lu:%llu:%lu\n", path.c_str(), durationUs, population.files,
                     population.directories, static_cast<unsigned long long>(population.bytes), millis());
    return;
  }
  if (command.startsWith("OPEN:")) {
    const String path = command.substring(5);
    if (!validCalibrationEpubPath(path)) {
      calibrationReply("ERROR", "invalid-epub-path");
      return;
    }
    beginCalibrationActivity();
    activityManager.goToReader(std::string(path.c_str()));
    calibrationReply("OPEN", path);
    return;
  }
  if (command == "HOME:SETTINGS") {
    beginCalibrationActivity();
    activityManager.goHome(HomeMenuItem::SETTINGS_MENU);
    calibrationReply("HOME", "SETTINGS");
    return;
  }
  if (command == "HOME:FILES") {
    beginCalibrationActivity();
    activityManager.goHome(HomeMenuItem::FILE_BROWSER);
    calibrationReply("HOME", "FILES");
    return;
  }
  if (command.startsWith("BENCH:SD:")) {
    const int separator = command.indexOf(':', 9);
    uint32_t byteCount = 0;
    uint32_t iterations = 0;
    if (separator < 0 || !parseCalibrationCount(command.substring(9, separator), 4096, 4 * 1024 * 1024, byteCount) ||
        !parseCalibrationCount(command.substring(separator + 1), 1, 32, iterations)) {
      calibrationReply("ERROR", "invalid-sd-benchmark");
      return;
    }
    beginCalibrationActivity();
    runCalibrationSdBenchmark(byteCount, iterations);
    return;
  }
  if (command.startsWith("BENCH:DIR:")) {
    uint32_t iterations = 0;
    if (!parseCalibrationCount(command.substring(10), 1, 10, iterations)) {
      calibrationReply("ERROR", "invalid-directory-benchmark");
      return;
    }
    beginCalibrationActivity();
    runCalibrationDirectoryBenchmark(iterations);
    return;
  }
  if (command.startsWith("BENCH:PANEL:")) {
    const int separator = command.indexOf(':', 12);
    if (separator < 0) {
      calibrationReply("ERROR", "invalid-panel-benchmark");
      return;
    }
    String modeName = command.substring(12, separator);
    modeName.toLowerCase();
    uint32_t iterations = 0;
    if (!parseCalibrationCount(command.substring(separator + 1), 1, 10, iterations)) {
      calibrationReply("ERROR", "invalid-panel-benchmark");
      return;
    }
    if (modeName == "fast") {
      beginCalibrationActivity();
      runCalibrationPanelBenchmark(modeName, HalDisplay::FAST_REFRESH, iterations);
      return;
    }
    if (modeName == "half") {
      beginCalibrationActivity();
      runCalibrationPanelBenchmark(modeName, HalDisplay::HALF_REFRESH, iterations);
      return;
    }
    if (modeName == "full") {
      beginCalibrationActivity();
      runCalibrationPanelBenchmark(modeName, HalDisplay::FULL_REFRESH, iterations);
      return;
    }
    calibrationReply("ERROR", "invalid-panel-benchmark");
    return;
  }
  if (command.startsWith("BENCH:GRAY:")) {
    const int separator = command.indexOf(':', 11);
    if (separator < 0) {
      calibrationReply("ERROR", "invalid-grayscale-benchmark");
      return;
    }
    String primaryName = command.substring(11, separator);
    primaryName.toLowerCase();
    uint32_t iterations = 0;
    if (!parseCalibrationCount(command.substring(separator + 1), 1, 10, iterations)) {
      calibrationReply("ERROR", "invalid-grayscale-benchmark");
      return;
    }
    if (primaryName == "fast") {
      beginCalibrationActivity();
      runCalibrationGrayscaleBenchmark(primaryName, HalDisplay::FAST_REFRESH, iterations);
      return;
    }
    if (primaryName == "half") {
      beginCalibrationActivity();
      runCalibrationGrayscaleBenchmark(primaryName, HalDisplay::HALF_REFRESH, iterations);
      return;
    }
    calibrationReply("ERROR", "invalid-grayscale-benchmark");
    return;
  }
  calibrationReply("ERROR", "unknown-command");
}
}  // namespace
#endif

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

// Verify power button press duration on wake-up from deep sleep
// Pre-condition: isWakeupByPowerButton() == true
void verifyPowerButtonDuration() {
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
    // Fast path for short press
    // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
    return;
  }

  // Give the user up to 1000ms to start holding the power button, and must hold for SETTINGS.getPowerButtonDuration()
  const auto start = millis();
  bool abort = false;
  // Subtract the current time, because inputManager only starts counting the HeldTime from the first update()
  // This way, we remove the time we already took to reach here from the duration,
  // assuming the button was held until now from millis()==0 (i.e. device start time).
  const uint16_t calibration = start;
  const uint16_t calibratedPressDuration =
      (calibration < SETTINGS.getPowerButtonDuration()) ? SETTINGS.getPowerButtonDuration() - calibration : 1;

  gpio.update();
  // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
  while (!gpio.isPressed(HalGPIO::BTN_POWER) && millis() - start < 1000) {
    delay(10);  // only wait 10ms each iteration to not delay too much in case of short configured duration.
    gpio.update();
  }

  t2 = millis();
  if (gpio.isPressed(HalGPIO::BTN_POWER)) {
    do {
      delay(10);
      gpio.update();
    } while (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getPowerButtonHeldTime() < calibratedPressDuration);
    abort = gpio.getPowerButtonHeldTime() < calibratedPressDuration;
  } else {
    abort = true;
  }

  if (abort) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    powerManager.startDeepSleep(gpio);
  }
}
void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  APP_STATE.showBootScreen = !isQuickResumeSleep;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, notoserif14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, notoserif12FontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, notoserif16FontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, notoserif18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  t1 = millis();

#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif

  HalSystem::begin();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();
  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  const auto wakeupReason = gpio.getWakeupReason();
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration");
      gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                   SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // Refresh the cached button state a few times — isPressed() needs ~half a second to settle
    // after boot per the HalGPIO contract. Use a millis-based deadline so we always wait the full
    // settle window even if the loop body takes longer than expected on slow boots.
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  const BootResume resume = isSilentReboot              ? BootResume::Silent
                            : !APP_STATE.showBootScreen ? BootResume::QuickResume
                                                        : BootResume::Splash;

  setupDisplayAndFonts(resume != BootResume::Splash);

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::QuickResume:
      // One-shot flag: re-arm the splash for the next non-quick-resume boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a quick-resume-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (loadSleepFrameBuffer()) {
        // Frame restored: swap the sleep moon for the loading icon.
        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
  allowSleepAt = millis() + 2000;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
#if defined(CROSSPOINT_CALIBRATION)
      else if (cmd.startsWith("CAL:")) {
        handleCalibrationCommand(cmd.substring(4));
      }
#endif
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || halTiltSensor.hadActivity() ||
#if defined(CROSSPOINT_CALIBRATION)
      calibrationActivity ||
#endif
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
#if defined(CROSSPOINT_CALIBRATION)
    calibrationActivity = false;
#endif
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    RenderLock lock;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
