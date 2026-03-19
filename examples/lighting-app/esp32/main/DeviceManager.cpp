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

#include "DeviceManager.h"
#include "esp_log.h"

static const char TAG[] = "DeviceManager";

DeviceManager DeviceManager::sDeviceManager;

CHIP_ERROR DeviceManager::Init()
{
    ESP_LOGI(TAG, "Initializing DeviceManager");
    
    mFanController.Init();
    mLightController.Init();
    
    // Forward fan button-press state changes to the main callback
    mFanController.SetStateChangeCallback([this]() {
        if (mStateChangeCallback)
        {
            mStateChangeCallback();
        }
    });

    // Forward light button-press state changes to the main callback
    mLightController.SetStateChangeCallback([this]() {
        if (mStateChangeCallback)
        {
            mStateChangeCallback();
        }
    });

    ESP_LOGI(TAG, "DeviceManager initialized successfully");
    return CHIP_NO_ERROR;
}

void DeviceManager::SetFanSpeed(FanSpeed speed)
{
    mFanController.SetSpeed(speed);
    
    if (mStateChangeCallback)
    {
        mStateChangeCallback();
    }
}

FanSpeed DeviceManager::GetFanSpeed()
{
    return mFanController.GetSpeed();
}

bool DeviceManager::IsFanOn()
{
    return mFanController.IsOn();
}

uint8_t DeviceManager::GetFanPercentSpeed()
{
    return mFanController.GetPercentSpeed();
}

void DeviceManager::SetLightLevel(LightLevel level)
{
    // Use debounced version to handle rapid controller commands (e.g., slider movements)
    // This prevents multiple hardware cycles when user drags slider from 0->100%
    // The recursive snapping issue is now fixed with static flags in callbacks
    mLightController.SetLevelDebounced(level);
}

LightLevel DeviceManager::GetLightLevel()
{
    return mLightController.GetLevel();
}

bool DeviceManager::IsLightOn()
{
    return mLightController.IsOn();
}

uint8_t DeviceManager::GetLightBrightness()
{
    return mLightController.GetBrightness();
}

void DeviceManager::SetStateChangeCallback(StateChangeCallback_fn callback)
{
    mStateChangeCallback = callback;
}
