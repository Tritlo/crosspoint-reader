#include <EInkDisplay.h>
#include <PNGdec.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "emulator/DirectoryStorage.h"
#include "emulator/FreeRtosCompat.h"
#include "emulator/PanelModel.h"

namespace {

struct NativePanelState {
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t widthBytes = 0;
  std::string controller;
  std::string refreshMode = "initial";
  std::string previousPrimaryMode = "fast";
  std::vector<uint8_t> oldPlane;
  std::vector<uint8_t> newPlane;
  std::vector<uint8_t> visible;
  std::vector<emulator::PanelTransition> transitions;
  uint8_t currentCommand = 0;
  std::array<uint8_t, 8> commandData{};
  size_t commandDataSize = 0;
  size_t writeCursor = 0;
  uint8_t cdi = 0;
  std::string x3Bank = "none";
  uint8_t x4UpdateSequence = 0;
  bool x3WhiteBaseline = false;
  bool pendingRefresh = false;
  bool partialWindow = false;
  uint16_t windowXStart = 0;
  uint16_t windowXEnd = 0;
  uint16_t windowYStart = 0;
  uint16_t windowYEnd = 0;
  uint64_t busyUntilUs = 0;
  uint64_t postBusyTransferUs = 0;
  uint64_t generation = 0;
};

std::unordered_map<const freeink::EpdBus*, NativePanelState> panels;
const freeink::EpdBus* activeBus = nullptr;

struct InitialPngContext {
  PNG* decoder = nullptr;
  uint16_t width = 0;
  std::vector<uint8_t>* pixels = nullptr;
  std::vector<uint16_t> line;
};

int drawInitialPngLine(PNGDRAW* draw) {
  auto& context = *static_cast<InitialPngContext*>(draw->pUser);
  context.line.resize(context.width);
  context.decoder->getLineAsRGB565(draw, context.line.data(), PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFFU);
  for (uint16_t x = 0; x < context.width; ++x) {
    const uint16_t color = context.line[x];
    const uint16_t red = static_cast<uint16_t>(((color >> 11) & 0x1F) * 255 / 31);
    const uint16_t green = static_cast<uint16_t>(((color >> 5) & 0x3F) * 255 / 63);
    const uint16_t blue = static_cast<uint16_t>((color & 0x1F) * 255 / 31);
    (*context.pixels)[static_cast<size_t>(draw->y) * context.width + x] =
        static_cast<uint8_t>((red * 77 + green * 150 + blue * 29) >> 8);
  }
  return 1;
}

std::string readInitialPng(const std::filesystem::path& path, uint16_t width, uint16_t height,
                           std::vector<uint8_t>& pixels) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) return "could not open file";
  std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  if (input.bad()) return "could not read file";
  if (bytes.empty()) return "file is empty";
  if (bytes.size() > static_cast<size_t>(INT32_MAX)) return "file is too large";
  PNG decoder;
  const int openResult = decoder.openRAM(bytes.data(), static_cast<int>(bytes.size()), drawInitialPngLine);
  if (openResult != PNG_SUCCESS) return "decoder open error " + std::to_string(openResult);
  if (decoder.getWidth() != width || decoder.getHeight() != height) {
    decoder.close();
    return "geometry is " + std::to_string(decoder.getWidth()) + "x" + std::to_string(decoder.getHeight()) +
           ", expected " + std::to_string(width) + "x" + std::to_string(height);
  }
  pixels.assign(static_cast<size_t>(width) * height, 0xFF);
  InitialPngContext context{&decoder, width, &pixels, {}};
  const int decodeResult = decoder.decode(&context, PNG_CHECK_CRC);
  decoder.close();
  if (decodeResult != PNG_SUCCESS) return "decoder data error " + std::to_string(decodeResult);
  return {};
}

NativePanelState& stateFor(const freeink::EpdBus* bus) {
  auto found = panels.find(bus);
  if (found == panels.end()) throw std::logic_error("e-paper bus used before begin");
  return found->second;
}

std::string commandDetail(const NativePanelState& state, uint8_t command) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%s:0x%02x", state.controller.c_str(), command);
  return buffer;
}

void traceCommand(NativePanelState& state, uint8_t command) {
  emulator::runtimeTracePanel("command", commandDetail(state, command), command);
}

void setCommand(NativePanelState& state, uint8_t command) {
  state.currentCommand = command;
  state.commandDataSize = 0;
  state.writeCursor = 0;
  traceCommand(state, command);
}

void copyIntoPlane(std::vector<uint8_t>& plane, size_t& cursor, const uint8_t* bytes, size_t size) {
  if (bytes == nullptr || cursor >= plane.size()) return;
  const size_t count = std::min(size, plane.size() - cursor);
  std::copy_n(bytes, count, plane.begin() + static_cast<std::ptrdiff_t>(cursor));
  cursor += count;
}

void copyPlaneData(NativePanelState& state, std::vector<uint8_t>& plane, const uint8_t* bytes, size_t size) {
  if (!state.partialWindow) {
    copyIntoPlane(plane, state.writeCursor, bytes, size);
    return;
  }

  const uint16_t xStartByte = static_cast<uint16_t>(state.windowXStart / 8);
  const uint16_t xEndByte = static_cast<uint16_t>(state.windowXEnd / 8);
  const size_t rowBytes = static_cast<size_t>(xEndByte - xStartByte + 1);
  for (size_t index = 0; index < size; ++index) {
    const size_t relative = state.writeCursor + index;
    const size_t row = relative / rowBytes;
    const size_t column = relative % rowBytes;
    uint16_t logicalY = 0;
    if (state.controller == "uc8253") {
      const uint16_t logicalEnd = static_cast<uint16_t>(state.height - 1 - state.windowYStart);
      if (row > logicalEnd) continue;
      logicalY = static_cast<uint16_t>(logicalEnd - row);
    } else {
      logicalY = static_cast<uint16_t>(state.windowYStart + row);
    }
    if (logicalY >= state.height) continue;
    const size_t destination = static_cast<size_t>(logicalY) * state.widthBytes + xStartByte + column;
    if (destination < plane.size()) plane[destination] = bytes[index];
  }
  state.writeCursor += size;
}

void consumeData(NativePanelState& state, const uint8_t* bytes, size_t size) {
  if (bytes == nullptr || size == 0) return;
  if ((state.currentCommand == 0x44 || state.currentCommand == 0x45) && state.controller == "ssd1677") {
    const size_t remaining = state.commandData.size() - state.commandDataSize;
    const size_t count = std::min(size, remaining);
    std::copy_n(bytes, count, state.commandData.begin() + static_cast<std::ptrdiff_t>(state.commandDataSize));
    state.commandDataSize += count;
  }
  if (state.controller == "uc8253") {
    if (state.currentCommand == 0x10) {
      copyPlaneData(state, state.oldPlane, bytes, size);
    } else if (state.currentCommand == 0x13) {
      copyPlaneData(state, state.newPlane, bytes, size);
    } else if (state.currentCommand == 0x50) {
      state.cdi = bytes[0];
    } else if (state.currentCommand == 0x20 && size >= 5) {
      if (bytes[1] == 0x18)
        state.x3Bank = "full";
      else if (bytes[1] == 0x04)
        state.x3Bank = "fast";
      else if (bytes[1] == 0x03 && bytes[3] == 0x01)
        state.x3Bank = "grayscale";
      else if (bytes[1] == 0x06 && bytes[2] == 0x03)
        state.x3Bank = "condition";
      else
        state.x3Bank = "normal-or-half";
    } else if (state.currentCommand == 0x21 && state.x3Bank == "normal-or-half") {
      state.x3Bank = bytes[0] == 0xAA ? "half" : "condition";
    } else if (state.currentCommand == 0x90 && size >= 9) {
      state.windowXStart = static_cast<uint16_t>(bytes[0] << 8 | bytes[1]);
      state.windowXEnd = static_cast<uint16_t>(bytes[2] << 8 | bytes[3]);
      state.windowYStart = static_cast<uint16_t>(bytes[4] << 8 | bytes[5]);
      state.windowYEnd = static_cast<uint16_t>(bytes[6] << 8 | bytes[7]);
      state.partialWindow = true;
    }
  } else {
    if (state.currentCommand == 0x24) {
      copyPlaneData(state, state.newPlane, bytes, size);
    } else if (state.currentCommand == 0x26) {
      copyPlaneData(state, state.oldPlane, bytes, size);
    } else if (state.currentCommand == 0x46) {
      std::fill(state.newPlane.begin(), state.newPlane.end(), 0xFF);
    } else if (state.currentCommand == 0x47) {
      std::fill(state.oldPlane.begin(), state.oldPlane.end(), 0xFF);
    } else if (state.currentCommand == 0x22) {
      state.x4UpdateSequence = bytes[0];
    } else if (state.currentCommand == 0x44 && state.commandDataSize >= 4) {
      const auto& data = state.commandData;
      const uint16_t first = static_cast<uint16_t>(data[0] | data[1] << 8);
      const uint16_t second = static_cast<uint16_t>(data[2] | data[3] << 8);
      state.windowXStart = std::min(first, second);
      state.windowXEnd = std::max(first, second);
    } else if (state.currentCommand == 0x45 && state.commandDataSize >= 4) {
      const auto& data = state.commandData;
      const uint16_t first = static_cast<uint16_t>(data[0] | data[1] << 8);
      const uint16_t second = static_cast<uint16_t>(data[2] | data[3] << 8);
      const uint16_t gateStart = std::min(first, second);
      const uint16_t gateEnd = std::max(first, second);
      state.windowYStart = static_cast<uint16_t>(state.height - 1 - gateEnd);
      state.windowYEnd = static_cast<uint16_t>(state.height - 1 - gateStart);
      state.partialWindow = state.windowXStart != 0 || state.windowXEnd + 1 != state.width || state.windowYStart != 0 ||
                            state.windowYEnd + 1 != state.height;
    }
  }
}

std::string pendingMode(const NativePanelState& state) {
  if (state.controller == "ssd1677") {
    if (state.x4UpdateSequence == 0xF7) return "full";
    if (state.x4UpdateSequence == 0xD7) return "half";
    if (state.x4UpdateSequence == 0xFC || state.x4UpdateSequence == 0x1C) return "fast";
    if (state.x4UpdateSequence == 0x0C || state.x4UpdateSequence == 0x0F || state.x4UpdateSequence == 0xCC ||
        state.x4UpdateSequence == 0xCF || state.x4UpdateSequence == 0xC7) {
      return "grayscale";
    }
    return "controller";
  }
  if (state.x3Bank != "none") return state.x3Bank;
  if (state.x3WhiteBaseline) return "full";
  return state.cdi == 0xA9 ? "half" : "fast";
}

uint64_t refreshDurationUs(const NativePanelState& state, const std::string& mode) {
  const auto& timing = emulator::runtimeStorage().config().timing;
  if (timing.calibrated && state.controller == "ssd1677") {
    if (mode == "full") return timing.panel.fullBusyUs;
    if (mode == "half") return timing.panel.halfBusyUs;
    if (mode == "grayscale") {
      return state.previousPrimaryMode == "half" ? timing.panel.grayscaleAfterHalfBusyUs
                                                 : timing.panel.grayscaleAfterFastBusyUs;
    }
    return timing.panel.fastBusyUs;
  }
  if (state.controller == "ssd1677") {
    if (mode == "full") return 1800000;
    if (mode == "half") return 900000;
    if (mode == "grayscale") return 1200000;
    return 500000;
  }
  if (mode == "full") return 800000;
  if (mode == "half") return 600000;
  if (mode == "grayscale") return 700000;
  return 350000;
}

struct RefreshTransferTiming {
  uint64_t beforeBusyUs = 0;
  uint64_t afterBusyUs = 0;
};

RefreshTransferTiming refreshTransferTiming(const NativePanelState& state, const std::string& mode) {
  const auto& timing = emulator::runtimeStorage().config().timing;
  if (!timing.calibrated || state.controller != "ssd1677") return {};

  uint64_t totalUs = 0;
  uint64_t planesBeforeBusy = 0;
  // The X4 single-buffer path writes BW before FAST, BW+RED before HALF/FULL,
  // then re-seeds both planes after every blocking primary refresh.
  constexpr uint64_t planesAfterBusy = 2;
  if (mode == "fast") {
    totalUs = timing.render.fast.primaryTransferUs;
    planesBeforeBusy = 1;
  } else if (mode == "half") {
    totalUs = timing.render.half.primaryTransferUs;
    planesBeforeBusy = 2;
  } else if (mode == "full") {
    totalUs = timing.panel.fullOperationUs - timing.panel.fullBusyUs;
    planesBeforeBusy = 2;
  } else if (mode == "grayscale") {
    totalUs = state.previousPrimaryMode == "half" ? timing.render.half.grayscaleTransferUs
                                                  : timing.render.fast.grayscaleTransferUs;
    return {totalUs, 0};
  }

  const uint64_t beforeBusyUs = totalUs * planesBeforeBusy / (planesBeforeBusy + planesAfterBusy);
  return {beforeBusyUs, totalUs - beforeBusyUs};
}

std::string_view transferBeforeBusyModel(const NativePanelState& state, const std::string& mode) {
  if (mode == "full") return "panel.full.transfer-before-busy";
  if (mode == "half") return "panel.half.transfer-before-busy";
  if (mode == "grayscale") {
    return state.previousPrimaryMode == "half" ? "panel.grayscale-after-half.transfer"
                                               : "panel.grayscale-after-fast.transfer";
  }
  return "panel.fast.transfer-before-busy";
}

std::string_view transferAfterBusyModel(const std::string& mode) {
  if (mode == "full") return "panel.full.transfer-after-busy";
  if (mode == "half") return "panel.half.transfer-after-busy";
  return "panel.fast.transfer-after-busy";
}

std::string_view busyModel(const NativePanelState& state, const std::string& mode) {
  if (mode == "full") return "panel.full.busy";
  if (mode == "half") return "panel.half.busy";
  if (mode == "grayscale") {
    return state.previousPrimaryMode == "half" ? "panel.grayscale-after-half.busy" : "panel.grayscale-after-fast.busy";
  }
  return "panel.fast.busy";
}

std::vector<uint8_t> targetPixels(const NativePanelState& state, const std::string& mode) {
  std::vector<uint8_t> target(static_cast<size_t>(state.width) * state.height, 0xFF);
  for (size_t pixel = 0; pixel < target.size(); ++pixel) {
    const uint8_t mask = static_cast<uint8_t>(0x80U >> (pixel % 8));
    const size_t byte = (pixel / state.width) * state.widthBytes + ((pixel % state.width) / 8);
    const bool oldSet = byte < state.oldPlane.size() && (state.oldPlane[byte] & mask) != 0;
    const bool newSet = byte < state.newPlane.size() && (state.newPlane[byte] & mask) != 0;
    if (mode == "grayscale") {
      // Grayscale strips are an overlay on the already-displayed BW base, not an
      // absolute two-bit image: 00 leaves the base untouched, 10 nudges it light,
      // and 11 nudges it dark. GfxRenderer deliberately clears strip scratch to
      // zero and sets both bits for dark antialiasing pixels.
      if (!oldSet && !newSet)
        target[pixel] = state.visible[pixel];
      else if (!oldSet && newSet)
        target[pixel] = 170;
      else
        target[pixel] = 85;
    } else {
      target[pixel] = newSet ? 0xFF : 0x00;
    }
  }
  return target;
}

std::vector<uint8_t> settledPixels(const NativePanelState& state, const std::string& mode) {
  auto target = targetPixels(state, mode);
  if (mode == "fast" && state.visible.size() == target.size()) {
    for (size_t index = 0; index < target.size(); ++index) {
      target[index] = static_cast<uint8_t>((static_cast<uint16_t>(target[index]) * 15U + state.visible[index]) / 16U);
    }
  }
  return target;
}

bool isLightTarget(const std::vector<uint8_t>& pixels) {
  uint64_t sum = 0;
  for (uint8_t pixel : pixels) sum += pixel;
  return !pixels.empty() && sum >= pixels.size() * 128ULL;
}

bool hasDarkPixelPercent(const std::vector<uint8_t>& pixels, uint8_t minimumPercent) {
  const size_t dark = std::count_if(pixels.begin(), pixels.end(), [](uint8_t pixel) { return pixel < 128; });
  return !pixels.empty() && dark * 100ULL >= pixels.size() * minimumPercent;
}

void addTransition(NativePanelState& state, uint64_t timeUs, uint64_t generation, std::string phase,
                   std::vector<uint8_t> pixels) {
  emulator::runtimeTracePanel("phase", phase, timeUs);
  state.transitions.push_back({timeUs, generation, std::move(phase), std::move(pixels)});
}

void advanceVisiblePanel(NativePanelState& state, uint64_t timeUs) {
  const emulator::PanelTransition* latest = nullptr;
  for (const auto& transition : state.transitions) {
    if (transition.simulatedTimeUs <= timeUs &&
        (latest == nullptr || transition.simulatedTimeUs >= latest->simulatedTimeUs)) {
      latest = &transition;
    }
  }
  if (latest != nullptr) state.visible = latest->pixels;
}

bool hasPendingOpticalTransition(const NativePanelState& state, uint64_t timeUs) {
  return std::any_of(
      state.transitions.begin(), state.transitions.end(),
      [timeUs](const emulator::PanelTransition& transition) { return transition.simulatedTimeUs > timeUs; });
}

void addMeasuredTransitions(NativePanelState& state, const std::string& mode, uint64_t operationStartUs,
                            uint64_t generation) {
  const auto& optical = emulator::runtimeStorage().config().timing.optical;
  auto target = settledPixels(state, mode);
  const bool light = isLightTarget(target);
  if (mode == "grayscale") {
    const auto& grayscaleTiming =
        state.previousPrimaryMode == "half" ? optical.grayscaleAfterHalf : optical.grayscaleAfterFast;
    if (!grayscaleTiming.visibleChangeDetected) {
      addTransition(state, state.busyUntilUs, generation, "no-visible-change", state.visible);
      return;
    }
    const auto& timing = light ? grayscaleTiming.light : grayscaleTiming.dark;
    auto drive = target;
    for (size_t index = 0; index < drive.size(); ++index) {
      drive[index] = static_cast<uint8_t>((static_cast<uint16_t>(drive[index]) + state.visible[index]) / 2U);
    }
    addTransition(state, operationStartUs + timing.onsetUs, generation, "optical-drive", std::move(drive));
    addTransition(state, operationStartUs + timing.endUs, generation, "optical-settled", std::move(target));
    return;
  }

  if (mode == "half") {
    auto inverted = target;
    for (uint8_t& pixel : inverted) pixel = static_cast<uint8_t>(0xFFU - pixel);
    addTransition(state, operationStartUs + optical.readerHalf.invertedTargetUs, generation, "inverted-target",
                  std::move(inverted));
    addTransition(state, operationStartUs + optical.readerHalf.settledTargetUs, generation, "optical-settled",
                  std::move(target));
    return;
  }

  if (mode == "fast") {
    const bool imageHeavy = hasDarkPixelPercent(target, optical.readerImageFast.darkPixelPercentMin);
    const auto& fastTiming = imageHeavy ? optical.readerImageFast.timing : optical.readerFast;
    auto drive = target;
    for (size_t index = 0; index < drive.size(); ++index) {
      const uint8_t base = imageHeavy ? 0xFF : state.visible[index];
      drive[index] = static_cast<uint8_t>((static_cast<uint16_t>(drive[index]) + base) / 2U);
    }
    addTransition(state, operationStartUs + fastTiming.transitionStartUs, generation,
                  imageHeavy ? "optical-image-drive" : "optical-drive", std::move(drive));
    addTransition(state, operationStartUs + fastTiming.transitionEndUs, generation, "optical-settled",
                  std::move(target));
    return;
  }

  const auto& timing = light ? optical.full.white : optical.full.black;
  const uint8_t firstDrive = light ? 0xFF : 0x00;
  uint64_t timeUs = timing.onsetUs;
  size_t pulse = 0;
  while (timeUs < timing.endUs) {
    const uint8_t value = pulse % 2 == 0 ? firstDrive : static_cast<uint8_t>(~firstDrive);
    addTransition(state, operationStartUs + timeUs, generation, value == 0 ? "charge-black" : "charge-white",
                  std::vector<uint8_t>(state.visible.size(), value));
    timeUs += optical.fullPulseIntervalUs;
    ++pulse;
  }
  addTransition(state, operationStartUs + timing.endUs, generation, "optical-settled", std::move(target));
}

void finishRefresh(NativePanelState& state) {
  if (!state.pendingRefresh) return;
  const std::string mode = pendingMode(state);
  const auto& timing = emulator::runtimeStorage().config().timing;
  if (timing.optical.measured && state.controller == "ssd1677") {
    advanceVisiblePanel(state, emulator::runtimeMicroseconds());
  } else {
    state.visible = settledPixels(state, mode);
  }
  state.refreshMode = mode;
  state.pendingRefresh = false;
  state.x3WhiteBaseline = false;
  ++state.generation;
  if (!timing.optical.measured || state.controller != "ssd1677") {
    state.transitions.push_back({state.busyUntilUs, state.generation, "settled", state.visible});
  }
  emulator::runtimeTracePanel("phase", "settled", state.busyUntilUs);
  emulator::runtimeTracePanel("refresh", mode, state.generation);
}

void startRefresh(NativePanelState& state) {
  const std::string mode = pendingMode(state);
  const auto& timing = emulator::runtimeStorage().config().timing;
  const RefreshTransferTiming transfer = refreshTransferTiming(state, mode);
  if (transfer.beforeBusyUs > 0) {
    emulator::runtimeApplyTiming(transferBeforeBusyModel(state, mode), mode, transfer.beforeBusyUs);
  }
  state.postBusyTransferUs = transfer.afterBusyUs;
  const uint64_t start = emulator::runtimeMicroseconds();
  const uint64_t operationStart = start - transfer.beforeBusyUs;
  advanceVisiblePanel(state, start);
  const uint64_t duration = refreshDurationUs(state, mode);
  if (timing.calibrated && state.controller == "ssd1677") {
    emulator::runtimeTraceTiming(busyModel(state, mode), mode, duration);
  }
  state.pendingRefresh = true;
  state.busyUntilUs = start + duration;
  if (mode == "fast" || mode == "half" || mode == "full") state.previousPrimaryMode = mode;
  const uint64_t nextGeneration = state.generation + 1;
  if (timing.optical.measured && state.controller == "ssd1677") {
    addMeasuredTransitions(state, mode, operationStart, nextGeneration);
  } else if (mode == "full") {
    state.transitions.push_back(
        {start + duration / 4, nextGeneration, "black-flash", std::vector<uint8_t>(state.visible.size(), 0x00)});
    state.transitions.push_back(
        {start + duration / 2, nextGeneration, "white-flash", std::vector<uint8_t>(state.visible.size(), 0xFF)});
    emulator::runtimeTracePanel("phase", "black-flash", start + duration / 4);
    emulator::runtimeTracePanel("phase", "white-flash", start + duration / 2);
  } else {
    auto transition = targetPixels(state, mode);
    for (size_t index = 0; index < transition.size(); ++index) {
      transition[index] = static_cast<uint8_t>((static_cast<uint16_t>(transition[index]) + state.visible[index]) / 2U);
    }
    state.transitions.push_back({start + duration / 2, nextGeneration, "drive", std::move(transition)});
    emulator::runtimeTracePanel("phase", "drive", start + duration / 2);
  }
  emulator::runtimeTracePanel("busy", mode, state.busyUntilUs);
}

}  // namespace

namespace freeink {

void EpdBus::begin(const EpdPins& pins, uint32_t spiHz, BusyPolarity busy, int8_t, int8_t coCs) {
  _pins = pins;
  _spiHz = spiHz;
  _busy = busy;
  _coCs = coCs;
  const auto& profile = emulator::runtimeStorage().config().profile;
  NativePanelState state;
  state.width = profile.panelWidth;
  state.height = profile.panelHeight;
  state.widthBytes = static_cast<uint16_t>((state.width + 7) / 8);
  state.controller = profile.controller;
  const size_t bytes = static_cast<size_t>(state.widthBytes) * state.height;
  state.oldPlane.assign(bytes, 0xFF);
  state.newPlane.assign(bytes, 0xFF);
  const auto previous = panels.find(this);
  if (previous != panels.end() && previous->second.width == state.width && previous->second.height == state.height) {
    advanceVisiblePanel(previous->second, emulator::runtimeMicroseconds());
    state.visible = previous->second.visible;
    state.generation = previous->second.generation;
    state.transitions = previous->second.transitions;
    state.refreshMode = "retained-reset";
  } else {
    const auto& png = emulator::runtimeStorage().config().initialPanelPng;
    if (png) {
      const std::string pngError = readInitialPng(*png, state.width, state.height, state.visible);
      if (!pngError.empty()) {
        throw std::runtime_error("initial panel PNG " + png->string() + ": " + pngError);
      }
      state.transitions.push_back({emulator::runtimeMicroseconds(), 0, "initial-png", state.visible});
    } else {
      const uint8_t initial = emulator::runtimeStorage().config().initialPanel == "black" ? 0x00 : 0xFF;
      state.visible.assign(static_cast<size_t>(state.width) * state.height, initial);
      state.transitions.push_back(
          {emulator::runtimeMicroseconds(), 0, initial == 0 ? "initial-black" : "initial-white", state.visible});
    }
  }
  panels[this] = std::move(state);
  activeBus = this;
  emulator::runtimeTracePanel("initialized", profile.controller, bytes);
  emulator::runtimeTracePanel(
      "phase", panels[this].refreshMode == "retained-reset" ? "retained-reset" : panels[this].transitions.back().phase,
      emulator::runtimeMicroseconds());
}

void EpdBus::reset(uint16_t extraSettleMs) { delay(42 + extraSettleMs); }

void EpdBus::cmd(uint8_t command) {
  auto& state = stateFor(this);
  setCommand(state, command);
  if (state.controller == "uc8253" && command == 0x91) state.partialWindow = true;
  if (state.controller == "uc8253" && command == 0x92) state.partialWindow = false;
  if (state.controller == "uc8253" && command == 0x12) startRefresh(state);
  if (state.controller == "ssd1677" && command == 0x20) startRefresh(state);
  if ((state.controller == "uc8253" && command == 0x07) || (state.controller == "ssd1677" && command == 0x10)) {
    emulator::runtimeTracePanel("power", "deep-sleep", state.generation);
  }
}

void EpdBus::data(uint8_t value) { consumeData(stateFor(this), &value, 1); }
void EpdBus::data(const uint8_t* bytes, uint16_t size) { consumeData(stateFor(this), bytes, size); }

void EpdBus::cmdData(uint8_t command, const uint8_t* bytes, uint16_t size) {
  auto& state = stateFor(this);
  setCommand(state, command);
  consumeData(state, bytes, size);
}

void EpdBus::cmdData2(uint8_t command, uint8_t first, uint8_t second) {
  const uint8_t bytes[] = {first, second};
  cmdData(command, bytes, sizeof(bytes));
}

void EpdBus::beginTxn() {}
void EpdBus::endTxn() {}
void EpdBus::rawCmd(uint8_t command) { setCommand(stateFor(this), command); }
void EpdBus::rawData(uint8_t value) { consumeData(stateFor(this), &value, 1); }
void EpdBus::rawWriteBytes(const uint8_t* bytes, uint16_t size) { consumeData(stateFor(this), bytes, size); }

void EpdBus::waitBusy(const char* tag) { waitBusy(_busy, tag); }

void EpdBus::waitBusy(BusyPolarity, const char* tag) {
  auto& state = stateFor(this);
  if (state.pendingRefresh) {
    const std::string mode = pendingMode(state);
    const uint64_t now = emulator::runtimeMicroseconds();
    if (state.busyUntilUs > now) emulator::runtimeDelay(state.busyUntilUs - now);
    finishRefresh(state);
    const uint64_t postBusyTransferUs = state.postBusyTransferUs;
    state.postBusyTransferUs = 0;
    if (postBusyTransferUs > 0 && (tag == nullptr || std::strcmp(tag, "async refresh") != 0)) {
      // The blocking single-buffer SSD1677 path re-seeds BW and RED after BUSY.
      // Keep the measured whole operation unchanged while leaving BUSY at the
      // controller boundary. Async refreshes have no post-refresh re-seed.
      emulator::runtimeApplyTiming(transferAfterBusyModel(mode), mode, postBusyTransferUs);
    }
  } else {
    emulator::runtimeDelay(5000);
  }
}

bool EpdBus::isBusy() const {
  auto& state = stateFor(this);
  if (state.pendingRefresh && emulator::runtimeMicroseconds() >= state.busyUntilUs) finishRefresh(state);
  return state.pendingRefresh;
}

void EpdBus::writeMirroredPlane(const uint8_t* plane, uint16_t height, uint16_t widthBytes, bool invert) {
  if (plane == nullptr) return;
  auto& state = stateFor(this);
  std::vector<uint8_t> mirrored(static_cast<size_t>(height) * widthBytes);
  for (uint16_t row = 0; row < height; ++row) {
    const uint8_t* source = plane + static_cast<size_t>(height - 1 - row) * widthBytes;
    for (uint16_t column = 0; column < widthBytes; ++column) {
      mirrored[static_cast<size_t>(row) * widthBytes + column] =
          invert ? static_cast<uint8_t>(~source[column]) : source[column];
    }
  }
  consumeData(state, mirrored.data(), mirrored.size());
}

void EpdBus::sendPlaneFlipped(uint8_t command, const uint8_t* plane, uint16_t height, uint16_t widthBytes) {
  auto& state = stateFor(this);
  setCommand(state, command);
  if (plane == nullptr) return;
  const size_t size = std::min(static_cast<size_t>(height) * widthBytes,
                               command == 0x10 ? state.oldPlane.size() : state.newPlane.size());
  if (command == 0x10) {
    std::copy_n(plane, size, state.oldPlane.begin());
  } else if (command == 0x13) {
    std::copy_n(plane, size, state.newPlane.begin());
  }
}

void EpdBus::fillPlane(uint8_t command, uint8_t fillByte, uint16_t, uint16_t) {
  auto& state = stateFor(this);
  setCommand(state, command);
  if (command == 0x10) {
    std::fill(state.oldPlane.begin(), state.oldPlane.end(), fillByte);
    state.x3WhiteBaseline = fillByte == 0xFF;
  } else if (command == 0x13) {
    std::fill(state.newPlane.begin(), state.newPlane.end(), fillByte);
  }
}

}  // namespace freeink

namespace emulator {

PanelSnapshot panelSnapshot() {
  if (activeBus == nullptr) return {};
  auto& state = stateFor(activeBus);
  const uint64_t now = runtimeMicroseconds();
  if (state.pendingRefresh && now >= state.busyUntilUs) finishRefresh(state);
  advanceVisiblePanel(state, now);
  return {
      state.width,      state.height, state.generation, state.pendingRefresh, hasPendingOpticalTransition(state, now),
      state.controller, state.visible};
}

const std::vector<PanelTransition>& panelTransitions() {
  static const std::vector<PanelTransition> empty;
  if (activeBus == nullptr) return empty;
  return stateFor(activeBus).transitions;
}

}  // namespace emulator
