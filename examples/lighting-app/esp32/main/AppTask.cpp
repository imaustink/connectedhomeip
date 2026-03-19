/*
 *
 *    Copyright (c) 2022-2023 Project CHIP Authors
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
#include "DeviceManager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "DeviceWithDisplay.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/clusters/fan-control-server/fan-control-server.h>
#include <app/reporting/reporting.h>
#include <app/server/Server.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/InteractionModelEngine.h>
#include "driver/gpio.h"

#define APP_TASK_NAME "APP"
#define APP_EVENT_QUEUE_SIZE 10
#define APP_TASK_STACK_SIZE (3072)
#define BUTTON_PRESSED 1
#define APP_LIGHT_SWITCH 1

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::Credentials;
using namespace ::chip::DeviceLayer;

static const char TAG[] = "app-task";

LEDWidget AppLED;

namespace {
constexpr EndpointId kLightEndpointId = 1;
constexpr EndpointId kFanEndpointId = 3;
QueueHandle_t sAppEventQueue;
TaskHandle_t sAppTaskHandle;
} // namespace

AppTask AppTask::sAppTask;
TimerHandle_t AppTask::sCommissioningLEDTimer = nullptr;
bool AppTask::sCommissioningLEDState = false;
bool AppTask::sUpdatingFromHardware = false;
bool AppTask::sInitialSyncDone = false;

CHIP_ERROR AppTask::StartAppTask()
{
    sAppEventQueue = xQueueCreate(APP_EVENT_QUEUE_SIZE, sizeof(AppEvent));
    if (sAppEventQueue == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate app event queue");
        return APP_ERROR_EVENT_QUEUE_FAILED;
    }

    // Start App task.
    BaseType_t xReturned;
    xReturned = xTaskCreate(AppTaskMain, APP_TASK_NAME, APP_TASK_STACK_SIZE, NULL, 1, &sAppTaskHandle);
    return (xReturned == pdPASS) ? CHIP_NO_ERROR : APP_ERROR_CREATE_TASK_FAILED;
}

void AppTask::ButtonEventHandler(const uint8_t buttonHandle, uint8_t btnAction)
{
    if (btnAction != APP_BUTTON_PRESSED)
    {
        return;
    }

    AppEvent button_event = {};
    button_event.Type     = AppEvent::kEventType_Button;

#if CONFIG_HAVE_DISPLAY
    button_event.ButtonEvent.PinNo  = buttonHandle;
    button_event.ButtonEvent.Action = btnAction;
    button_event.mHandler           = ButtonPressedAction;
#else
    button_event.mHandler = AppTask::LightingActionEventHandler;
#endif

    sAppTask.PostEvent(&button_event);
}

#if CONFIG_DEVICE_TYPE_M5STACK
void AppTask::ButtonPressedAction(AppEvent * aEvent)
{
    uint32_t io_num = aEvent->ButtonEvent.PinNo;
    int level       = gpio_get_level((gpio_num_t) io_num);
    if (level == 0)
    {
        bool woken = WakeDisplay();
        if (woken)
        {
            return;
        }
        // Button 1 is connected to the pin 39
        // Button 2 is connected to the pin 38
        // Button 3 is connected to the pin 37
        // So we use 40 - io_num to map the pin number to button number
        ScreenManager::ButtonPressed(40 - io_num);
    }
}
#endif

CHIP_ERROR AppTask::Init()
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    AppLED.Init();
    
    // Suppress non-critical errors during boot and operation
    // These are timing-related issues that resolve automatically as the network comes up
    esp_log_level_set("chip[DIS]", ESP_LOG_NONE);  // mDNS timeouts when Thread network isn't ready
    esp_log_level_set("chip[DMG]", ESP_LOG_NONE);  // "Not implemented" responses for optional attributes
    esp_log_level_set("chip[DL]", ESP_LOG_NONE);   // SRP timeouts and "long dispatch" warnings from network latency
    
    // Initialize device manager for fan and light
    err = DeviceMgr().Init();
    if (err != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "DeviceManager.Init() failed");
        return err;
    }
    
    // Register callback for hardware state changes (button presses and Matter command completions)
    DeviceMgr().SetStateChangeCallback([]() {
        ESP_LOGI(TAG, "Hardware state changed, scheduling Matter update");
        chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
            sAppTask.UpdateClusterState();
        });
    });

    // Initialize commissioning button
    err = InitCommissioningButton();
    if (err != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "InitCommissioningButton() failed");
        return err;
    }
    
    // Initialize commissioning LED
    err = InitCommissioningLED();
    if (err != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "InitCommissioningLED() failed");
        return err;
    }

#if CONFIG_HAVE_DISPLAY
    InitDeviceDisplay();

    AppLED.SetVLED(ScreenManager::AddVLED(TFT_YELLOW));
#endif

    return err;
}

void AppTask::AppTaskMain(void * pvParameter)
{
    AppEvent event;
    CHIP_ERROR err = sAppTask.Init();
    if (err != CHIP_NO_ERROR)
    {
        ESP_LOGI(TAG, "AppTask.Init() failed due to %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    ESP_LOGI(TAG, "App Task started");

    while (true)
    {
        BaseType_t eventReceived = xQueueReceive(sAppEventQueue, &event, pdMS_TO_TICKS(500));
        while (eventReceived == pdTRUE)
        {
            sAppTask.DispatchEvent(&event);
            eventReceived = xQueueReceive(sAppEventQueue, &event, 0); // return immediately if the queue is empty
        }
        
        // Check if we need to do initial cluster sync (delayed until subscriptions are active)
        if (!sInitialSyncDone)
        {
            // Check if there are any active subscriptions
            uint32_t numSubscriptions = chip::app::InteractionModelEngine::GetInstance()->GetNumActiveReadHandlers(
                chip::app::ReadHandler::InteractionType::Subscribe);
            
            if (numSubscriptions > 0)
            {
                ESP_LOGI(TAG, "Subscriptions active (%lu), performing initial cluster sync", numSubscriptions);
                sInitialSyncDone = true;
                chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
                    sAppTask.UpdateClusterState();
                });
            }
        }
        
        // Check for hardware state changes from interrupts (non-blocking)
        DeviceMgr().GetLightController().NotifyStateChange();

        // Check for fan button-press interrupts (non-blocking, mirrors light approach)
        DeviceMgr().GetFanController().NotifyStateChange();
        
        // Monitor commissioning window state
        static bool sWasCommissioningWindowOpen = false;
        bool isCommissioningWindowOpen = chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
        if (sWasCommissioningWindowOpen && !isCommissioningWindowOpen)
        {
            // Window just closed, stop LED flashing
            StopCommissioningLEDFlash();
        }
        sWasCommissioningWindowOpen = isCommissioningWindowOpen;
    }
}

void AppTask::PostEvent(const AppEvent * aEvent)
{
    if (sAppEventQueue != NULL)
    {
        BaseType_t status;
        if (xPortInIsrContext())
        {
            BaseType_t higherPrioTaskWoken = pdFALSE;
            status                         = xQueueSendFromISR(sAppEventQueue, aEvent, &higherPrioTaskWoken);
        }
        else
        {
            status = xQueueSend(sAppEventQueue, aEvent, 1);
        }
        if (!status)
            ESP_LOGE(TAG, "Failed to post event to app task event queue");
    }
    else
    {
        ESP_LOGE(TAG, "Event Queue is NULL should never happen");
    }
}

void AppTask::DispatchEvent(AppEvent * aEvent)
{
    if (aEvent->mHandler)
    {
        aEvent->mHandler(aEvent);
    }
    else
    {
        ESP_LOGI(TAG, "Event received with no handler. Dropping event.");
    }
}

void AppTask::LightingActionEventHandler(AppEvent * aEvent)
{
    AppLED.Toggle();
    // Schedule cluster state update on Matter thread
    chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
        sAppTask.UpdateClusterState();
    });
}

void AppTask::UpdateClusterState()
{
    Protocols::InteractionModel::Status status;
    
    // Set flag to prevent attribute callbacks from triggering hardware changes
    sUpdatingFromHardware = true;
    
    // Update Light endpoint
    ESP_LOGI(TAG, "Updating Light endpoint (EP %d)", kLightEndpointId);
    bool lightOn = DeviceMgr().IsLightOn();
    uint8_t lightBrightness = DeviceMgr().GetLightBrightness();
    
    ESP_LOGI(TAG, "Light state from hardware: OnOff=%d, Brightness=%d", lightOn, lightBrightness);
    
    status = Clusters::OnOff::Attributes::OnOff::Set(kLightEndpointId, lightOn);
    if (status != Protocols::InteractionModel::Status::Success)
    {
        ESP_LOGE(TAG, "Updating light on/off cluster failed: %x", to_underlying(status));
    }
    else
    {
        ESP_LOGI(TAG, "OnOff cluster updated to: %d", lightOn);
        // Notify subscribers of the change
        MatterReportingAttributeChangeCallback(kLightEndpointId, Clusters::OnOff::Id, 
                                               Clusters::OnOff::Attributes::OnOff::Id);
    }
    
    status = Clusters::LevelControl::Attributes::CurrentLevel::Set(kLightEndpointId, lightBrightness);
    if (status != Protocols::InteractionModel::Status::Success)
    {
        ESP_LOGE(TAG, "Updating light level cluster failed: %x", to_underlying(status));
    }
    else
    {
        ESP_LOGI(TAG, "CurrentLevel cluster updated to: %d", lightBrightness);
        // Notify subscribers of the change
        MatterReportingAttributeChangeCallback(kLightEndpointId, Clusters::LevelControl::Id,
                                               Clusters::LevelControl::Attributes::CurrentLevel::Id);
    }
    
    // Update Fan endpoint (already on Matter thread from ScheduleWork)
    ESP_LOGI(TAG, "Updating Fan endpoint (EP %d)", kFanEndpointId);
    uint8_t fanSpeed = DeviceMgr().GetFanPercentSpeed();
    ESP_LOGI(TAG, "Fan speed from hardware: %d%%", fanSpeed);
    
    status = Clusters::FanControl::Attributes::PercentSetting::Set(kFanEndpointId, fanSpeed);
    if (status != Protocols::InteractionModel::Status::Success)
    {
        ESP_LOGE(TAG, "Updating fan PercentSetting failed: %x", to_underlying(status));
    }
    else
    {
        ESP_LOGI(TAG, "PercentSetting cluster updated to: %d", fanSpeed);
        // Notify subscribers of the change
        MatterReportingAttributeChangeCallback(kFanEndpointId, Clusters::FanControl::Id,
                                               Clusters::FanControl::Attributes::PercentSetting::Id);
    }
    
    status = Clusters::FanControl::Attributes::PercentCurrent::Set(kFanEndpointId, fanSpeed);
    if (status != Protocols::InteractionModel::Status::Success)
    {
        ESP_LOGE(TAG, "Updating fan PercentCurrent failed: %x", to_underlying(status));
    }
    else
    {
        ESP_LOGI(TAG, "PercentCurrent cluster updated to: %d", fanSpeed);
        // Notify subscribers of the change
        MatterReportingAttributeChangeCallback(kFanEndpointId, Clusters::FanControl::Id,
                                               Clusters::FanControl::Attributes::PercentCurrent::Id);
    }
    
    // Fan is binary: Off or High (100%). Map accordingly.
    chip::app::Clusters::FanControl::FanModeEnum fanMode;
    if (fanSpeed == 0)
    {
        fanMode = chip::app::Clusters::FanControl::FanModeEnum::kOff;
    }
    else
    {
        fanMode = chip::app::Clusters::FanControl::FanModeEnum::kHigh;
    }
    
    status = Clusters::FanControl::Attributes::FanMode::Set(kFanEndpointId, fanMode);
    if (status != Protocols::InteractionModel::Status::Success)
    {
        ESP_LOGE(TAG, "Updating fan FanMode failed: %x", to_underlying(status));
    }
    else
    {
        ESP_LOGI(TAG, "FanMode cluster updated to: %d", to_underlying(fanMode));
        // Notify subscribers of the change
        MatterReportingAttributeChangeCallback(kFanEndpointId, Clusters::FanControl::Id,
                                               Clusters::FanControl::Attributes::FanMode::Id);
    }
    
    // Clear flag - callbacks can now trigger hardware changes
    sUpdatingFromHardware = false;
}

// Commissioning Button ISR (non-static, declared as friend in AppTask.h)
void IRAM_ATTR commissioning_button_isr_handler(void * arg)
{
    static TickType_t lastInterruptTime = 0;
    TickType_t currentTime = xTaskGetTickCountFromISR();
    
    // Debounce: ignore interrupts within 500ms of last press
    if ((currentTime - lastInterruptTime) < pdMS_TO_TICKS(500))
    {
        return;
    }
    lastInterruptTime = currentTime;
    
    AppEvent event;
    event.Type = AppEvent::kEventType_Button;
    event.mHandler = AppTask::CommissioningButtonEventHandler;
    
    // Post event from ISR
    AppTask::sAppTask.PostEvent(&event);
}

CHIP_ERROR AppTask::InitCommissioningButton()
{
    ESP_LOGI(TAG, "Initializing commissioning button on GPIO %d", COMMISSIONING_BUTTON_GPIO);
    
    // Configure GPIO
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_NEGEDGE;  // Trigger on falling edge (button press)
    io_conf.pin_bit_mask = (1ULL << COMMISSIONING_BUTTON_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;  // Enable internal pull-up
    
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return CHIP_ERROR_INTERNAL;
    }
    
    // Install ISR service if not already installed (may already be installed by display or LED code)
    // Note: ESP-IDF logs "GPIO isr service already installed" internally, which is harmless
    err = gpio_install_isr_service(0);
    if (err == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGI(TAG, "GPIO ISR service already installed (expected)");
    }
    else if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "GPIO ISR service install failed: %s", esp_err_to_name(err));
        return CHIP_ERROR_INTERNAL;
    }
    
    // Hook ISR handler
    err = gpio_isr_handler_add(COMMISSIONING_BUTTON_GPIO, commissioning_button_isr_handler, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "GPIO ISR handler add failed: %s", esp_err_to_name(err));
        return CHIP_ERROR_INTERNAL;
    }
    
    ESP_LOGI(TAG, "Commissioning button initialized successfully");
    return CHIP_NO_ERROR;
}

void AppTask::CommissioningButtonEventHandler(AppEvent * aEvent)
{
    ESP_LOGI(TAG, "Commissioning button pressed - opening commissioning window");
    
    // Schedule work on the Matter thread to open commissioning window
    chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
        auto & commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
        
        // Check if already in commissioning mode
        if (commissionMgr.IsCommissioningWindowOpen())
        {
            ESP_LOGI(TAG, "Commissioning window already open");
            return;
        }
        
        // Open basic commissioning window with default timeout
        CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow();
        if (err != CHIP_NO_ERROR)
        {
            ESP_LOGE(TAG, "Failed to open commissioning window: %" CHIP_ERROR_FORMAT, err.Format());
        }
        else
        {
            ESP_LOGI(TAG, "Commissioning window opened successfully");
            // Start LED flashing to indicate commissioning mode
            StartCommissioningLEDFlash();
        }
    });
}

CHIP_ERROR AppTask::InitCommissioningLED()
{
    ESP_LOGI(TAG, "Initializing commissioning LED on GPIO %d", COMMISSIONING_LED_GPIO);
    
    // Configure GPIO as output
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << COMMISSIONING_LED_GPIO);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "GPIO config for LED failed: %s", esp_err_to_name(err));
        return CHIP_ERROR_INTERNAL;
    }
    
    // Set initial state to OFF
    gpio_set_level(COMMISSIONING_LED_GPIO, 0);
    
    // Create timer for LED flashing (500ms period = 250ms on, 250ms off)
    sCommissioningLEDTimer = xTimerCreate(
        "CommLED",
        pdMS_TO_TICKS(250),  // 250ms period
        pdTRUE,              // Auto-reload
        nullptr,             // Timer ID
        CommissioningLEDTimerCallback
    );
    
    if (sCommissioningLEDTimer == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create commissioning LED timer");
        return CHIP_ERROR_INTERNAL;
    }
    
    ESP_LOGI(TAG, "Commissioning LED initialized successfully");
    return CHIP_NO_ERROR;
}

void AppTask::StartCommissioningLEDFlash()
{
    ESP_LOGI(TAG, "Starting commissioning LED flash");
    if (sCommissioningLEDTimer != nullptr)
    {
        sCommissioningLEDState = false;
        gpio_set_level(COMMISSIONING_LED_GPIO, 0);
        xTimerStart(sCommissioningLEDTimer, 0);
    }
}

void AppTask::StopCommissioningLEDFlash()
{
    ESP_LOGI(TAG, "Stopping commissioning LED flash");
    if (sCommissioningLEDTimer != nullptr)
    {
        xTimerStop(sCommissioningLEDTimer, 0);
        // Turn off LED
        gpio_set_level(COMMISSIONING_LED_GPIO, 0);
        sCommissioningLEDState = false;
    }
}

void AppTask::CommissioningLEDTimerCallback(TimerHandle_t xTimer)
{
    // Toggle LED state
    sCommissioningLEDState = !sCommissioningLEDState;
    gpio_set_level(COMMISSIONING_LED_GPIO, sCommissioningLEDState ? 1 : 0);
}
