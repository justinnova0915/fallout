// main/main.cpp
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "driver/uart.h"

// File imports
#include "screen.hpp"
#include "voltmeter.hpp"
#include "input.hpp"
#include "magnetic.hpp"
#include "ble.hpp"

static const char* MAIN_TAG = "FALLOUT_MAIN";

// Hardware Channel Mappings from your encoder spec
#define SENSOR_A_CHAN ADC_CHANNEL_3 // GPIO 4
#define SENSOR_B_CHAN ADC_CHANNEL_4 // GPIO 5
#define POLE_PITCH_MM 2.0f

/**
 * @brief putting everything together
 * @details Inits all hardware systems and drives cross-core coordination pipelines.
 */
extern "C" void app_main(void) {
    ESP_LOGI(MAIN_TAG, "Starting");

    // Analog Digital Converter init
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

    // Puts the slider code onto its own core (Core 1)
    xTaskCreatePinnedToCore(
        fader_tracking_task, 
        "fader_task", 
        4096, 
        (void*)adc1_handle, 
        configMAX_PRIORITIES - 1, 
        nullptr, 
        1
    );

    // Screen init (UART interface for DWIN display panel)
    const uart_port_t dwin_uart = UART_NUM_1;
    uart_config_t uart_config = {};
    uart_config.baud_rate = 115200; 
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity    = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(dwin_uart, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(dwin_uart, 16, 17, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)); 
    ESP_ERROR_CHECK(uart_driver_install(dwin_uart, 1024, 8192, 0, nullptr, 0));
    
    static Screen carousel({.uart_port = dwin_uart, .step_duration_ms = 300});
    
    // Init other peripheral control objects
    static VoltMeter gauge({.gpio_pin = 14, .timer_sel = LEDC_TIMER_0, .channel_sel = LEDC_CHANNEL_0});
    static InputManager inputs({.sda_pin = 6, .scl_pin = 7, .pcf_address = 0x20});
    static BluetoothManager bt_serial("Fallout-Terminal");

    // Tracking registers for edge detection
    uint32_t last_telemetry_time = 0;
    bool last_power = false;
    int32_t last_ticks = 0;
    int last_macro_key = 0;

    // Core execution loop running on Core 0
    while (true) {
        // Step local state animations and process key matrix scans
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
        float current_mm = (current_ticks * POLE_PITCH_MM) / 4.0f; // Calculate physical mm displacement

        if (current_ticks != last_ticks) {
            char fader_buf[32];
            snprintf(fader_buf, sizeof(fader_buf), "CMD:FADER:%.2f\n", current_mm);
            bt_serial.writeString(fader_buf);
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
            
            // Check Match Token A: Outbound Telemetry Metrics (PC -> Voltmeter Gage)
            if (msg.rfind("STATS:", 0) == 0) {
                float cpu = 0.0f, ram = 0.0f;
                if (sscanf(msg.c_str(), "STATS:CPU:%f:RAM:%f", &cpu, &ram) == 2) {
                    float target_voltage = (cpu / 100.0f) * 5.0f;
                    gauge.setVoltage(target_voltage);
                }
            }
            // Check Match Token B: Hyprland Workspace Viewport Configuration (PC -> Carousel Engine)
            else if (msg.rfind("WS_MAP:", 0) == 0) {
                int active_workspace = 1;
                // Buffer to hold app list layout token data without breaking execution path
                char apps_layout[256] = {0}; 
                
                if (sscanf(msg.c_str(), "WS_MAP:%d:%255s", &active_workspace, apps_layout) >= 1) {
                    ESP_LOGI(MAIN_TAG, "[RECEPTION ROUTER] Latching spotlight focus viewport to workspace index: %d", active_workspace);
                    
                    // Dispatch the updated viewport coordinate target down to your screen animation loop
                    carousel.scrollToWorkspace(static_cast<uint8_t>(active_workspace));
                }
            }
        }

        // Execution throttling pass to allow background low-priority operations to cycle
        vTaskDelay(pdMS_TO_TICKS(20)); 
    }
}