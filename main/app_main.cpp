/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <bsp/esp-bsp.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_ota.h>
#include <nvs_flash.h>
#include "esp_wifi.h"
#include <cmath>

#include <app_openthread_config.h>
#include <app_reset.h>
#include <common_macros.h>

// v1.6.1: sanctioned setters for read-only measurement attributes.
// attribute::update() is rejected (ESP_ERR_NOT_SUPPORTED) for these clusters
// now that they are served by CHIP's data_model_provider.
#include <data_model_provider/clusters/illuminance_measurement/integration.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

// drivers implemented by this project
#include <drivers/veml7700_handler.h>

// Oral-B iO advertisement/battery watcher
#include "oralb_ble.h"

// Brush state changes -> Telegram
#include "brush_handler.h"
#include "telegram.h"

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

// Endpoint 1: lux (Light Sensor). Brushing is reported to Telegram instead of
// being exposed as a Matter endpoint.
static uint16_t s_light_endpoint_id = 0;

// static esp_err_t factory_reset_button_register()
// {
//     button_handle_t push_button;
//     esp_err_t err = bsp_iot_button_create(&push_button, NULL, BSP_BUTTON_NUM);
//     VerifyOrReturnError(err == ESP_OK, err);
//     return app_reset_button_register(push_button);
// }

static void open_commissioning_window_if_necessary()
{
    VerifyOrReturn(chip::Server::GetInstance().GetFabricTable().FabricCount() == 0);

    chip::CommissioningWindowManager & commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    VerifyOrReturn(commissionMgr.IsCommissioningWindowOpen() == false);

    // After removing last fabric, this example does not remove the Wi-Fi credentials
    // and still has IP connectivity so, only advertising on DNS-SD.
    CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(300),
                                    chip::CommissioningWindowAdvertisement::kDnssdOnly);
    if (err != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
    }
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Fabric removed successfully");
        open_commissioning_window_if_necessary();
        break;

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
        break;

    default:
        break;
    }
}

// This callback is invoked when clients interact with the Identify Cluster.
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);
    return ESP_OK;
}

// This callback is called for every attribute update.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    // Sensors are read-only; accept everything so writes do not fail early.
    return ESP_OK;
}

static uint16_t lux_to_matter_value(uint32_t lux)
{
    if (lux <= 0) return 0; // Completely dark

    float measured_value = 10000 * log10((float)lux) + 1;

    if (measured_value > 0xFFFE) return 0xFFFE; // Upper limit
    return (uint16_t)measured_value;
}

extern "C" void app_main()
{
    /* Initialize the ESP NVS layer */
    nvs_flash_init();
    /* Initialize push button on the dev-kit to reset the device */
    // esp_err_t err = factory_reset_button_register();
    // ABORT_APP_ON_FAILURE(ESP_OK == err, ESP_LOGE(TAG, "Failed to initialize reset button, err:%d", err));

    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    endpoint::light_sensor::config_t sensor_config;
    // Standard value at boot (e.g. 1 lux)
    sensor_config.illuminance_measurement.measured_value = 1;

    endpoint_t *endpoint = endpoint::light_sensor::create(node, &sensor_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create light sensor endpoint"));

    s_light_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Lux endpoint %u", s_light_endpoint_id);

    /* Matter start. This also brings up the NimBLE host and waits until it is
     * synced, so oralb_ble_init() can attach to the running host below. The
     * Wi-Fi station is owned by Matter (credentials are provisioned during
     * commissioning), so Telegram just waits for it to get an IP. */
    esp_err_t err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    /* Matter is mains powered and must answer CASE/MRP promptly. The default
     * modem sleep adds ~100-200 ms per packet and, combined with the BLE scan,
     * makes the AP drop beacons. Keep the radio awake instead. */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "could not disable Wi-Fi power save: %d", ps_err);
    }

    err = telegram_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Telegram task, err:%d", err);
    }

    err = oralb_ble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Oral-B BLE watcher, err:%d", err);
    }

    BaseType_t task_ok = xTaskCreate(brush_handler_task, "brush_handler", 4096,
                                     NULL, 5, NULL);
    ABORT_APP_ON_FAILURE(task_ok == pdPASS, ESP_LOGE(TAG, "Failed to create brush handler task"));

    if (sensor_veml7700_init() == ESP_OK) {
        ESP_LOGI("MAIN", "Sensor ready!");
    }

    while (1) {
        uint32_t lux = 0;

        if (esp_timer_get_time() > 12ULL * 60ULL * 60ULL * 1000000ULL) {
            ESP_LOGI(TAG, "Restarting after 12 hours");
            esp_restart();
        }
        
        if (sensor_veml7700_read(&lux) == ESP_OK && lux > 0) {

            uint16_t matter_val = lux_to_matter_value(lux);

            // SetMeasuredValue() does not take the stack lock, so run it on the Matter
            // thread. This mirrors how the official sensors example reports its readings.
            chip::DeviceLayer::SystemLayer().ScheduleLambda([matter_val, lux]() {
                CHIP_ERROR cerr = IlluminanceMeasurement::SetMeasuredValue(
                    s_light_endpoint_id, chip::app::DataModel::Nullable<uint16_t>(matter_val));

                if (cerr != CHIP_NO_ERROR) {
                    ESP_LOGE("MAIN", "Matter update failed: %" CHIP_ERROR_FORMAT, cerr.Format());
                } else {
                    ESP_LOGI("MAIN",
                            "Lux: %lu -> Matter: %u",
                            (unsigned long)lux, matter_val);
                }
            });
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
