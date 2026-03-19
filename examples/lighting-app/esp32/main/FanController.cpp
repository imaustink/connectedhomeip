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

#include "FanController.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char TAG[] = "FanController";

void FanController::Init()
{
    // Configure control pins as outputs
    gpio_config_t out_conf = {};
    out_conf.intr_type     = GPIO_INTR_DISABLE;
    out_conf.mode          = GPIO_MODE_OUTPUT;
    out_conf.pin_bit_mask  = (1ULL << kFanOffControlPin) | (1ULL << kFanLowControlPin) |
                             (1ULL << kFanMediumControlPin) | (1ULL << kFanHighControlPin);
    out_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    out_conf.pull_up_en    = GPIO_PULLUP_DISABLE;
    gpio_config(&out_conf);

    // Initialize all control pins to low
    gpio_set_level(kFanOffControlPin, 0);
    gpio_set_level(kFanLowControlPin, 0);
    gpio_set_level(kFanMediumControlPin, 0);
    gpio_set_level(kFanHighControlPin, 0);
    
    // Configure status pins as inputs (active-LOW with pull-up - pulled to ground when that speed is active)
    gpio_config_t in_conf = {};
    in_conf.intr_type     = GPIO_INTR_DISABLE;
    in_conf.mode          = GPIO_MODE_INPUT;
    in_conf.pin_bit_mask  = (1ULL << kFanLowStatusPin) | (1ULL << kFanMediumStatusPin) |
                            (1ULL << kFanHighStatusPin);
    in_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    in_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    esp_err_t err = gpio_config(&in_conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure status pins: %d", err);
    }
    
    ESP_LOGI(TAG, "Status pins configured: D7(GPIO%d), D8(GPIO%d), D9(GPIO%d)", 
             kFanLowStatusPin, kFanMediumStatusPin, kFanHighStatusPin);
    
    // Read current state from hardware
    mCurrentSpeed = ReadCurrentSpeed();

    ESP_LOGI(TAG, "Fan controller initialized with current speed: %d", static_cast<uint8_t>(mCurrentSpeed));
}

void FanController::SetSpeed(FanSpeed speed)
{
    if (speed == mCurrentSpeed)
    {
        return;
    }

    mCurrentSpeed = speed;
    
    // Send pulse to appropriate control pin
    gpio_num_t controlPin;
    switch (speed)
    {
    case FanSpeed::Off:
        controlPin = kFanOffControlPin;
        break;
    case FanSpeed::Low:
        controlPin = kFanLowControlPin;
        break;
    case FanSpeed::Medium:
        controlPin = kFanMediumControlPin;
        break;
    case FanSpeed::High:
        controlPin = kFanHighControlPin;
        break;
    default:
        ESP_LOGE(TAG, "Invalid fan speed");
        return;
    }
    
    SendControlPulse(controlPin);
    
    // Small delay to let hardware respond
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Verify state changed by reading status pins
    FanSpeed actualSpeed = ReadCurrentSpeed();
    if (actualSpeed != speed)
    {
        ESP_LOGW(TAG, "Fan speed mismatch: expected %d, got %d", 
                 static_cast<uint8_t>(speed), static_cast<uint8_t>(actualSpeed));
    }
    
    ESP_LOGI(TAG, "Fan speed set to %d", static_cast<uint8_t>(speed));
}

void FanController::SendControlPulse(gpio_num_t pin)
{
    // Send a pulse to trigger state change
    gpio_set_level(pin, 1);
    vTaskDelay(pdMS_TO_TICKS(500)); // 500ms pulse
    gpio_set_level(pin, 0);
}

FanSpeed FanController::ReadCurrentSpeed()
{
    // Read the actual GPIO status pins to determine current speed (active-LOW: LOW=active)
    int lowLevel = gpio_get_level(kFanLowStatusPin);
    int mediumLevel = gpio_get_level(kFanMediumStatusPin);
    int highLevel = gpio_get_level(kFanHighStatusPin);
    
    if (!highLevel)  // Active-LOW: 0 = active
    {
        return FanSpeed::High;
    }
    else if (!mediumLevel)  // Active-LOW: 0 = active
    {
        return FanSpeed::Medium;
    }
    else if (!lowLevel)  // Active-LOW: 0 = active
    {
        return FanSpeed::Low;
    }
    return FanSpeed::Off;  // All pins HIGH = off
}

FanSpeed FanController::GetSpeed()
{
    // Always read from hardware for most accurate state
    mCurrentSpeed = ReadCurrentSpeed();
    return mCurrentSpeed;
}

bool FanController::IsOn()
{
    return mCurrentSpeed != FanSpeed::Off;
}

uint8_t FanController::GetPercentSpeed()
{
    switch (mCurrentSpeed)
    {
    case FanSpeed::Low:
        return 33;
    case FanSpeed::Medium:
        return 66;
    case FanSpeed::High:
        return 100;
    case FanSpeed::Off:
    default:
        return 0;
    }
}
