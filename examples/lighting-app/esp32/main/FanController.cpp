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

// ISR: fires on fan button press (rising or falling edge depending on circuit).
// Mirrors the LightController ISR approach with debounce.
void IRAM_ATTR FanController::FanButtonISR(void * arg)
{
    FanController * self = static_cast<FanController *>(arg);

    // Hardware debounce: ignore edges within kDebounceMs of the previous one
    uint32_t now = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;
    if ((now - self->mLastInterruptTime) < kDebounceMs)
    {
        return;
    }
    self->mLastInterruptTime = now;

    // Notify the AppTask (stored during Init)
    if (self->mTaskHandle != nullptr)
    {
        BaseType_t higherPriorityTaskWoken = pdFALSE;
        vTaskNotifyGiveFromISR(self->mTaskHandle, &higherPriorityTaskWoken);
        portYIELD_FROM_ISR(higherPriorityTaskWoken);
    }
}

void FanController::Init()
{
    // Store the calling task handle (AppTask) so the ISR can notify it
    mTaskHandle = xTaskGetCurrentTaskHandle();
    mLastInterruptTime = 0;
    mLastProcessedTime = 0;

    // Configure control pin as output, default LOW
    gpio_config_t out_conf  = {};
    out_conf.intr_type      = GPIO_INTR_DISABLE;
    out_conf.mode           = GPIO_MODE_OUTPUT;
    out_conf.pin_bit_mask   = (1ULL << kFanControlPin);
    out_conf.pull_down_en   = GPIO_PULLDOWN_DISABLE;
    out_conf.pull_up_en     = GPIO_PULLUP_DISABLE;
    gpio_config(&out_conf);
    gpio_set_level(kFanControlPin, 0);

    // Configure LED status pin as input with pull-up (active-LOW: LOW = fan ON)
    gpio_config_t status_conf  = {};
    status_conf.intr_type      = GPIO_INTR_DISABLE;
    status_conf.mode           = GPIO_MODE_INPUT;
    status_conf.pin_bit_mask   = (1ULL << kFanLEDStatusPin);
    status_conf.pull_down_en   = GPIO_PULLDOWN_DISABLE;
    status_conf.pull_up_en     = GPIO_PULLUP_ENABLE;
    esp_err_t err = gpio_config(&status_conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure LED status pin: %d", err);
    }

    // Configure interrupt pin as input with pull-up, trigger on any edge
    gpio_config_t int_conf  = {};
    int_conf.intr_type      = GPIO_INTR_ANYEDGE;
    int_conf.mode           = GPIO_MODE_INPUT;
    int_conf.pin_bit_mask   = (1ULL << kFanInterruptPin);
    int_conf.pull_down_en   = GPIO_PULLDOWN_DISABLE;
    int_conf.pull_up_en     = GPIO_PULLUP_ENABLE;
    err = gpio_config(&int_conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure interrupt pin: %d", err);
    }

    // ISR service should already be installed by AppTask (commissioning button)
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %d", err);
    }

    err = gpio_isr_handler_add(kFanInterruptPin, FanButtonISR, this);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %d", err);
    }

    // Read initial state from LED
    mCurrentSpeed = ReadLEDStatus() ? FanSpeed::High : FanSpeed::Off;
    ESP_LOGI(TAG, "FanController initialized. LED status pin GPIO%d, fan is %s",
             kFanLEDStatusPin, mCurrentSpeed == FanSpeed::High ? "ON" : "OFF");
}

void FanController::SetSpeed(FanSpeed speed)
{
    // Only Off and High are supported
    if (speed != FanSpeed::Off && speed != FanSpeed::High)
    {
        ESP_LOGW(TAG, "Unsupported fan speed %d, clamping to High", static_cast<uint8_t>(speed));
        speed = FanSpeed::High;
    }

    bool targetOn   = (speed == FanSpeed::High);
    bool currentOn  = ReadLEDStatus();

    if (targetOn == currentOn)
    {
        ESP_LOGI(TAG, "Fan already in requested state (%s), skipping pulse",
                 targetOn ? "ON" : "OFF");
        mCurrentSpeed = speed;
        return;
    }

    ESP_LOGI(TAG, "Sending control pulse to set fan %s", targetOn ? "ON" : "OFF");
    SendControlPulse();

    // Allow hardware to settle then verify via LED
    vTaskDelay(pdMS_TO_TICKS(200));
    bool actualOn = ReadLEDStatus();
    if (actualOn != targetOn)
    {
        ESP_LOGW(TAG, "Fan state mismatch after pulse: expected %s, LED reads %s",
                 targetOn ? "ON" : "OFF", actualOn ? "ON" : "OFF");
    }

    mCurrentSpeed = actualOn ? FanSpeed::High : FanSpeed::Off;
    ESP_LOGI(TAG, "Fan speed set to: %s", mCurrentSpeed == FanSpeed::High ? "High/ON" : "Off");
}

void FanController::SendControlPulse()
{
    gpio_set_level(kFanControlPin, 1);
    vTaskDelay(pdMS_TO_TICKS(kPulseDurationMs));
    gpio_set_level(kFanControlPin, 0);
}

bool FanController::ReadLEDStatus()
{
    // Active-LOW: GPIO LOW means fan is ON
    return gpio_get_level(kFanLEDStatusPin) == 0;
}

// Called from the main AppTask loop (non-blocking, mirrors LightController::NotifyStateChange)
void FanController::NotifyStateChange()
{
    // Non-blocking check for pending ISR notification
    if (ulTaskNotifyTake(pdFALSE, 0) == 0)
    {
        return; // No notification pending
    }

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if ((now - mLastProcessedTime) < kProcessCooldownMs)
    {
        ESP_LOGI(TAG, "Fan button press ignored (cooldown: %lu ms since last)",
                 now - mLastProcessedTime);
        return;
    }
    mLastProcessedTime = now;

    // Read LED to determine new state
    bool isOn     = ReadLEDStatus();
    FanSpeed prev = mCurrentSpeed;
    mCurrentSpeed = isOn ? FanSpeed::High : FanSpeed::Off;

    ESP_LOGI(TAG, "Fan button pressed: state %s -> %s",
             prev == FanSpeed::High ? "ON" : "OFF",
             mCurrentSpeed == FanSpeed::High ? "ON" : "OFF");

    if (mStateChangeCallback)
    {
        mStateChangeCallback();
    }
}

FanSpeed FanController::GetSpeed()
{
    mCurrentSpeed = ReadLEDStatus() ? FanSpeed::High : FanSpeed::Off;
    return mCurrentSpeed;
}

bool FanController::IsOn()
{
    return ReadLEDStatus();
}

uint8_t FanController::GetPercentSpeed()
{
    return IsOn() ? 100 : 0;
}
