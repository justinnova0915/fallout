// main/magnetic.hpp
#pragma once

#include <cstdint>

/**
 * @brief Shared global telemetry for the magnetic fader tracking task.
 */
// Expose shared volatile tracking variables to main.cpp and other modules
extern volatile int32_t global_ticks;
extern volatile int global_raw_a;
extern volatile int global_raw_b;
extern volatile const char* global_dir;

// Expose the Core 1 high-speed tracking task signature
void fader_tracking_task(void *pvParameters);