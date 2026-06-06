// main/voltmeter.cpp
#include "voltmeter.hpp"
#include "esp_log.h"

/**
 * @brief Voltmeter Constructor
 */
VoltMeter::VoltMeter(const Config& config) : cfg_(config) {
    // Configure PWM Timer
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode       = SPEED_MODE;
    ledc_timer.duty_resolution  = DUTY_RES;
    ledc_timer.timer_num        = cfg_.timer_sel;
    ledc_timer.freq_hz          = PWM_FREQ_HZ;
    ledc_timer.clk_cfg          = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Configure PWM Channel linked to your transistor GPIO
    ledc_channel_config_t ledc_channel = {};
    ledc_channel.speed_mode     = SPEED_MODE;
    ledc_channel.channel        = cfg_.channel_sel;
    ledc_channel.timer_sel      = cfg_.timer_sel;
    ledc_channel.intr_type      = LEDC_INTR_DISABLE;
    ledc_channel.gpio_num       = cfg_.gpio_pin;
    ledc_channel.duty           = 0; 
    ledc_channel.hpoint         = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

// Uses pwm to set the voltage
void VoltMeter::setVoltage(float voltage) {
    if (voltage < 0.0f) voltage = 0.0f;
    if (voltage > 5.0f) voltage = 5.0f;

    // Map 0V-5V linearly to 0-255 PWM duty cycle
    uint32_t duty = static_cast<uint32_t>((voltage / 5.0f) * 255.0f);
    
    ESP_ERROR_CHECK(ledc_set_duty(SPEED_MODE, cfg_.channel_sel, duty));
    ESP_ERROR_CHECK(ledc_update_duty(SPEED_MODE, cfg_.channel_sel));
}