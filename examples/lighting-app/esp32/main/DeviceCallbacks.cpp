/*
 *
 *    Copyright (c) 2021-2023 Project CHIP Authors
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
#include "AppTask.h"

#include "DeviceCallbacks.h"
#include "DeviceManager.h"
#include "Globals.h"
#include "LEDWidget.h"
#include <app/server/Server.h>

#include <app/util/util.h>

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <app/clusters/fan-control-server/fan-control-server.h>
#include <app/reporting/reporting.h>
#include <lib/support/logging/CHIPLogging.h>

static const char TAG[] = "light-app-callbacks";

extern LEDWidget AppLED;

using namespace chip;

constexpr chip::EndpointId kLightEndpointId = 1;
constexpr chip::EndpointId kFanEndpointId = 3;
using namespace chip::Inet;
using namespace chip::System;
using namespace chip::app::Clusters;

void AppDeviceCallbacks::PostAttributeChangeCallback(EndpointId endpointId, ClusterId clusterId, AttributeId attributeId,
                                                     uint8_t type, uint16_t size, uint8_t * value)
{
    ESP_LOGI(TAG, "PostAttributeChangeCallback - Cluster ID: '0x%" PRIx32 "', EndPoint ID: '0x%x', Attribute ID: '0x%" PRIx32 "'",
             clusterId, endpointId, attributeId);

    switch (clusterId)
    {
    case OnOff::Id:
        OnOnOffPostAttributeChangeCallback(endpointId, attributeId, value);
        break;

    case LevelControl::Id:
        OnLevelControlAttributeChangeCallback(endpointId, attributeId, value);
        break;

    case FanControl::Id:
        OnFanControlAttributeChangeCallback(endpointId, attributeId, value);
        break;

#if CONFIG_LED_TYPE_RMT
    case ColorControl::Id:
        OnColorControlAttributeChangeCallback(endpointId, attributeId, value);
        break;
#endif

    default:
        ESP_LOGI(TAG, "Unhandled cluster ID: %" PRIu32, clusterId);
        break;
    }

    ESP_LOGI(TAG, "Current free heap: %u\n", static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
}

void AppDeviceCallbacks::OnOnOffPostAttributeChangeCallback(EndpointId endpointId, AttributeId attributeId, uint8_t * value)
{
    bool isOn;
    
    VerifyOrExit(attributeId == OnOff::Attributes::OnOff::Id,
                 ESP_LOGI(TAG, "Unhandled Attribute ID: '0x%" PRIx32 "'", attributeId));
    VerifyOrExit(endpointId == kLightEndpointId, ESP_LOGE(TAG, "Unexpected EndPoint ID for OnOff: `0x%02x'", endpointId));
    
    // Skip hardware update if this callback was triggered by UpdateClusterState (hardware -> Matter)
    if (GetAppTask().IsUpdatingFromHardware())
    {
        ESP_LOGI(TAG, "Skipping OnOff hardware update (triggered by hardware state sync)");
        goto exit;
    }

    isOn = *value;
    ESP_LOGI(TAG, "Light OnOff: %s", isOn ? "ON" : "OFF");
    
    if (isOn)
    {
        if (!DeviceMgr().IsLightOn())
        {
            // Default to High (100%) when turning on via voice/switch
            DeviceMgr().SetLightLevel(LightLevel::High);
        }
    }
    else
    {
        DeviceMgr().SetLightLevel(LightLevel::Off);
    }
    
    AppLED.Set(*value);

exit:
    return;
}

void AppDeviceCallbacks::OnLevelControlAttributeChangeCallback(EndpointId endpointId, AttributeId attributeId, uint8_t * value)
{
    uint8_t brightness;
    uint8_t snappedLevel;
    static bool isSnapping = false;  // Prevent recursive snapping
    bool onOffState = false;
    chip::app::DataModel::Nullable<uint8_t> currentLevel;
    
    VerifyOrExit(attributeId == LevelControl::Attributes::CurrentLevel::Id,
                 ESP_LOGI(TAG, "Unhandled Attribute ID: '0x%" PRIx32 "'", attributeId));
    VerifyOrExit(endpointId == kLightEndpointId, ESP_LOGE(TAG, "Unexpected EndPoint ID for LevelControl: `0x%02x'", endpointId));
    
    // Skip hardware update if this callback was triggered by UpdateClusterState (hardware -> Matter)
    if (GetAppTask().IsUpdatingFromHardware())
    {
        ESP_LOGI(TAG, "Skipping LevelControl hardware update (triggered by hardware state sync)");
        goto exit;
    }

    brightness = *value;
    ESP_LOGI(TAG, "Light Level requested: %d", brightness);
    
    // Check OnOff state - if light should be on at High level, ignore level=1 requests
    // This prevents the default restore level from overriding voice "turn on" commands
    chip::app::Clusters::OnOff::Attributes::OnOff::Get(kLightEndpointId, &onOffState);
    chip::app::Clusters::LevelControl::Attributes::CurrentLevel::Get(kLightEndpointId, currentLevel);
    
    // If OnOff just turned on and set level to 254 (High), but now level=1 arrives, ignore it
    // This is Matter's default restore behavior conflicting with our "turn on = High" preference
    if (onOffState && !currentLevel.IsNull() && currentLevel.Value() == 254 && brightness == 1)
    {
        ESP_LOGI(TAG, "Ignoring level=1 request (light already set to High by OnOff)");
        goto exit;
    }
    
    // Skip snapping if we're already in a snap operation (prevents recursive callbacks)
    if (isSnapping)
    {
        ESP_LOGI(TAG, "Skipping snap (recursive call from snap operation)");
        AppLED.SetBrightness(brightness);
        goto exit;
    }
    
    // Snap to discrete levels: 0% (off), 50% (low), 100% (high)
    // Matter level range: 1-254 (0 is off)
    // Snap to: 0 (off), 127 (50%), 254 (100%)
    if (brightness == 0)
    {
        snappedLevel = 0;
    }
    else if (brightness <= 190)  // 1-190 -> 50% (low)
    {
        snappedLevel = 127;  // 50% of 254
    }
    else  // 191-254 -> 100% (high)
    {
        snappedLevel = 254;  // 100%
    }
    
    // Update the cluster to reflect the snapped level if it changed (already on Matter thread)
    if (snappedLevel != brightness)
    {
        ESP_LOGI(TAG, "Snapping brightness from %d to %d", brightness, snappedLevel);
        isSnapping = true;  // Set flag to prevent recursive snap
        chip::app::Clusters::LevelControl::Attributes::CurrentLevel::Set(kLightEndpointId, snappedLevel);
        isSnapping = false;  // Clear flag
        
        // Immediately notify subscribers of the snapped value (don't wait for hardware)
        // This ensures controllers see the correct state instantly
        MatterReportingAttributeChangeCallback(kLightEndpointId, LevelControl::Id, 
                                              LevelControl::Attributes::CurrentLevel::Id);
        ESP_LOGI(TAG, "Triggered immediate attribute report for snapped value");
    }
    
    // Apply the SNAPPED level to hardware (not the original requested level)
    // This ensures the debounce timer stores the snapped value
    if (snappedLevel == 0)
    {
        DeviceMgr().SetLightLevel(LightLevel::Off);
    }
    else if (snappedLevel <= 190)  // 127 snaps to Low
    {
        DeviceMgr().SetLightLevel(LightLevel::Low);
    }
    else  // 254 snaps to High
    {
        DeviceMgr().SetLightLevel(LightLevel::High);
    }
    
    AppLED.SetBrightness(snappedLevel);

exit:
    return;
}

void AppDeviceCallbacks::OnFanControlAttributeChangeCallback(EndpointId endpointId, AttributeId attributeId, uint8_t * value)
{
    using namespace FanControl::Attributes;
    static bool isSnapping = false;  // Prevent recursive snapping
    static TimerHandle_t sFanDebounceTimer = NULL;
    static FanSpeed sPendingFanSpeed = FanSpeed::Off;
    
    VerifyOrExit(endpointId == kFanEndpointId, ESP_LOGE(TAG, "Unexpected EndPoint ID for FanControl: `0x%02x'", endpointId));
    
    // Skip hardware update if this callback was triggered by UpdateClusterState (hardware -> Matter)
    if (GetAppTask().IsUpdatingFromHardware())
    {
        ESP_LOGI(TAG, "Skipping FanControl hardware update (triggered by hardware state sync)");
        goto exit;
    }
    
    if (attributeId == FanMode::Id)
    {
        uint8_t fanMode = *value;
        ESP_LOGI(TAG, "Fan Mode requested: %d (0=Off, 1=Low, 2=Med, 3=High, 4=On, 5=Auto, 6=Smart)", fanMode);
        
        // FanMode values: Off=0, Low=1, Medium=2, High=3, On=4, Auto=5, Smart=6
        FanSpeed speed;
        uint8_t percent;
        
        switch (fanMode)
        {
            case 0:  // Off
                speed = FanSpeed::Off;
                percent = 0;
                ESP_LOGI(TAG, "FanMode Off -> 0%%");
                break;
            case 1:  // Low
                speed = FanSpeed::Low;
                percent = 33;
                ESP_LOGI(TAG, "FanMode Low -> 33%%");
                break;
            case 2:  // Medium
                speed = FanSpeed::Medium;
                percent = 66;
                ESP_LOGI(TAG, "FanMode Medium -> 66%%");
                break;
            case 3:  // High
                speed = FanSpeed::High;
                percent = 100;
                ESP_LOGI(TAG, "FanMode High -> 100%%");
                break;
            case 4:  // On (generic on - default to Low)
                speed = FanSpeed::Low;
                percent = 33;
                ESP_LOGI(TAG, "FanMode On -> 33%% (Low)");
                break;
            default:  // Auto, Smart, etc. - not supported
                ESP_LOGI(TAG, "FanMode %d not supported, ignoring", fanMode);
                goto exit;
        }
        
        // Update PercentSetting/PercentCurrent to match FanMode
        chip::app::Clusters::FanControl::Attributes::PercentSetting::Set(kFanEndpointId, percent);
        chip::app::Clusters::FanControl::Attributes::PercentCurrent::Set(kFanEndpointId, percent);
        
        // Immediately notify subscribers
        MatterReportingAttributeChangeCallback(kFanEndpointId, FanControl::Id, 
                                              FanControl::Attributes::PercentSetting::Id);
        MatterReportingAttributeChangeCallback(kFanEndpointId, FanControl::Id, 
                                              FanControl::Attributes::PercentCurrent::Id);
        
        // Apply to hardware (with debounce for on, immediate for off)
        if (speed == FanSpeed::Off)
        {
            DeviceMgr().SetFanSpeed(FanSpeed::Off);
        }
        else
        {
            // Store pending speed and reset timer (same debounce as PercentSetting)
            sPendingFanSpeed = speed;
            if (sFanDebounceTimer != NULL)
            {
                xTimerReset(sFanDebounceTimer, 0);
                ESP_LOGI(TAG, "Fan hardware update debounced (300ms), pending speed: %d", static_cast<uint8_t>(speed));
            }
            else
            {
                // Fallback if timer not created yet
                DeviceMgr().SetFanSpeed(speed);
            }
        }
    }
    else if (attributeId == PercentSetting::Id)
    {
        uint8_t percentSpeed = *value;
        uint8_t snappedPercent;
        ESP_LOGI(TAG, "Fan Speed requested: %d%%", percentSpeed);
        
        // Skip snapping if we're already in a snap operation (prevents recursive callbacks)
        if (isSnapping)
        {
            ESP_LOGI(TAG, "Skipping fan snap (recursive call from snap operation)");
            goto exit;
        }
        
        // Snap to discrete levels: 0%, 33%, 66%, 100%
        FanSpeed speed;
        if (percentSpeed == 0)
        {
            snappedPercent = 0;
            speed = FanSpeed::Off;
        }
        else if (percentSpeed <= 50)  // 1-50 -> 33% (low)
        {
            snappedPercent = 33;
            speed = FanSpeed::Low;
        }
        else if (percentSpeed <= 83)  // 51-83 -> 66% (medium)
        {
            snappedPercent = 66;
            speed = FanSpeed::Medium;
        }
        else  // 84-100 -> 100% (high)
        {
            snappedPercent = 100;
            speed = FanSpeed::High;
        }
        
        // Check if hardware already at this speed (avoid feedback loop from UpdateClusterState)
        FanSpeed currentSpeed = DeviceMgr().GetFanSpeed();
        if (currentSpeed == speed)
        {
            ESP_LOGI(TAG, "Fan already at requested speed %d, skipping hardware update", static_cast<uint8_t>(speed));
            goto exit;
        }
        
        // Apply hardware change immediately for responsive feel during slider interaction
        ESP_LOGI(TAG, "Setting fan hardware to speed: %d immediately", static_cast<uint8_t>(speed));
        DeviceMgr().SetFanSpeed(speed);
        
        // Debounce the snapping feedback to avoid jarring slider jumps on slower controllers
        // The snapped value will be reported back after the timer expires, providing smooth UX
        // Note: We don't immediately snap the Matter attribute during interaction
        ESP_LOGI(TAG, "Will report snapped value %d%% after 300ms debounce", snappedPercent);
        
        // Create timer on first use
        if (sFanDebounceTimer == NULL)
        {
            sFanDebounceTimer = xTimerCreate(
                "FanSnapDebounce",
                pdMS_TO_TICKS(300),  // 300ms debounce for snapping feedback
                pdFALSE,             // One-shot timer
                NULL,
                [](TimerHandle_t timer) {
                    // When timer expires, trigger state sync which will report the snapped value
                    ESP_LOGI(TAG, "Fan snap debounce timer expired, syncing state");
                    chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
                        GetAppTask().UpdateClusterState();
                    });
                }
            );
        }
        
        // Store pending speed for potential use and reset timer
        sPendingFanSpeed = speed;
        if (sFanDebounceTimer != NULL)
        {
            xTimerReset(sFanDebounceTimer, 0);
        }
    }
    else
    {
        ESP_LOGI(TAG, "Unhandled FanControl Attribute ID: '0x%" PRIx32 "'", attributeId);
    }

exit:
    return;
}

#if CONFIG_LED_TYPE_RMT
void AppDeviceCallbacks::OnColorControlAttributeChangeCallback(EndpointId endpointId, AttributeId attributeId, uint8_t * value)
{
    using namespace ColorControl::Attributes;
    uint8_t hue, saturation;

    VerifyOrExit(attributeId == CurrentHue::Id || attributeId == CurrentSaturation::Id,
                 ESP_LOGI(TAG, "Unhandled AttributeId ID: '0x%" PRIx32 "'", attributeId));
    VerifyOrExit(endpointId == 1, ESP_LOGE(TAG, "Unexpected EndPoint ID: `0x%02x'", endpointId));

    if (attributeId == CurrentHue::Id)
    {
        hue = *value;
        CurrentSaturation::Get(endpointId, &saturation);
    }
    else
    {
        saturation = *value;
        CurrentHue::Get(endpointId, &hue);
    }
    AppLED.SetColor(hue, saturation);

exit:
    return;
}
#endif // CONFIG_LED_TYPE_RMT

void emberAfOnOffClusterInitCallback(EndpointId endpoint)
{
    ESP_LOGI(TAG, "emberAfOnOffClusterInitCallback");
    GetAppTask().UpdateClusterState();
}

void AppDeviceCallbacksDelegate::OnIPv4ConnectivityEstablished()
{
    wifiLED.Set(true);
}

void AppDeviceCallbacksDelegate::OnIPv4ConnectivityLost()
{
    wifiLED.Set(false);
}
