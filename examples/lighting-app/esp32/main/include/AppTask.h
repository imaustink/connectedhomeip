/*
 *
 *    Copyright (c) 2022-2023 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "AppEvent.h"
#include "Button.h"
#include "LEDWidget.h"
#include "freertos/FreeRTOS.h"
#include <platform/CHIPDeviceLayer.h>

// Application-defined error codes in the CHIP_ERROR space.
#define APP_ERROR_EVENT_QUEUE_FAILED CHIP_APPLICATION_ERROR(0x01)
#define APP_ERROR_CREATE_TASK_FAILED CHIP_APPLICATION_ERROR(0x02)
#define APP_ERROR_UNHANDLED_EVENT CHIP_APPLICATION_ERROR(0x03)
#define APP_ERROR_CREATE_TIMER_FAILED CHIP_APPLICATION_ERROR(0x04)
#define APP_ERROR_START_TIMER_FAILED CHIP_APPLICATION_ERROR(0x05)
#define APP_ERROR_STOP_TIMER_FAILED CHIP_APPLICATION_ERROR(0x06)

// Commissioning button on D6 (GPIO16)
#define COMMISSIONING_BUTTON_GPIO GPIO_NUM_16

// Commissioning LED on GPIO15 (User LED)
#define COMMISSIONING_LED_GPIO GPIO_NUM_15

extern LEDWidget AppLED;
extern Button AppButton;

class AppTask
{

public:
    CHIP_ERROR StartAppTask();
    static void AppTaskMain(void * pvParameter);
    void PostEvent(const AppEvent * event);

    void ButtonEventHandler(const uint8_t buttonHandle, uint8_t btnAction);

    void UpdateClusterState();
    
    // Flag to prevent feedback loop when updating Matter from hardware
    static bool IsUpdatingFromHardware() { return sUpdatingFromHardware; }

private:
    friend AppTask & GetAppTask(void);
    friend void commissioning_button_isr_handler(void * arg);
    
    CHIP_ERROR Init();
    void DispatchEvent(AppEvent * event);
    static void SwitchActionEventHandler(AppEvent * aEvent);
    static void LightingActionEventHandler(AppEvent * aEvent);
    static void CommissioningButtonEventHandler(AppEvent * aEvent);

#if CONFIG_DEVICE_TYPE_M5STACK
    static void ButtonPressedAction(AppEvent * aEvent);
#endif

    CHIP_ERROR InitCommissioningButton();
    CHIP_ERROR InitCommissioningLED();
    
    static void StartCommissioningLEDFlash();
    static void StopCommissioningLEDFlash();
    static void CommissioningLEDTimerCallback(TimerHandle_t xTimer);

    static AppTask sAppTask;
    static TimerHandle_t sCommissioningLEDTimer;
    static bool sCommissioningLEDState;
    static bool sUpdatingFromHardware;  // Flag to prevent feedback loop
    static bool sInitialSyncDone;       // Flag to track if initial cluster sync is complete
};

inline AppTask & GetAppTask(void)
{
    return AppTask::sAppTask;
}
