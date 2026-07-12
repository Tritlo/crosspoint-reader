#pragma once

#include "freertos/FreeRTOS.h"

struct EmulatorTaskControl;
using TaskHandle_t = EmulatorTaskControl*;
using TaskFunction_t = void (*)(void*);

enum eNotifyAction { eNoAction, eSetBits, eIncrement, eSetValueWithOverwrite, eSetValueWithoutOverwrite };

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t function, const char* name, uint32_t stackDepth, void* parameter,
                                   UBaseType_t priority, TaskHandle_t* createdTask, BaseType_t core);
TaskHandle_t xTaskGetCurrentTaskHandle();
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value, eNotifyAction action);
uint32_t ulTaskNotifyTake(BaseType_t clearCountOnExit, TickType_t ticksToWait);
void vTaskDelay(TickType_t ticks);
