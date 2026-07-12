#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct EmulatorSemaphore;
using SemaphoreHandle_t = EmulatorSemaphore*;
#include "freertos/queue.h"

SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex();
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticksToWait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, TickType_t ticksToWait);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore);
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t semaphore);
BaseType_t xQueuePeek(QueueHandle_t queue, void* item, TickType_t ticksToWait);
