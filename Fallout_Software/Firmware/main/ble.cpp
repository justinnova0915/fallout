// main/ble.cpp
#include "ble.hpp"
#include "esp_log.h"
#include "nvs_flash.h"
#include <cstring>
#include <stdio.h>

// NimBLE Host & Controller Headers
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"

static const char* TAG = "BLE_DRV";

// Buffer for storing inbound data
static std::string s_rx_buffer = "";
static uint16_t s_conn_handle = 0;
static uint16_t s_tx_char_handle = 0;

// Bluetooth custom 128-bit layout identifiers 
static const ble_uuid128_t g_svc_uuid = 
    BLE_UUID128_INIT(0xab, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);
static const ble_uuid128_t g_rx_uuid = 
    BLE_UUID128_INIT(0xac, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);
static const ble_uuid128_t g_tx_uuid = 
    BLE_UUID128_INIT(0xad, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static int ble_gap_event(struct ble_gap_event *event, void *arg);
static void ble_advertise();

/**
 * @brief NimBLE stack reset lifecycle callback
 */
static void ble_on_reset(int reason) {
    ESP_LOGW(TAG, "Resetting BLE host stack; reason=%d", reason);
}

/**
 * @brief Custom local GATT registration tracking callback
 */
static void ble_on_gatt_register(struct ble_gatt_register_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_REGISTER_OP_SVC) {
        ESP_LOGI(TAG, "[GATT_REG] Exposing Service Handle: %d", ctxt->svc.handle);
    } else if (ctxt->op == BLE_GATT_REGISTER_OP_CHR) {
        ESP_LOGI(TAG, "[GATT_REG] Exposing Characteristic Def Handle: %d, Val Handle: %d", 
                 ctxt->chr.def_handle, ctxt->chr.val_handle);
    }
}

/**
 * @brief GATT service inbound packet write parser callback 
 */ 
static int gatt_callback(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        struct os_mbuf *om = ctxt->om;
        while (om != nullptr) {
            s_rx_buffer.append(reinterpret_cast<char*>(om->om_data), om->om_len);
            om = SLIST_NEXT(om, om_next);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// GATT service definitions data matrix with absolute inline array declarations
static const struct ble_gatt_svc_def g_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_svc_uuid.u,
        .includes = nullptr,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &g_rx_uuid.u,
                .access_cb = gatt_callback,
                .arg = nullptr,
                .descriptors = nullptr,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .min_key_size = 0,
                .val_handle = nullptr,
                .cpfd = nullptr,
            },
            {
                .uuid = &g_tx_uuid.u,
                .access_cb = gatt_callback, 
                .arg = nullptr,
                .descriptors = nullptr,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .min_key_size = 0,
                .val_handle = &s_tx_char_handle,
                .cpfd = nullptr,
            },
            {
                .uuid = nullptr,
                .access_cb = nullptr,
                .arg = nullptr,
                .descriptors = nullptr,
                .flags = 0,
                .min_key_size = 0,
                .val_handle = nullptr,
                .cpfd = nullptr,
            }
        },
    },
    {
        .type = 0,
        .uuid = nullptr,
        .includes = nullptr,
        .characteristics = nullptr,
    }
};

/**
 * @brief Broadcasts out GAP fields to initiate wireless discovery
 */
static void ble_advertise() {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    std::memset(&fields, 0, sizeof(fields));
    
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)"TALOS-01";
    fields.name_len = std::strlen("TALOS-01");
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error setting advertisement fields; rc=%d", rc);
        return;
    }

    std::memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, nullptr, BLE_HS_FOREVER, &adv_params, ble_gap_event, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error starting advertisement; rc=%d", rc);
    }
}

/**
 * @brief Core Radio Hardware Synchronization Handler
 */
static void ble_on_sync(void) {
    ESP_LOGI(TAG, "[SYNC] Radio hardware locked. Loading configuration profiles...");

    uint8_t addr_type;
    ble_hs_id_infer_auto(0, &addr_type);

    ble_svc_gap_init();
    ble_svc_gap_device_name_set("TALOS-01");

    int rc = ble_gatts_count_cfg(g_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT count config failed; rc=%d", rc);
        return;
    }
    
    rc = ble_gatts_add_svcs(g_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT adding services failed; rc=%d", rc);
        return;
    }

    rc = ble_gatts_start();
    if (rc != 0) {
        ESP_LOGE(TAG, "CRITICAL: Failed to execute ble_gatts_start(); rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "[DATABASE] GATT table verified and active. Starting broadcast...");
    ble_advertise();
}

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected to Host Daemon");
        } else {
            ble_advertise();
        }
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        s_conn_handle = 0;
        ESP_LOGW(TAG, "Disconnected from Host Daemon");
        ble_advertise();
    }
    return 0;
}

static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "[THREAD] FreeRTOS NimBLE port loop spinning up safely.");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

BluetoothManager::BluetoothManager(const std::string& device_name) : dev_name_(device_name) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    initBluetoothStack();
}

BluetoothManager::~BluetoothManager() {
    nimble_port_stop();
}

void BluetoothManager::initBluetoothStack() {
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = ble_on_sync; 
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.gatts_register_cb = ble_on_gatt_register;

    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "[SUCCESS] Background radio pipeline mounted securely.");
}

void BluetoothManager::writeString(const std::string& str) {
    if (s_conn_handle != 0 && s_tx_char_handle != 0) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(str.c_str(), str.length());
        if (om) {
            ble_gatts_notify_custom(s_conn_handle, s_tx_char_handle, om); 
        }
    }
}

bool BluetoothManager::available() const { return !s_rx_buffer.empty(); } 

std::string BluetoothManager::readStringUntil(char terminator) {
    size_t pos = s_rx_buffer.find(terminator); 

    if (pos == std::string::npos) { 
        return ""; 
    }

    std::string token = s_rx_buffer.substr(0, pos); 
    s_rx_buffer.erase(0, pos + 1); 
    return token; 
}