// main/input.hpp
#pragma once

#include <cstdint>
#include "driver/i2c_master.h"

/**
 * @brief Tracks left-wing input hardware through the PCF8574 expander.
 * @details The scanner reads the power button state and macro keypad matrix
 *          so the main loop can forward user actions to the host.
 */
class InputManager {
public:
    /**
     * @brief Hardware configuration for the input-expander interface.
     */
    struct Config {
        int sda_pin;
        int scl_pin;
        uint8_t pcf_address; // PCF8574 on Left Wing 
    };

    explicit InputManager(const Config& config);
    ~InputManager() = default;

    // Called inside app_main loop to execute the I2C scans
    void update(); 

    bool isPowerButtonPressed() const;
    int getPressedMacroKey() const; 

private:
    Config cfg_;
    i2c_master_bus_handle_t bus_handle_;
    i2c_master_dev_handle_t pcf_device_handle_;   

    bool power_btn_pressed_;
    int pressed_key;

    void initI2C();
    uint8_t readPCF(uint8_t data_to_write);
};