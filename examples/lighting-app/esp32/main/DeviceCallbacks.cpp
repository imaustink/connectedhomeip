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

void AppDeviceCallbacks::OnFanControlAttributeChangeCallback(EndpointId endpointId, AttributeId attributeId, uint8_t * value)
{
    using namespace FanControl::Attributes;
    static bool isSnapping = false;  // Prevent recursive snapping
    
    VerifyOrExit(endpointId == kFanEndpointId, ESP_LOGE(TAG, "Unexpected EndPoint ID for FanControl: `0x%02x'", endpointId));
    
    if (attributeId == PercentSetting::Id)
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
        
        // Update the cluster to reflect the snapped speed if it changed (already on Matter thread)
        if (snappedPercent != percentSpeed)
        {
            ESP_LOGI(TAG, "Snapping fan speed from %d%% to %d%%", percentSpeed, snappedPercent);
            isSnapping = true;  // Set flag to prevent recursive snap
            chip::app::Clusters::FanControl::Attributes::PercentSetting::Set(kFanEndpointId, snappedPercent);
            chip::app::Clusters::FanControl::Attributes::PercentCurrent::Set(kFanEndpointId, snappedPercent);
            isSnapping = false;  // Clear flag
        }
        
        ESP_LOGI(TAG, "Setting fan hardware to speed: %d", static_cast<uint8_t>(speed));
        DeviceMgr().SetFanSpeed(speed);
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
