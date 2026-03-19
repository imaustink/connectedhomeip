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

enum class FanSpeed : uint8_t
{
    Off    = 0,
    Low    = 1,
    Medium = 2,
    High   = 3
};

class FanController
{
public:
    void Init();
    void SetSpeed(FanSpeed speed);
    FanSpeed GetSpeed();
    bool IsOn();
    uint8_t GetPercentSpeed();

private:
    FanSpeed mCurrentSpeed;
    
    // GPIO pins for fan speed control (output - pulse to set state) [XIAO ESP32C6 D3-D6]
    static constexpr gpio_num_t kFanOffControlPin    = GPIO_NUM_21; // D3
    static constexpr gpio_num_t kFanLowControlPin    = GPIO_NUM_22; // D4
    static constexpr gpio_num_t kFanMediumControlPin = GPIO_NUM_23; // D5
    static constexpr gpio_num_t kFanHighControlPin   = GPIO_NUM_18; // D10 (boot-safe, no strapping)
    
    // GPIO pins for fan status feedback (input - active-LOW with pull-up) [XIAO ESP32C6 D7-D9]
    static constexpr gpio_num_t kFanLowStatusPin    = GPIO_NUM_17; // D7
    static constexpr gpio_num_t kFanMediumStatusPin = GPIO_NUM_19; // D8
    static constexpr gpio_num_t kFanHighStatusPin   = GPIO_NUM_20; // D9

    void SendControlPulse(gpio_num_t pin);
    FanSpeed ReadCurrentSpeed();
};
