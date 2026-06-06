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
 * @details inits everything :P
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

    // Puts the slider code onto its own core
    xTaskCreatePinnedToCore(
        fader_tracking_task, 
        "fader_task", 
        4096, 
        (void*)adc1_handle, 
        configMAX_PRIORITIES - 1, 
        NULL, 
        1
    );

    // Screen init
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
    
    // init other peripherals
    static VoltMeter gauge({.gpio_pin = 14, .timer_sel = LEDC_TIMER_0, .channel_sel = LEDC_CHANNEL_0});
    static InputManager inputs({.sda_pin = 6, .scl_pin = 7, .pcf_address = 0x20});
    static BluetoothManager bt_serial("Fallout-Terminal");

    // tracking vars
    uint32_t last_telemetry_time = 0;
    
    bool last_power = false;
    int32_t last_ticks = 0;
    int last_macro_key = 0;

    // main loop
    while (true) {
        carousel.update();
        inputs.update();

        // power btn
        bool current_power = inputs.isPowerButtonPressed();
        if (current_power && !last_power) {
            bt_serial.writeString("CMD:POWER_PRESS\n");
        }
        last_power = current_power;

        // slider
        int32_t current_ticks = global_ticks;
        float current_mm = (current_ticks * POLE_PITCH_MM) / 4.0f; // Calculate physical mm distance

        if (current_ticks != last_ticks) {
            char fader_buf[32];
            snprintf(fader_buf, sizeof(fader_buf), "CMD:FADER:%.2f\n", current_mm);
            bt_serial.writeString(fader_buf);
            last_ticks = current_ticks;
        }

        // Macro Keys
        int current_macro = inputs.getPressedMacroKey();
        if (current_macro != last_macro_key) {
            if (current_macro != 0) {
                char macro_buf[24];
                snprintf(macro_buf, sizeof(macro_buf), "CMD:MACRO:%d\n", current_macro);
                bt_serial.writeString(macro_buf);
            }
            last_macro_key = current_macro;
        }

        // Bluetooth Incoming Data
        if (bt_serial.available()) {
            std::string msg = bt_serial.readStringUntil('\n');
            float cpu = 0.0f, ram = 0.0f;
            if (sscanf(msg.c_str(), "STATS:CPU:%f:RAM:%f", &cpu, &ram) == 2) {
                float target_voltage = (cpu / 100.0f) * 5.0f;
                gauge.setVoltage(target_voltage);
            }
        }

        // Debug prints
        uint32_t now_ms = esp_log_timestamp();
        if (now_ms - last_telemetry_time >= 40) { 
            last_telemetry_time = now_ms;
            
            printf(">RawA:%d\n", global_raw_a);
            printf(">RawB:%d\n", global_raw_b);
            printf(">PosMM:%.2f\n", current_mm);
            printf(">DirStr:%s\n", global_dir);
            printf(">MacroKey:%d\n", current_macro);
            printf(">PowerBtn:%d\n", current_power ? 1 : 0);
        }

        vTaskDelay(pdMS_TO_TICKS(20)); 
    }
}