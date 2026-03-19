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

#include "FanController.h"
#include "LightController.h"

#include <stdbool.h>
#include <stdint.h>

#include <functional>

#include <lib/core/CHIPError.h>

class DeviceManager
{
public:
    CHIP_ERROR Init();
    
    // Fan control
    void SetFanSpeed(FanSpeed speed);
    FanSpeed GetFanSpeed();
    bool IsFanOn();
    uint8_t GetFanPercentSpeed();
    
    // Fan accessor (for ISR notification polling in AppTask)
    FanController & GetFanController() { return mFanController; }

    // Light control
    void SetLightLevel(LightLevel level);
    LightLevel GetLightLevel();
    bool IsLightOn();
    uint8_t GetLightBrightness();
    LightController & GetLightController() { return mLightController; }

    using StateChangeCallback_fn = std::function<void()>;
    void SetStateChangeCallback(StateChangeCallback_fn callback);

private:
    friend DeviceManager & DeviceMgr(void);
    
    FanController mFanController;
    LightController mLightController;
    StateChangeCallback_fn mStateChangeCallback;
    
    static DeviceManager sDeviceManager;
};

inline DeviceManager & DeviceMgr(void)
{
    return DeviceManager::sDeviceManager;
}
