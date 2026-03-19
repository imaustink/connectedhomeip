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
#include "freertos/task.h"
#include <functional>

// Fan is treated as binary: Off or On (High/100%).
// Medium/Low states are intentionally not supported.
enum class FanSpeed : uint8_t
{
    Off  = 0,
    High = 3  // Only on state - maps to 100%
};

class FanController
{
public:
    using StateChangeCallback = std::function<void()>;

    void Init();
    void SetSpeed(FanSpeed speed);
    FanSpeed GetSpeed();
    bool IsOn();
    uint8_t GetPercentSpeed();

    // Called by ISR to notify a button-press state change
    void NotifyStateChange();

    void SetStateChangeCallback(StateChangeCallback callback) { mStateChangeCallback = callback; }

private:
    FanSpeed mCurrentSpeed = FanSpeed::Off;
    TaskHandle_t mTaskHandle = nullptr;
    StateChangeCallback mStateChangeCallback;

    // Debouncing (mirrors LightController approach)
    volatile uint32_t mLastInterruptTime  = 0;
    volatile uint32_t mLastProcessedTime  = 0;
    static constexpr uint32_t kDebounceMs        = 50;    // ISR hardware debounce
    static constexpr uint32_t kProcessCooldownMs = 1000;  // Cooldown after processing

    // GPIO pin assignments [XIAO ESP32C6]
    // D3 (GPIO21): control pulse output - pulse to toggle fan on/off
    // D4 (GPIO22): button interrupt input - fires when fan button is pressed
    // D7 (GPIO17): LED status input - active-LOW with pull-up (LOW = fan is ON)
    static constexpr gpio_num_t kFanControlPin   = GPIO_NUM_21; // D3 - output
    static constexpr gpio_num_t kFanInterruptPin = GPIO_NUM_22; // D4 - input, ISR
    static constexpr gpio_num_t kFanLEDStatusPin = GPIO_NUM_17; // D7 - input, LED status

    static constexpr uint32_t kPulseDurationMs = 500;

    void SendControlPulse();
    bool ReadLEDStatus(); // true = fan is ON

    static void IRAM_ATTR FanButtonISR(void * arg);
};
