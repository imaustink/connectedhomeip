/*
 *
 *    Copyright (c) 2024 Project CHIP Authors
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

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <functional>

enum class LightLevel : uint8_t
{
    Off  = 0,
    Low  = 1,
    High = 2
};

class LightController
{
public:
    using StateChangeCallback = std::function<void()>;
    
    bool Init();
    void SetLevel(LightLevel level);
    void SetLevelDebounced(LightLevel level); // Debounced version for Matter
    LightLevel GetLevel();
    bool IsOn();
    uint8_t GetBrightness();
    
    // Called by ISR to notify state change
    void NotifyStateChange();
    
    // Set callback for state changes
    void SetStateChangeCallback(StateChangeCallback callback) { mStateChangeCallback = callback; }

private:
    LightLevel mCurrentLevel;
    LightLevel mTargetLevel;
    bool mPowerStatus;
    SemaphoreHandle_t mSyncMutex;
    TaskHandle_t mTaskHandle;
    TimerHandle_t mDebounceTimer;
    LightLevel mPendingLevel;
    LightLevel mLevelWhenDebounceStarted;  // Track level when debounce started
    bool mUpdatingFromHardware;  // Flag to prevent Matter callback loops in TEST MODE
    bool mStartupComplete;  // Flag to prevent hardware cycling during startup init
    bool mHasPendingCycleCommand;  // Flag for pending commands received during cycles
    StateChangeCallback mStateChangeCallback;
    
    // Debouncing for hardware ISR
    volatile uint32_t mLastInterruptTime;
    volatile uint32_t mLastProcessedTime;  // Track when we last processed a state change
    static constexpr uint32_t kDebounceMs = 50; // Debounce time in milliseconds
    static constexpr uint32_t kProcessCooldownMs = 1000; // Ignore button presses for 1s after processing
    
    // Debouncing for Matter attribute changes (slider movements)
    static constexpr uint32_t kMatterDebounceMs = 300; // Wait for slider to stop
    
    // GPIO pins for light control [XIAO ESP32C6]
    static constexpr gpio_num_t kLightControlPin      = GPIO_NUM_0; // Output: pulse to cycle state
    static constexpr gpio_num_t kLightInterruptPin    = GPIO_NUM_1; // Input: state change notification
    static constexpr gpio_num_t kLightPowerStatusPin  = GPIO_NUM_2; // Input: power on/off status
    
    static constexpr uint32_t kPulseDurationMs        = 500;
    static constexpr uint32_t kStateChangeTimeoutMs   = 200;
    static constexpr uint32_t kPowerReadDelayMs       = 150;

    void SendPulse();
    void CycleToTargetLevel();
    void ReadPowerStatus();
    void SynchronizeOnStartup();
    
    static void IRAM_ATTR StateChangeISR(void * arg);
    static void IRAM_ATTR StatusLEDChangeISR(void * arg);
    static void DebounceTimerCallback(TimerHandle_t timer);
};
