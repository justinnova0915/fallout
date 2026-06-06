// main/ble.cpp
#include "ble.hpp"
#include "esp_log.h"
#include "nvs_flash.h"
#include <cstring>

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

// Bluetooth requires qunique UUID identifiers
static const ble_uuid128_t g_svc_uuid = 
    BLE_UUID128_INIT(0xab, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);
static const ble_uuid128_t g_rx_uuid = 
    BLE_UUID128_INIT(0xac, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);
static const ble_uuid128_t g_tx_uuid = 
    BLE_UUID128_INIT(0xad, 0x90, 0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12);

static int ble_gap_event(struct ble_gap_event *event, void *arg);
static void ble_advertise();

/**
 * @brief GATT service write callback function
 * @details This callback function is passed into the GATT service and is called by the service
 *          when the computer wants to write data to the memory. the data is in an os memory buffer, which is just
 *          a linked list of packets recived. the function will piece the packets together then push it into the buffer.
 * @param conn_handle the ID of the source of the incoming signal
 * @param attr_handle the address of the memory being written to
 * @param ctxt context of the request. includes things like the operation and the data
 * @param arg generic pointer
 */ 
static int gatt_callback(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        struct os_mbuf *om = ctxt->om;
        while (om != NULL) {
            s_rx_buffer.append(reinterpret_cast<char*>(om->om_data), om->om_len);
            om = SLIST_NEXT(om, om_next);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// GATT service
static const struct ble_gatt_svc_def g_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_svc_uuid.u,
        .includes = NULL,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &g_rx_uuid.u,
                .access_cb = gatt_callback,
                .arg = NULL,
                .descriptors = NULL,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .min_key_size = 0,
                .val_handle = NULL,
                .cpfd = NULL,
            },
            {
                .uuid = &g_tx_uuid.u,
                .access_cb = NULL,
                .arg = NULL,
                .descriptors = NULL,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .min_key_size = 0,
                .val_handle = &s_tx_char_handle,
                .cpfd = NULL,
            },
            { 
                .uuid = NULL,
                .access_cb = NULL,
                .arg = NULL,
                .descriptors = NULL,
                .flags = 0,
                .min_key_size = 0,
                .val_handle = NULL,
                .cpfd = NULL
            }
        },
    },
    { .type = 0, .uuid = NULL, .includes = NULL, .characteristics = NULL }
};

/**
 * @brief Broadcasts out the device, making it discoverable
 */
static void ble_advertise() {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    
    // Sets the device to be discoverable and BLE only
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)"TALOS-01";
    fields.name_len = strlen("TALOS-01");
    fields.name_is_complete = 1;

    ble_gap_adv_set_fields(&fields);
    memset(&adv_params, 0, sizeof(adv_params));
    // Sets the device to be pairable to any device
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    // Makes the device advertise continuously and indefinitely
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    
    // Starts Advertising
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
}

// Pairing callback func
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected");
        } else {
            ble_advertise();
        }
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        s_conn_handle = 0;
        ESP_LOGW(TAG, "Disconnected");
        ble_advertise();
    }
    return 0;
}

static void ble_host_task(void *param) {
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
    
    // advertise once the bluetooth is set up
    ble_hs_cfg.sync_cb = ble_advertise;
    
    // initializes GAP (Generic Access Profile), 
    // which allows the device to be discoverable and talked to.
    ble_svc_gap_init();
    ble_gatts_count_cfg(g_gatt_svcs);
    ble_gatts_add_svcs(g_gatt_svcs);
    
    ble_svc_gap_device_name_set("TALOS-01");

    // Creates the FreeRTOS task
    xTaskCreate(ble_host_task, "ble_host", 4096, NULL, 5, NULL);
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