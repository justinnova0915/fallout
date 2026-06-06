// main/magnetic.cpp
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

// Hardware Mapping
#define SENSOR_A_CHAN ADC_CHANNEL_3 // GPIO 4
#define SENSOR_B_CHAN ADC_CHANNEL_4 // GPIO 5
#define THRESHOLD 1780 

// Shared variables across cores exposed globally
volatile int32_t global_ticks = 0;
volatile int global_raw_a = 0;
volatile int global_raw_b = 0;
volatile const char* global_dir = "IDLE    ";

// Dedicated tracking loop running on Core 1
/**
 * @brief Task for tracking the slider position
 * @details two SS49E hall effect sensors are placed quarter phase (quadrature) from each other.
 *          we track the difference in magnetic pole and calculate the distancer traveled via number
 *          of full phases we detected.
 * @param pvParameters 
 */
void fader_tracking_task(void* pvParameters) {
    adc_oneshot_unit_handle_t adc1_handle = (adc_oneshot_unit_handle_t)pvParameters;
    int last_state = 0;

    while (1) {
        int raw_a = 0, raw_b = 0;
        
        if (adc_oneshot_read(adc1_handle, SENSOR_A_CHAN, &raw_a) == ESP_OK &&
            adc_oneshot_read(adc1_handle, SENSOR_B_CHAN, &raw_b) == ESP_OK) {
            
            global_raw_a = raw_a;
            global_raw_b = raw_b;
            
            // Determines the state of the sensors
            int state_a = (raw_a > THRESHOLD) ? 1 : 0;
            int state_b = (raw_b > THRESHOLD) ? 1 : 0;
            int current_state = (state_a << 1) | state_b;
            
            // Moving forward follows a 2 -> 3 -> 1 -> 0 pattern
            if (current_state != last_state) {
                // Quadrature direction decoding
                if ((last_state == 0 && current_state == 2) || 
                    (last_state == 2 && current_state == 3) || 
                    (last_state == 3 && current_state == 1) || 
                    (last_state == 1 && current_state == 0)) {
                    global_ticks = global_ticks + 1;
                    global_dir = "FORWARD ";
                } else {
            // Moving backward follows any other pattern
                    global_ticks = global_ticks - 1;
                    global_dir = "BACKWARD";
                }
                last_state = current_state;
            }
        }

        // skips the OS delay
        portYIELD(); 
    }
}