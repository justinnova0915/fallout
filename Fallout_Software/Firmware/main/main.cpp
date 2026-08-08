// main/main.cpp
#include <cstdio>
#include <cstring>  
#include <cmath>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "esp_timer.h" 

// File imports
#include "screen.hpp"
#include "voltmeter.hpp"
#include "input.hpp"
#include "magnetic.hpp"
#include "ble.hpp"

static const char* MAIN_TAG = "FALLOUT_MAIN";

#define SENSOR_A_CHAN ADC_CHANNEL_3 
#define SENSOR_B_CHAN ADC_CHANNEL_4 
#define POLE_PITCH_MM 2.0f

static constexpr float MM_PER_WORKSPACE = 8.0f; 

// EXACT VARIABLE POINTER LAYOUT ADDRESSES
static constexpr uint16_t TIME_TEXT_VP     = 0x3F00;
static constexpr uint16_t DATE_TEXT_VP     = 0x6800;
static constexpr uint16_t WEATHER_TEXT_VP  = 0x6900;
static constexpr uint16_t WEATHER_ICON_VP  = 0x5F00;

// APP ICON POINTER LAYOUT ADDRESS
static constexpr uint16_t APP_ICON_VP      = 0x6B00;

// MPRIS MUSIC POINTER LAYOUT ADDRESSES
static constexpr uint16_t MUSIC_TITLE_VP   = 0x7000;
static constexpr uint16_t MUSIC_ARTIST_VP  = 0x7100;
static constexpr uint16_t MUSIC_STATUS_VP  = 0x7200;

void send_dwin_raw_int(uart_port_t uart_port, uint16_t address, uint16_t value) {
    uint8_t buffer[8];
    buffer[0] = 0x5A; buffer[1] = 0xA5; buffer[2] = 0x05; buffer[3] = 0x82; 
    buffer[4] = static_cast<uint8_t>((address >> 8) & 0xFF);
    buffer[5] = static_cast<uint8_t>(address & 0xFF);
    buffer[6] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buffer[7] = static_cast<uint8_t>(value & 0xFF);
    uart_write_bytes(uart_port, reinterpret_cast<const char*>(buffer), 8);
}

void send_dwin_raw_string(uart_port_t uart_port, uint16_t address, const char* str) {
    uint8_t len = ::strlen(str);
    uint8_t buffer[128];
    buffer[0] = 0x5A;
    buffer[1] = 0xA5;
    buffer[2] = len + 3; 
    buffer[3] = 0x82;
    buffer[4] = static_cast<uint8_t>((address >> 8) & 0xFF);
    buffer[5] = static_cast<uint8_t>(address & 0xFF);
    ::memcpy(&buffer[6], str, len);
    
    if (len % 2 != 0) {
        buffer[6 + len] = 0x00;
        buffer[2]++;
        uart_write_bytes(uart_port, reinterpret_cast<const char*>(buffer), 7 + len);
    } else {
        uart_write_bytes(uart_port, reinterpret_cast<const char*>(buffer), 6 + len);
    }
}

void update_dwin_workspace_apps(uart_port_t uart_port, int workspace_idx, const char* apps_list) {
    if (workspace_idx < 1 || workspace_idx > 10) return;

    uint16_t target_text_vp = 0x2000 + ((workspace_idx - 1) * 0x0100);

    char final_label[16];
    std::snprintf(final_label, sizeof(final_label), "[%d]", workspace_idx);
    send_dwin_raw_string(uart_port, target_text_vp, final_label);
}

extern "C" void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(MAIN_TAG, "Starting");

    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config = {}; 
    init_config.unit_id = ADC_UNIT_1;
    init_config.ulp_mode = ADC_ULP_MODE_DISABLE;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc1_handle));

    adc_oneshot_chan_cfg_t adc_cfg = {};
    adc_cfg.atten = ADC_ATTEN_DB_12; 
    adc_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, SENSOR_A_CHAN, &adc_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, SENSOR_B_CHAN, &adc_cfg));

    xTaskCreatePinnedToCore(
        fader_tracking_task, 
        "fader_task", 
        4096, 
        (void*)adc1_handle, 
        configMAX_PRIORITIES - 1, 
        nullptr, 
        1
    );

    const uart_port_t dwin_uart = UART_NUM_1;
    uart_config_t uart_config = {};
    uart_config.baud_rate = 115200; 
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity    = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(dwin_uart, &uart_config));
    
    ESP_ERROR_CHECK(uart_set_pin(dwin_uart, 4, 5, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(dwin_uart, 1024, 8192, 0, nullptr, 0));
    
    static Screen carousel({.uart_port = dwin_uart, .step_duration_ms = 300});
    
    loadAndSendEmbeddedJpeg(carousel);

    // INLINE STARTUP SLIDE ANIMATION SEQUENCE
    ESP_LOGI(MAIN_TAG, "[BOOT TEST] Initiating workspace sliding sequence...");
    vTaskDelay(pdMS_TO_TICKS(1000)); 
    carousel.scrollToWorkspace(9);
    for (int i = 0; i < 120; ++i) { carousel.update(); vTaskDelay(pdMS_TO_TICKS(10)); }
    vTaskDelay(pdMS_TO_TICKS(1000)); 
    carousel.scrollToWorkspace(1);
    for (int i = 0; i < 240; ++i) { carousel.update(); vTaskDelay(pdMS_TO_TICKS(10)); }
    vTaskDelay(pdMS_TO_TICKS(1000)); 
    carousel.scrollToWorkspace(5);
    for (int i = 0; i < 120; ++i) { carousel.update(); vTaskDelay(pdMS_TO_TICKS(10)); }

    // Set initial icon frame on boot (4 = Kitty)
    send_dwin_raw_int(dwin_uart, APP_ICON_VP, 4);

    static VoltMeter gauge({.gpio_pin = 14, .timer_sel = LEDC_TIMER_0, .channel_sel = LEDC_CHANNEL_0});
    static InputManager inputs({.sda_pin = 6, .scl_pin = 7, .pcf_address = 0x20});
    static BluetoothManager bt_serial("Fallout-Terminal");

    bool last_power = false;
    int32_t last_ticks = 0;
    int last_macro_key = 0;
    uint8_t last_calculated_workspace = 5; 

    // STABILIZED LOCK MECHANISMS
    bool pc_override_lock = false; 
    uint8_t target_pc_workspace = 5;

    while (true) {
        carousel.update();
        inputs.update();

        // 1. POWER BUTTON HANDLER
        bool current_power = inputs.isPowerButtonPressed();
        if (current_power && !last_power) {
            bt_serial.writeString("CMD:POWER_PRESS\n");
        }
        last_power = current_power;

        // 2. PHYSICAL STRIDE SLIDER HANDLER
        int32_t current_ticks = global_ticks;
        float current_mm = (current_ticks * POLE_PITCH_MM) / 4.0f; 

        int target_ws = static_cast<int>(current_mm / MM_PER_WORKSPACE) + 5; 
        if (target_ws < 1)  target_ws = 1;
        if (target_ws > 10) target_ws = 10;

        if (pc_override_lock && target_ws == target_pc_workspace) {
            pc_override_lock = false;
            last_calculated_workspace = static_cast<uint8_t>(target_ws);
            last_ticks = current_ticks; 
            ESP_LOGI(MAIN_TAG, "[LOCK RELEASE] Encoder aligned with PC context at WS %d.", target_ws);
        }

        if (current_ticks != last_ticks) {
            if (!pc_override_lock) {
                char fader_buf[32];
                snprintf(fader_buf, sizeof(fader_buf), "CMD:FADER:%.2f\n", current_mm);
                bt_serial.writeString(fader_buf);
                
                if (static_cast<uint8_t>(target_ws) != last_calculated_workspace) {
                    last_calculated_workspace = static_cast<uint8_t>(target_ws);
                    carousel.scrollToWorkspace(last_calculated_workspace);
                }
            }
            last_ticks = current_ticks;
        }

        // 3. WING MACRO KEY PAD HANDLER
        int current_macro = inputs.getPressedMacroKey();
        if (current_macro != last_macro_key) {
            if (current_macro != 0) {
                char macro_buf[24];
                snprintf(macro_buf, sizeof(macro_buf), "CMD:MACRO:%d\n", current_macro);
                bt_serial.writeString(macro_buf);
            }
            last_macro_key = current_macro;
        }

        // 4. INBOUND WIRELESS DATA ROUTER
        if (bt_serial.available()) {
            std::string msg = bt_serial.readStringUntil('\n');
            
            if (msg.rfind("STATS:", 0) == 0) {
                float cpu = 0.0f, ram = 0.0f;
                if (std::sscanf(msg.c_str(), "STATS:CPU:%f:RAM:%f", &cpu, &ram) == 2) {
                    float target_voltage = (cpu / 100.0f) * 5.0f;
                    gauge.setVoltage(target_voltage);
                }
            }
            else if (msg.rfind("TIME:", 0) == 0) {
                int hour = 0, minute = 0;
                if (std::sscanf(msg.c_str(), "TIME:%d:%d", &hour, &minute) == 2) {
                    char time_label[16];
                    std::snprintf(time_label, sizeof(time_label), "%02d:%02d", hour, minute);
                    
                    ESP_LOGI(MAIN_TAG, "[BLE INBOUND] Received Time Sync: %s", time_label);
                    send_dwin_raw_string(dwin_uart, TIME_TEXT_VP, time_label);
                }
            }
            else if (msg.rfind("DATE:", 0) == 0) {
                int year = 0, month = 0, day = 0;
                if (std::sscanf(msg.c_str(), "DATE:%d:%d:%d", &year, &month, &day) == 3) {
                    char date_label[24];
                    std::snprintf(date_label, sizeof(date_label), "%04d-%02d-%02d", year, month, day);
                    
                    ESP_LOGI(MAIN_TAG, "[BLE INBOUND] Received Date Sync: %s", date_label);
                    send_dwin_raw_string(dwin_uart, DATE_TEXT_VP, date_label);
                }
            }
            else if (msg.rfind("WEATHER:", 0) == 0) {
                char weather_layout[64] = {0};
                if (std::sscanf(msg.c_str(), "WEATHER:%63[^\n]", weather_layout) == 1) {
                    ESP_LOGI(MAIN_TAG, "[BLE INBOUND] Received Temperature: %s", weather_layout);
                    send_dwin_raw_string(dwin_uart, WEATHER_TEXT_VP, weather_layout);
                }
            }
            else if (msg.rfind("W_ICON:", 0) == 0) {
                int parsed_icon = 0;
                if (std::sscanf(msg.c_str(), "W_ICON:%d", &parsed_icon) == 1) {
                    ESP_LOGI(MAIN_TAG, "[BLE INBOUND] Received Weather Icon Index: %d", parsed_icon);
                    send_dwin_raw_int(dwin_uart, WEATHER_ICON_VP, static_cast<uint16_t>(parsed_icon));
                }
            }
            else if (msg.rfind("APP_ICON:", 0) == 0) {
                int parsed_icon = 0;
                if (std::sscanf(msg.c_str(), "APP_ICON:%d", &parsed_icon) == 1) {
                    ESP_LOGI(MAIN_TAG, "[BLE INBOUND] Received App Icon Index: %d", parsed_icon);
                    carousel.setAppIcon(static_cast<uint16_t>(parsed_icon));
                }
            }
            else if (msg.rfind("MUSIC:", 0) == 0) {
                char status[16] = {0};
                char artist[64] = {0};
                char title[64] = {0};

                if (std::sscanf(msg.c_str(), "MUSIC:%15[^:]:%63[^:]:%63[^\r\n]", status, artist, title) == 3) {
                    ESP_LOGI(MAIN_TAG, "[BLE INBOUND MUSIC] [%s] %s - %s", status, artist, title);

                    send_dwin_raw_string(dwin_uart, MUSIC_TITLE_VP, title);
                    send_dwin_raw_string(dwin_uart, MUSIC_ARTIST_VP, artist);
                    send_dwin_raw_string(dwin_uart, MUSIC_STATUS_VP, status);
                }
            }
            else if (msg.rfind("WS_MAP:", 0) == 0) {
                int target_workspace = 1;
                char apps_layout[256] = {0}; 
                
                int matched = std::sscanf(msg.c_str(), "WS_MAP:%d:%255[^:\n]", &target_workspace, apps_layout);
                if (matched >= 1) {
                    update_dwin_workspace_apps(dwin_uart, target_workspace, "");
                    
                    if (static_cast<uint8_t>(target_workspace) != target_pc_workspace) {
                        target_pc_workspace = static_cast<uint8_t>(target_workspace);
                        pc_override_lock = true; 
                        
                        carousel.scrollToWorkspace(target_pc_workspace);
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20)); 
    }
}