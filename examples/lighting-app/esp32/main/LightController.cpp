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

#include "LightController.h"
#include "AppTask.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <app-common/zap-generated/attributes/Accessors.h>
#include <platform/CHIPDeviceLayer.h>

static const char TAG[] = "LightController";

void IRAM_ATTR LightController::StateChangeISR(void * arg)
{
    LightController * controller = static_cast<LightController *>(arg);
    if (controller == nullptr)
    {
        ESP_EARLY_LOGI(TAG, "ISR: controller is null");
        return;
    }
    
    ESP_EARLY_LOGI(TAG, "ISR: Button interrupt fired!");
    
    // Debounce: check if enough time has passed since last interrupt
    uint32_t currentTime = xTaskGetTickCountFromISR();
    uint32_t timeSinceLastInterrupt = currentTime - controller->mLastInterruptTime;
    
    // Convert debounce time to ticks
    uint32_t debounceTicks = pdMS_TO_TICKS(kDebounceMs);
    
    if (timeSinceLastInterrupt < debounceTicks)
    {
        // Ignore this interrupt - too soon after previous one
        ESP_EARLY_LOGI(TAG, "ISR: Debounced (too soon)");
        return;
    }
    
    // Update last interrupt time
    controller->mLastInterruptTime = currentTime;
    
    // Notify the controller's task (ISR-safe)
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    if (controller->mTaskHandle != nullptr)
    {
        vTaskNotifyGiveFromISR(controller->mTaskHandle, &higherPriorityTaskWoken);
        portYIELD_FROM_ISR(higherPriorityTaskWoken);
        ESP_EARLY_LOGI(TAG, "ISR: Task notified");
    }
    else
    {
        ESP_EARLY_LOGI(TAG, "ISR: Task handle is null");
    }
}

void IRAM_ATTR LightController::StatusLEDChangeISR(void * arg)
{
    LightController * controller = static_cast<LightController *>(arg);
    if (controller == nullptr)
    {
        ESP_EARLY_LOGI(TAG, "Status ISR: controller is null");
        return;
    }
    
    ESP_EARLY_LOGI(TAG, "Status ISR: LED state changed!");
    
    // Debounce: check if enough time has passed since last interrupt
    uint32_t currentTime = xTaskGetTickCountFromISR();
    uint32_t timeSinceLastInterrupt = currentTime - controller->mLastInterruptTime;
    
    // Convert debounce time to ticks
    uint32_t debounceTicks = pdMS_TO_TICKS(kDebounceMs);
    
    if (timeSinceLastInterrupt < debounceTicks)
    {
        // Ignore this interrupt - too soon after previous one
        ESP_EARLY_LOGI(TAG, "Status ISR: Debounced (too soon)");
        return;
    }
    
    // Update last interrupt time
    controller->mLastInterruptTime = currentTime;
    
    // Notify the controller's task (ISR-safe)
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    if (controller->mTaskHandle != nullptr)
    {
        vTaskNotifyGiveFromISR(controller->mTaskHandle, &higherPriorityTaskWoken);
        portYIELD_FROM_ISR(higherPriorityTaskWoken);
        ESP_EARLY_LOGI(TAG, "Status ISR: Task notified");
    }
    else
    {
        ESP_EARLY_LOGI(TAG, "Status ISR: Task handle is null");
    }
}

bool LightController::Init()
{
    ESP_LOGI(TAG, "Initializing LightController");
    mTargetLevel       = LightLevel::Off;
    mPowerStatus       = false;
    mDebounceTimer     = NULL;
    mLastInterruptTime = 0;
    mLastProcessedTime = 0;
    mUpdatingFromHardware = false;
    mStartupComplete = false;
    mLevelWhenDebounceStarted = LightLevel::Off;
    mHasPendingCycleCommand = false;
    
    mSyncMutex = xSemaphoreCreateMutex();
    if (mSyncMutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create sync mutex");
        return false;
    }
    
    // Create debounce timer for Matter attribute changes
    mDebounceTimer = xTimerCreate("LightDebounce", pdMS_TO_TICKS(kMatterDebounceMs),
                                   pdFALSE, this, DebounceTimerCallback);
    if (mDebounceTimer == NULL)
    {
        ESP_LOGE(TAG, "Failed to create debounce timer");
        return false;
    }
    mPendingLevel = mCurrentLevel;
    
    mTaskHandle        = xTaskGetCurrentTaskHandle();
    mLastInterruptTime = 0;

    // Configure control pin as output
    gpio_config_t out_conf = {};
    out_conf.intr_type     = GPIO_INTR_DISABLE;
    out_conf.mode          = GPIO_MODE_OUTPUT;
    out_conf.pin_bit_mask  = (1ULL << kLightControlPin);
    out_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    out_conf.pull_up_en    = GPIO_PULLUP_DISABLE;
    gpio_config(&out_conf);
    gpio_set_level(kLightControlPin, 0);

    // Configure interrupt pin (optocoupler inverts signal - button pull to ground activates opto, GPIO sees rising edge)
    gpio_config_t int_conf = {};
    int_conf.intr_type     = GPIO_INTR_POSEDGE; // Trigger on rising edge (button activates optocoupler, GPIO goes high)
    int_conf.mode          = GPIO_MODE_INPUT;
    int_conf.pin_bit_mask  = (1ULL << kLightInterruptPin);
    int_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    int_conf.pull_up_en    = GPIO_PULLUP_ENABLE; // Internal pull-up keeps pin high when idle
    gpio_config(&int_conf);
    
    // Configure status pin (active LOW - pulled to ground when light is on)
    // NOW WITH INTERRUPT to detect LED state changes automatically
    gpio_config_t status_conf = {};
    status_conf.intr_type     = GPIO_INTR_ANYEDGE;  // Trigger on both rising and falling edges
    status_conf.mode          = GPIO_MODE_INPUT;
    status_conf.pin_bit_mask  = (1ULL << kLightPowerStatusPin);
    status_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    status_conf.pull_up_en    = GPIO_PULLUP_ENABLE;  // Pull-up makes pin HIGH when not driven
    gpio_config(&status_conf);

    // Install ISR service and add handlers
    gpio_install_isr_service(0);
    gpio_isr_handler_add(kLightInterruptPin, StateChangeISR, this);
    gpio_isr_handler_add(kLightPowerStatusPin, StatusLEDChangeISR, this);

    ESP_LOGI(TAG, "Light controller initialized");
    ESP_LOGI(TAG, "Interrupt pin (GPIO %d) initial level: %d", kLightInterruptPin, gpio_get_level(kLightInterruptPin));
    ESP_LOGI(TAG, "Status pin (GPIO %d) initial level: %d", kLightPowerStatusPin, gpio_get_level(kLightPowerStatusPin));

    // Synchronize state on startup
    SynchronizeOnStartup();
    
    ESP_LOGI(TAG, "LightController initialized successfully");
    return true;
}

void LightController::SendPulse()
{
    ESP_LOGI(TAG, "Sending 500ms pulse on GPIO %d", kLightControlPin);
    
    // Send a 500ms pulse
    gpio_set_level(kLightControlPin, 1);
    vTaskDelay(pdMS_TO_TICKS(kPulseDurationMs));
    gpio_set_level(kLightControlPin, 0);
    
    ESP_LOGI(TAG, "Pulse complete, waiting %dms for state change", kStateChangeTimeoutMs);
    
    // Wait for state change notification
    vTaskDelay(pdMS_TO_TICKS(kStateChangeTimeoutMs));
}

void LightController::NotifyStateChange()
{
    // Check for task notification with NO WAIT (non-blocking)
    uint32_t notificationValue = ulTaskNotifyTake(pdTRUE, 0);
    
    if (notificationValue > 0)
    {
        // FIRST: Check if we're in the middle of a Matter-initiated cycle
        // If so, these ISRs are responses to OUR control pulses, not button presses
        if (mUpdatingFromHardware)
        {
            ESP_LOGI(TAG, "Ignoring ISR during Matter cycle (not a button press)");
            // Clear any other pending notifications
            while (ulTaskNotifyTake(pdTRUE, 0) > 0) {}
            return;
        }
        
        // Check cooldown period - ignore button presses too soon after processing
        uint32_t now = xTaskGetTickCount();
        if (mLastProcessedTime > 0)
        {
            uint32_t timeSinceLastMs = (now - mLastProcessedTime) * portTICK_PERIOD_MS;
            if (timeSinceLastMs < kProcessCooldownMs)
            {
                ESP_LOGI(TAG, "Button press ignored (cooldown: %lu ms since last)", timeSinceLastMs);
                // Clear any other pending notifications
                while (ulTaskNotifyTake(pdTRUE, 0) > 0) {}
                return;
            }
        }
        
        // Hardware interrupt detected
        ESP_LOGI(TAG, "Hardware interrupt: button pressed");
        
        // Clear any additional notifications that came in during processing
        while (ulTaskNotifyTake(pdTRUE, 0) > 0)
        {
            ESP_LOGI(TAG, "Cleared duplicate notification");
        }
        
        // Brief delay to let hardware stabilize
        vTaskDelay(pdMS_TO_TICKS(kPowerReadDelayMs));
        
        // If there's a pending debounced level, cancel it - button press is a manual override
        if (mDebounceTimer != NULL && xTimerIsTimerActive(mDebounceTimer))
        {
            ESP_LOGI(TAG, "Canceling pending level %d (button press overrides controller)",
                     static_cast<uint8_t>(mPendingLevel));
            xTimerStop(mDebounceTimer, 0);  // Cancel the timer
        }
        
        // Read power status (only tells us ON/OFF, not Low vs High)
        bool wasPowerOn = mPowerStatus;
        LightLevel oldLevel = mCurrentLevel;
        ReadPowerStatus();
        
        // Determine new level based on cycling: OFF → HIGH → LOW → OFF
        // Status pin only tells ON/OFF, so we track level based on cycles
        if (wasPowerOn && !mPowerStatus)
        {
            // Transitioned from ON to OFF
            mCurrentLevel = LightLevel::Off;
            ESP_LOGI(TAG, "Button press: Cycled to OFF");
        }
        else if (!wasPowerOn && mPowerStatus)
        {
            // Transitioned from OFF to ON - must be HIGH (first brightness level)
            mCurrentLevel = LightLevel::High;
            ESP_LOGI(TAG, "Button press: Cycled from OFF to HIGH");
        }
        else if (wasPowerOn && mPowerStatus)
        {
            // Still ON - must have cycled from HIGH to LOW
            if (oldLevel == LightLevel::High)
            {
                mCurrentLevel = LightLevel::Low;
                ESP_LOGI(TAG, "Button press: Cycled from HIGH to LOW");
            }
            else
            {
                // Shouldn't happen, but handle it
                ESP_LOGW(TAG, "Button press: Power stayed ON but level was %d", static_cast<uint8_t>(oldLevel));
                mCurrentLevel = LightLevel::Low;
            }
        }
        
        ESP_LOGI(TAG, "Hardware state: Power=%s, Level=%d (%s)",
                 mPowerStatus ? "ON" : "OFF", static_cast<uint8_t>(mCurrentLevel),
                 mCurrentLevel == LightLevel::Off ? "OFF" : (mCurrentLevel == LightLevel::Low ? "LOW" : "HIGH"));
        
        // Update last processed time
        mLastProcessedTime = xTaskGetTickCount();
        
        // Schedule Matter cluster update
        ESP_LOGI(TAG, "Scheduling Matter cluster update");
        chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
            GetAppTask().UpdateClusterState();
        });
    }
}

void LightController::ReadPowerStatus()
{
    // Active LOW: pin is pulled to ground when light is ON
    mPowerStatus = (gpio_get_level(kLightPowerStatusPin) == 0);
    
    if (!mPowerStatus)
    {
        mCurrentLevel = LightLevel::Off;
    }
    
    ESP_LOGI(TAG, "Power status: %s (GPIO=%d)", mPowerStatus ? "ON" : "OFF", gpio_get_level(kLightPowerStatusPin));
}

void LightController::SynchronizeOnStartup()
{
    ESP_LOGI(TAG, "Synchronizing light state on startup");
    
    // Block NotifyStateChange() during sync
    mUpdatingFromHardware = true;
    
    // Read current power status
    ReadPowerStatus();
    
    if (mPowerStatus)
    {
        ESP_LOGI(TAG, "Light is ON at startup - cycling until OFF");
        
        // Keep pulsing until status pin shows OFF
        // Cycle: Off→High→Low→Off (3 states, max 3 pulses needed)
        int pulseCount = 0;
        const int maxPulses = 3;  // Safety limit
        
        while (mPowerStatus && pulseCount < maxPulses)
        {
            pulseCount++;
            ESP_LOGI(TAG, "Sync pulse %d: Sending...", pulseCount);
            SendPulse();
            
            // Wait for hardware to settle
            vTaskDelay(pdMS_TO_TICKS(kPowerReadDelayMs));
            
            // Check if we reached OFF
            ReadPowerStatus();
            ESP_LOGI(TAG, "Sync pulse %d: Status is now %s", pulseCount, mPowerStatus ? "ON" : "OFF");
        }
        
        if (mPowerStatus)
        {
            ESP_LOGE(TAG, "SYNC FAILED: Still ON after %d pulses - hardware may be stuck!", pulseCount);
        }
        else
        {
            ESP_LOGI(TAG, "Sync complete: Reached OFF after %d pulse%s", pulseCount, pulseCount == 1 ? "" : "s");
        }
    }
    else
    {
        ESP_LOGI(TAG, "Light is already OFF at startup");
    }
    
    // Set software state to match hardware
    mCurrentLevel = LightLevel::Off;
    
    // CRITICAL: Drain all ISR notifications from sync pulses
    // These were hardware responses to OUR control pulses, not button presses
    int clearedCount = 0;
    while (ulTaskNotifyTake(pdTRUE, 0) > 0)
    {
        clearedCount++;
    }
    if (clearedCount > 0)
    {
        ESP_LOGI(TAG, "Cleared %d ISR notification%s from sync", clearedCount, clearedCount == 1 ? "" : "s");
    }
    
    // Cancel any pending debounce timers from initialization
    if (mDebounceTimer != NULL && xTimerIsTimerActive(mDebounceTimer))
    {
        ESP_LOGI(TAG, "Canceling pending debounce timer from initialization");
        xTimerStop(mDebounceTimer, 0);
    }
    
    // Clear the cycle flag but DON'T set mStartupComplete yet
    // This allows button presses but blocks Matter changes until post-startup update completes
    mUpdatingFromHardware = false;
    
    ESP_LOGI(TAG, "Startup sync complete - device ready for button presses");
    
    // Schedule Matter cluster update to match actual hardware state
    // This will set mStartupComplete when done
    ESP_LOGI(TAG, "Scheduling post-startup Matter update to match hardware");
    LightController * controller = this;
    chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t arg) {
        vTaskDelay(pdMS_TO_TICKS(500));  // Wait for Matter init to complete
        GetAppTask().UpdateClusterState();
        // NOW it's safe to accept Matter changes
        LightController * ctrl = reinterpret_cast<LightController *>(arg);
        ctrl->mStartupComplete = true;
        ESP_LOGI(TAG, "Post-startup Matter update complete - accepting Matter changes");
    }, reinterpret_cast<intptr_t>(controller));
}

void LightController::CycleToTargetLevel()
{
    if (xSemaphoreTake(mSyncMutex, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return;
    }

    // Block NotifyStateChange() from processing GPIO changes during cycling
    // This prevents treating hardware responses to our control pulses as button presses
    mUpdatingFromHardware = true;

    const int kMaxPulses = 3; // Cycle has 3 states, so 3 pulses always guarantees off
    int pulseCount = 0;

    if (mTargetLevel == LightLevel::Off)
    {
        // We cannot reliably know if we are at Low or High, so we cannot calculate a
        // fixed pulse count. Instead, pulse until the status pin confirms off.
        ReadPowerStatus();
        while (mPowerStatus && pulseCount < kMaxPulses)
        {
            pulseCount++;
            ESP_LOGI(TAG, "Off pulse %d: sending...", pulseCount);
            SendPulse();
            vTaskDelay(pdMS_TO_TICKS(kPowerReadDelayMs));
            ReadPowerStatus();
            ESP_LOGI(TAG, "Off pulse %d: status is now %s", pulseCount, mPowerStatus ? "ON" : "OFF");
        }

        if (mPowerStatus)
        {
            ESP_LOGE(TAG, "Failed to turn off after %d pulses - hardware may be stuck!", pulseCount);
        }
        else
        {
            ESP_LOGI(TAG, "Reached OFF after %d pulse%s", pulseCount, pulseCount == 1 ? "" : "s");
        }
        mCurrentLevel = LightLevel::Off;
    }
    else
    {
        // For an On target (Low or High), first pulse-to-off so we have a known
        // starting position, then send the exact number of pulses to reach the target.
        // Cycle order from Off: Off → High (1 pulse) → Low (2 pulses)
        ReadPowerStatus();
        while (mPowerStatus && pulseCount < kMaxPulses)
        {
            pulseCount++;
            ESP_LOGI(TAG, "Pre-off pulse %d: sending to reach known Off state", pulseCount);
            SendPulse();
            vTaskDelay(pdMS_TO_TICKS(kPowerReadDelayMs));
            ReadPowerStatus();
        }

        if (mPowerStatus)
        {
            ESP_LOGE(TAG, "Could not reach Off before cycling to target - aborting");
            mUpdatingFromHardware = false;
            xSemaphoreGive(mSyncMutex);
            return;
        }

        // Now in confirmed Off state; send the required pulses for target
        uint8_t pulsesForTarget = (mTargetLevel == LightLevel::High) ? 1 : 2;
        ESP_LOGI(TAG, "At confirmed Off; sending %d pulse%s to reach level %d",
                 pulsesForTarget, pulsesForTarget == 1 ? "" : "s",
                 static_cast<uint8_t>(mTargetLevel));

        for (uint8_t i = 0; i < pulsesForTarget; i++)
        {
            SendPulse();
        }

        vTaskDelay(pdMS_TO_TICKS(kPowerReadDelayMs));
        ReadPowerStatus();

        if (!mPowerStatus)
        {
            ESP_LOGW(TAG, "Expected On state after cycling but power is off - trusting hardware");
            mCurrentLevel = LightLevel::Off;
        }
        else
        {
            mCurrentLevel = mTargetLevel;
            ESP_LOGI(TAG, "Reached level %d", static_cast<uint8_t>(mCurrentLevel));
        }
    }

    // Clear the flag - allow NotifyStateChange() to process button presses again
    mUpdatingFromHardware = false;
    
    // Check if there's a pending level change that arrived while we were cycling
    if (mDebounceTimer != NULL && xTimerIsTimerActive(mDebounceTimer))
    {
        ESP_LOGI(TAG, "Cycle complete, pending level %d will be applied by timer", static_cast<uint8_t>(mPendingLevel));
    }
    
    xSemaphoreGive(mSyncMutex);
    
    // Notify Matter that hardware state changed (for Matter-initiated changes)
    if (mStateChangeCallback)
    {
        ESP_LOGI(TAG, "Notifying Matter of completed level change");
        mStateChangeCallback();
    }
}

void LightController::SetLevel(LightLevel level)
{
    ESP_LOGI(TAG, "Setting light level to %d", static_cast<uint8_t>(level));
    
    mTargetLevel = level;
    
    // Don't cycle hardware during startup - let hardware state take priority
    if (!mStartupComplete) {
        ESP_LOGI(TAG, "Ignoring Matter change during startup (hardware state has priority)");
        return;
    }
    
    // Don't cycle hardware if button is currently updating (prevents race condition)
    if (mUpdatingFromHardware) {
        ESP_LOGI(TAG, "Ignoring Matter change (hardware button is updating)");
        return;
    }
    
    // Cycle hardware to target level
    CycleToTargetLevel();
}

void LightController::SetLevelDebounced(LightLevel level)
{
    ESP_LOGI(TAG, "Debounced level change requested: %d (timer=%p)", static_cast<uint8_t>(level), mDebounceTimer);
    
    // Don't accept new commands while a cycle is in progress
    // Instead, store the pending level to apply after current cycle completes
    if (mUpdatingFromHardware)
    {
        ESP_LOGI(TAG, "Cycle in progress, storing pending level %d to apply after", static_cast<uint8_t>(level));
        mPendingLevel = level;
        mHasPendingCycleCommand = true;  // Flag that we have a pending command
        // Reset timer to apply this level after current cycle completes
        if (mDebounceTimer != NULL)
        {
            xTimerReset(mDebounceTimer, 0);
        }
        return;
    }
    
    // Don't check if already at level - software/hardware can get out of sync
    // Let CycleToTargetLevel handle the actual position check
    
    // OFF commands are applied immediately (don't debounce)
    // This prevents subsequent restore-level calls from overriding OFF
    if (level == LightLevel::Off)
    {
        ESP_LOGI(TAG, "OFF command - applying immediately without debounce");
        // Cancel any pending timer
        if (mDebounceTimer != NULL && xTimerIsTimerActive(mDebounceTimer))
        {
            xTimerStop(mDebounceTimer, 0);
        }
        SetLevel(level);
        return;
    }
    
    // Check the OnOff cluster state to see if the light should be on
    // Use endpoint 1 (light endpoint)
    bool onOffState = false;
    chip::app::Clusters::OnOff::Attributes::OnOff::Get(1, &onOffState);
    
    // If OnOff cluster says OFF, ignore level changes (store them but don't cycle hardware)
    // This prevents "restore previous level" commands from turning the light back on
    if (!onOffState)
    {
        ESP_LOGI(TAG, "OnOff cluster is OFF, storing level %d but not cycling hardware", static_cast<uint8_t>(level));
        // Just update the target so when it's turned on, it goes to this level
        mTargetLevel = level;
        // Cancel any pending timer
        if (mDebounceTimer != NULL && xTimerIsTimerActive(mDebounceTimer))
        {
            xTimerStop(mDebounceTimer, 0);
        }
        return;
    }
    
    // If timer not ready yet (early initialization), apply immediately
    if (mDebounceTimer == NULL)
    {
        ESP_LOGI(TAG, "Timer not ready, applying immediately");
        SetLevel(level);
        return;
    }
    
    // Store pending level and current level when starting timer
    mPendingLevel = level;
    mLevelWhenDebounceStarted = mCurrentLevel;
    
    // Restart timer (cancels previous if running)
    BaseType_t result = xTimerReset(mDebounceTimer, 0);
    ESP_LOGI(TAG, "Timer reset result: %d (1=success)", result);
}

void LightController::DebounceTimerCallback(TimerHandle_t timer)
{
    LightController * controller = static_cast<LightController *>(pvTimerGetTimerID(timer));
    if (controller)
    {
        // If this is a pending command from a cycle, skip the button-press check
        if (!controller->mHasPendingCycleCommand)
        {
            // Check if level changed while we were waiting (button was pressed)
            if (controller->mCurrentLevel != controller->mLevelWhenDebounceStarted)
            {
                ESP_LOGI(TAG, "Debounce timer expired, but level changed from %d to %d (button pressed) - ignoring pending %d",
                         static_cast<uint8_t>(controller->mLevelWhenDebounceStarted),
                         static_cast<uint8_t>(controller->mCurrentLevel),
                         static_cast<uint8_t>(controller->mPendingLevel));
                return;
            }
        }
        else
        {
            ESP_LOGI(TAG, "Debounce timer expired for pending cycle command, applying level: %d", 
                     static_cast<uint8_t>(controller->mPendingLevel));
            controller->mHasPendingCycleCommand = false;  // Clear the flag
        }
        
        ESP_LOGI(TAG, "Debounce timer expired, applying level: %d", static_cast<uint8_t>(controller->mPendingLevel));
        controller->SetLevel(controller->mPendingLevel);
    }
}

LightLevel LightController::GetLevel()
{
    // Always check power status first (active LOW - 0 = ON)
    if (gpio_get_level(kLightPowerStatusPin) != 0)
    {
        return LightLevel::Off;
    }
    return mCurrentLevel;
}

bool LightController::IsOn()
{
    return mCurrentLevel != LightLevel::Off && mPowerStatus;
}

uint8_t LightController::GetBrightness()
{
    switch (mCurrentLevel)
    {
    case LightLevel::Low:
        return 127; // 50% (matches snapped value)
    case LightLevel::High:
        return 254; // 100% (matches snapped value, not 255)
    case LightLevel::Off:
    default:
        return 0;
    }
}
