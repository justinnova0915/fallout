#pragma once

#include <cstdint>
#include "driver/ledc.h"

/**
 * @brief Drives the 0V to 5V gauge output using a PWM timer channel.
 */
class VoltMeter {
public:
    /**
     * @brief PWM hardware mapping used to render the gauge voltage.
     */
    struct Config {
        int gpio_pin;
        ledc_timer_t timer_sel;
        ledc_channel_t channel_sel;
    };

    explicit VoltMeter(const Config& config);
    ~VoltMeter() = default;

    // Set voltage output representation (0.0f to 5.0f Volts)
    void setVoltage(float voltage);

private:
    Config cfg_;
    static constexpr uint32_t PWM_FREQ_HZ = 5000; // 5kHz keeps the needle steady
    static constexpr ledc_mode_t SPEED_MODE = LEDC_LOW_SPEED_MODE;
    static constexpr ledc_timer_bit_t DUTY_RES = LEDC_TIMER_8_BIT; // 0-255 steps
};