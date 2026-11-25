/**
 * @file blesh.c
 * @brief ESP32 BLE Remotesensig Controller using ESP-IDF Bluedroid Stack.
 *
 * @overview
 * Operates as a hybrid BLE Beacon + GATT Server:
 *  1. Broadcasts live Fire/Gas sensor status and Lighting state inside 4-byte
 *     Manufacturer-Specific Data (connectionless, readable by any scanner).
 *  2. Exposes a connectable GATT Lighting Service allowing a smartphone app
 *     to read or toggle the onboard/relay light via GPIO.
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"


/* ESP-IDF Bluedroid Bluetooth Stack Headers */
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

/* Logging & Hardware Configuration */
#define TAG "BLUEDROID_SMARTHOME"
#define LIGHT_GPIO GPIO_NUM_2

/* BLE Application & GAP Profile Identifiers */
#define PROFILE_APP_ID          0
#define DEVICE_NAME             "ESP32_Remotesensing"

/* Custom 16-bit UUIDs for Lighting Control Service & Characteristic */
#define SVC_LIGHT_UUID          0x00EE
#define CHR_LIGHT_UUID          0xEE01

/* Runtime Device States & Attribute Handles */
static uint8_t  fire_status = 0;                    // 0 = Normal/Safe, 1 = Fire/Gas Alert
static uint8_t  light_state = 0;                    // 0 = Light OFF,   1 = Light ON
static uint16_t light_svc_handle;                   // Handle assigned to Lighting Service
static uint16_t light_chr_handle;                   // Handle assigned to Ligh


/**
 * 4-Byte Manufacturer-Specific Advertising Payload:
 *  - Byte 0: Company ID LSB (0xFF = Test/Development ID)
 *  - Byte 1: Company ID MSB (0xFF = Test/Development ID)
 *  - Byte 2: Live Fire/Gas Alert Status (0x00 or 0x01)
 *  - Byte 3: Live Lighting Indicator State (0x00 or 0x01)
 */
static uint8_t mfg_payload[4] = {0xFF, 0xFF, 0x00, 0x00};

/**
 * Legacy BLE Advertising Payload Configuration (31-byte strict maximum):
 *  - Flags (3B) + Complete Name "ESP32_Remotesensing" (17B) + Mfg Data (6B) = 26 Bytes total
 */
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = false,
    .appearance = 0x00,
    .manufacturer_len = sizeof(mfg_payload),
    .p_manufacturer_data = mfg_payload,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

/**
 * GAP Advertising Parameters:
 *  - Interval tuned between 500 ms and 2,000 ms (in 0.625 ms BLE ticks) to balance
 *    low power consumption with acceptable fire-alert latency.
 *  - ADV_TYPE_IND enables both passive beacon scanning and active GATT connections.
 */
static esp_ble_adv_params_t adv_params = {
    .adv_int_min        = 0x0320, // 800 * 0.625 ms = 500 ms
    .adv_int_max        = 0x0C80, // 3200 * 0.625 ms = 2,000 ms
    .adv_type           = ADV_TYPE_IND, // Allows unconnected scanning AND connecting to write commands
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/**
 * @brief Syncs current sensor and light states into the BLE advertising packet.
 *
 * Updates the manufacturer payload bytes and pushes the new buffer to the BLE controller.
 * Completion triggers ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT to start/refresh broadcasting.
 */
static void update_advertising_status(void) {
    mfg_payload[2] = fire_status;
    mfg_payload[3] = light_state;
    // Re-configuring adv_data triggers ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT, restarting advertising
    esp_ble_gap_config_adv_data(&adv_data);
}

/**
 * @brief GAP (Generic Access Profile) Event Callback.
 *
 * Manages advertising lifecycle transitions after payload configuration or start commands.
 *
 * @param event GAP callback event type
 * @param param Union containing event-specific status and parameters
 */
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            esp_ble_gap_start_advertising(&adv_params);
            break;
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Broadcasting -> Fire: %d | Light: %d (Interval: 500-2000ms)", fire_status, light_state);
            }
            break;
        default:
            break;
    }
}

/**
 * @brief GATTS (GATT Server) Event Callback.
 *
 * Handles service registration, characteristic creation, and client Read/Write/Disconnect
 * operations for remote lighting control.
 *
 * @param event    GATT server event type
 * @param gatts_if GATT server interface identifier
 * @param param    Event parameters (handles, connection IDs, write payloads)
 */
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    switch (event) {
        case ESP_GATTS_REG_EVT: {
            esp_ble_gap_set_device_name(DEVICE_NAME);
            update_advertising_status(); // Start initial broadcast

            esp_gatt_srvc_id_t light_srvc_id = {
                .is_primary = true,
                .id.inst_id = 0x00,
                .id.uuid.len = ESP_UUID_LEN_16,
                .id.uuid.uuid.uuid16 = SVC_LIGHT_UUID,
            };
            esp_ble_gatts_create_service(gatts_if, &light_srvc_id, 3);
            break;
        }

        case ESP_GATTS_CREATE_EVT: {
            light_svc_handle = param->create.service_handle;
            esp_ble_gatts_start_service(light_svc_handle);

            esp_bt_uuid_t light_uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = CHR_LIGHT_UUID };
            esp_ble_gatts_add_char(light_svc_handle, &light_uuid,
                                   ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                   ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE,
                                   NULL, NULL);
            break;
        }

        case ESP_GATTS_ADD_CHAR_EVT:
            light_chr_handle = param->add_char.attr_handle; 
            break;

        case ESP_GATTS_READ_EVT: {
            esp_gatt_rsp_t rsp;
            memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
            rsp.attr_value.handle = param->read.handle;
            rsp.attr_value.len = 1;
            rsp.attr_value.value[0] = light_state;
            esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_OK, &rsp);
            break;
        }

        case ESP_GATTS_WRITE_EVT: {
            if (param->write.handle == light_chr_handle && param->write.len == 1) {
                light_state = param->write.value ? (uint8_t)1 : 0;
                gpio_set_level(LIGHT_GPIO, light_state);
                ESP_LOGI(TAG, "Command received: Light %s", light_state ? "ON" : "OFF");
                
                // Update the advertised broadcast data with the new light indicator
                update_advertising_status();
            }
            if (param->write.need_rsp) {
                esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
            }
            break;
        }

        case ESP_GATTS_DISCONNECT_EVT:
            ESP_LOGI(TAG, "Phone disconnected, resuming advertising...");
            esp_ble_gap_start_advertising(&adv_params);
            break;

        default:
            break;
    }
}


/**
 * @brief Main application entry point.
 */
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_reset_pin(LIGHT_GPIO);
    gpio_set_direction(LIGHT_GPIO, GPIO_MODE_OUTPUT);

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(PROFILE_APP_ID));
}
