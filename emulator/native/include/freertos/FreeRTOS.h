#pragma once

#include <cstdint>

using BaseType_t = int;
using UBaseType_t = unsigned int;
using TickType_t = uint32_t;
using StackType_t = uintptr_t;
using portMUX_TYPE = int;

constexpr BaseType_t pdFALSE = 0;
constexpr BaseType_t pdTRUE = 1;
constexpr BaseType_t pdFAIL = 0;
constexpr BaseType_t pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr TickType_t portTICK_PERIOD_MS = 1;
constexpr portMUX_TYPE portMUX_INITIALIZER_UNLOCKED = 0;

constexpr TickType_t pdMS_TO_TICKS(uint32_t milliseconds) { return milliseconds; }

void emulatorTaskEnterCritical(portMUX_TYPE* mux);
void emulatorTaskExitCritical(portMUX_TYPE* mux);

#define taskENTER_CRITICAL(mux) emulatorTaskEnterCritical(mux)
#define taskEXIT_CRITICAL(mux) emulatorTaskExitCritical(mux)
