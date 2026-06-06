// main/input.cpp
#include "input.hpp"
#include "esp_log.h"

static const char* TAG = "INPUT_SYSTEM";

/**
 * @brief Input Manager Constructor
 */
InputManager::InputManager(const Config& config) 
    : cfg_(config), 
      bus_handle_(nullptr), 
      pcf_device_handle_(nullptr),
      power_btn_pressed_(false), 
      pressed_key(0) 
    {
    initI2C();
}

// Inits the I2C master bus. Creates the I2C connection
void InputManager::initI2C() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(cfg_.sda_pin);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(cfg_.scl_pin);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true; 

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle_));

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = cfg_.pcf_address;
    dev_cfg.scl_speed_hz = 100000; 

    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle_, &dev_cfg, &pcf_device_handle_));
    ESP_LOGI(TAG, "PCF8574 mounted at 0x%02X", cfg_.pcf_address);
}

/**
 * @brief Reads the state of the macro pad
 * @details The PCF expander uses 8-bit quasi-bidirectional I/O pins, which means
 *          we have to pull all the pins high, wait for the input pins to pull 
 *          them low if they are active, then read the data from the board.
 * @param pin_mask a bit mask of one byte, determining the mode of the pins (I/O)
 */
uint8_t InputManager::readPCF(uint8_t pin_mask) {
    uint8_t rx_data = 0xFF;
    // Transmit output mask and sample input pin states back over the bus
    i2c_master_transmit_receive(pcf_device_handle_, &pin_mask, 1, &rx_data, 1, 50);
    return rx_data;
}

/**
 * @brief Gets the latest state of the PCF board.
 * @details A wrapper of readPCF
 */
void InputManager::update() {

    // power button check
    uint8_t power_check_mask = 0xFF; 
    uint8_t base_read = readPCF(power_check_mask);
    power_btn_pressed_ = ((base_read & (1 << 6)) == 0);

    uint16_t detected_key = 0;

    /*  pull rows low one by one, and check which cols are low
        pin 0 - 2 C3-C1
        Pin 3 - 5 R3-R1
        pin 6 - power button */
    for (int r = 0; r < 3; r++) {
        uint8_t write_mask = 0xFF & ~(1 << (5 - r));
        uint8_t read_val = readPCF(write_mask);

        for (int c = 0; c < 3; c++) {
            if ((read_val & (1 << (2 - c))) == 0) {
                uint8_t key_index = (3 * r) + c;
                
                detected_key |= (1 << key_index);
            }
        }
    }
    
    pressed_key = detected_key;
}

bool InputManager::isPowerButtonPressed() const { return power_btn_pressed_; }
int InputManager::getPressedMacroKey() const { return pressed_key; }