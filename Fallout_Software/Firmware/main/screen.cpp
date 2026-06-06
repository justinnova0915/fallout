// main/screen.cpp
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include "screen.hpp"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "CAROUSEL_ENGINE";

/**
 * @brief Initializes the display engine and boot layout for the carousel view.
 */

// ============================================================================
// EMBEDDED BINARY SYMBOLS DECLARATION
// These symbols point directly to the embedded 1.jpg file in the program's flash memory segment.
// ============================================================================
extern "C" {
    extern const uint8_t image_start[] asm("_binary_1_jpg_start");
    extern const uint8_t image_end[]   asm("_binary_1_jpg_end");
}

// ============================================================================
// Class Implementation
// ============================================================================

static constexpr uint16_t ADDR_INCREMENT = 0x0100;
static constexpr uint16_t ICON_SP_BASE   = 0x3000;
static constexpr uint16_t ICON_VP_BASE   = 0x4000;
static constexpr uint16_t TEXT_SP_BASE   = 0x5000;
static constexpr uint16_t TEXT_VP_BASE   = 0x2000; // Text content baseline address register 

static constexpr uint16_t COLOR_YELLOW   = 0xFFE0; 
static constexpr uint16_t COLOR_BLACK    = 0x0000; 

static constexpr uint16_t SIZE_PACKED_REST   = 0x2424; // X=36, Y=36
static constexpr uint16_t SIZE_PACKED_ACTIVE = 0x4848; // X=72, Y=72

Screen::Screen(const Config& config)
    : cfg_(config),
      state_(State::IDLE),
      current_focus_idx_(4),       // System starts centered naturally on Element 4
      target_step_idx_(4),
      ultimate_target_idx_(4),
      stride_balance_delta_(0),    
      current_global_pos_(4),      // INIT floor tracker
      queued_workspace_(4),        // INIT queue tracker
      is_floor_move_(false),       // INIT floor lock
      g_local_slide_x(0.0f),
      shifting_right_(false),
      starting_x_(10.0f),
      slot_spacing_(90.0f),    
      transition_start_time_us_(0),
      last_log_time_ms_(0),
      update_ticker_(0) {
    
    for (int i = 0; i < 10; ++i) {
        uniform_slots_x_[i] = static_cast<uint16_t>(starting_x_ + (i * slot_spacing_));
    }

    for (int i = 0; i < 10; ++i) {
        last_sent_frames_[i] = 0xFFFF;
        text_values_[i] = i+1; // Baseline initialization
    }

    // Clean boot sweep layout initialization sequence
    for (int i = 0; i < 10; ++i) {
        uint16_t init_frame = (i == current_focus_idx_) ? 7 : 0;
        last_sent_frames_[i] = init_frame;
        sendDwinWriteSingle(ICON_VP_BASE + (i * ADDR_INCREMENT), init_frame);
        
        // Force ALL text elements to initialize strictly as black on system boot
        sendDwinWriteSingle(TEXT_SP_BASE + (i * ADDR_INCREMENT) + 3, COLOR_BLACK);

        // Force ALL text components down to standard rest layout sizing (36x36 dots) at boot
        uint16_t init_size = (i == current_focus_idx_) ? 0x4848 : 0x2424;
        sendDwinWriteSingle(TEXT_SP_BASE + (i * ADDR_INCREMENT) + 0x0A, init_size);

        // Formatting with brackets here on boot init using the dedicated tracker
        char str_buf[8];
        snprintf(str_buf, sizeof(str_buf), "[%d]", text_values_[i]);
        sendDwinString(TEXT_VP_BASE + (i * ADDR_INCREMENT), str_buf);
    }

    // Pre-populate hardware location defaults safely
    updateHardwarePositions(0.0f);
    ESP_LOGI(TAG, "Dynamic Relay Multi-Stride Engine loaded. Anchored on Index [%d]", current_focus_idx_);
}

void Screen::scrollToWorkspace(uint8_t target_idx) {
    if (target_idx > 0) {
        target_idx -= 1; 
    } else {
        target_idx = 0; 
    }

    if (target_idx > 9) target_idx = 9; 
    ultimate_target_idx_ = target_idx;
    
    stride_balance_delta_ += static_cast<int8_t>(target_idx) - static_cast<int8_t>(queued_workspace_); 
    queued_workspace_ = target_idx;
}

void Screen::swipeLeft() {
    if (state_ != State::IDLE) return;

    shifting_right_ = false;
    target_step_idx_ = current_focus_idx_ + 1; 
    
    transition_start_time_us_ = esp_timer_get_time();
    state_ = State::MOVING;
    ESP_LOGI(TAG, "[STRIDE LEFT] Moving: Element [%u] -> Slot 4 Spotlight Pass", (unsigned int)target_step_idx_);
}

void Screen::swipeRight() {
    if (state_ != State::IDLE) return;

    shifting_right_ = true;
    target_step_idx_ = current_focus_idx_ - 1; 
    
    transition_start_time_us_ = esp_timer_get_time();
    state_ = State::MOVING;
    ESP_LOGI(TAG, "[STRIDE RIGHT] Moving: Element [%u] -> Slot 4 Spotlight Pass", (unsigned int)target_step_idx_);
}

void Screen::updateHardwarePositions(float ease_progress) {
    update_ticker_++;
    bool allow_property_update = (state_ == State::IDLE) || (update_ticker_ % 3 == 0);

    for (uint16_t i = 0; i < 10; ++i) {
        float slot_x = static_cast<float>(uniform_slots_x_[i]);
        float split_offset = 0.0f;

        if (state_ == State::MOVING) {
            if (!shifting_right_) { // SWIPE LEFT (Moving Forward)
                if (i < current_focus_idx_) {
                    split_offset = -CUSHION_PADDING;
                } else if (i > target_step_idx_) {
                    split_offset = CUSHION_PADDING;
                } else if (i == current_focus_idx_) {
                    split_offset = -CUSHION_PADDING * ease_progress; 
                } else if (i == target_step_idx_) {
                    split_offset = CUSHION_PADDING * (1.0f - ease_progress); 
                }
            } else { // SWIPE RIGHT (Moving Backward)
                if (i < target_step_idx_) {
                    split_offset = -CUSHION_PADDING;
                } else if (i > current_focus_idx_) {
                    split_offset = CUSHION_PADDING;
                } else if (i == target_step_idx_) {
                    split_offset = -CUSHION_PADDING * (1.0f - ease_progress); 
                } else if (i == current_focus_idx_) {
                    split_offset = CUSHION_PADDING * ease_progress;         
                }
            }
        } else {
            if (i < current_focus_idx_) {
                split_offset = -CUSHION_PADDING;
            } else if (i > current_focus_idx_) {
                split_offset = CUSHION_PADDING;
            } else {
                split_offset = 0.0f;
            }
        }

        float active_slide_x = g_local_slide_x;
        if (is_floor_move_) {
            active_slide_x = 0.0f;
        }

        uint16_t dynamic_x = static_cast<uint16_t>(slot_x + split_offset + active_slide_x);

        // --- PART 2: DYNAMIC INDEPENDENT Y-AXIS CALCULATIONS ---
        float icon_y_calculated = static_cast<float>(FORCED_Y_BASELINE);
        float text_y_calculated = static_cast<float>(FORCED_Y_BASELINE + REST_TEXT_Y_OFFSET);

        if (state_ == State::IDLE) {
            if (i == current_focus_idx_) {
                icon_y_calculated += static_cast<float>(ACTIVE_ICON_Y_OFFSET);
                text_y_calculated += static_cast<float>(ACTIVE_TEXT_Y_OFFSET);
            }
        } else if (state_ == State::MOVING) {
            float vertical_interpolation = 0.0f;
            if (i == current_focus_idx_) {
                vertical_interpolation = 1.0f - ease_progress; 
            } else if (i == target_step_idx_) {
                vertical_interpolation = ease_progress;        
            }

            if (vertical_interpolation > 0.0f) {
                icon_y_calculated += (static_cast<float>(ACTIVE_ICON_Y_OFFSET) * vertical_interpolation);
                text_y_calculated += (static_cast<float>(ACTIVE_TEXT_Y_OFFSET) * vertical_interpolation);
            }
        }

        moveDwinElementsPacked(ICON_SP_BASE + (i * ADDR_INCREMENT), dynamic_x, static_cast<uint16_t>(icon_y_calculated), false);

        float text_x = static_cast<float>(dynamic_x) + 200.0f;
        float text_size_dots = 36.0f;
        if (state_ == State::IDLE && i == current_focus_idx_) {
            text_size_dots = 72.0f;
        } else if (state_ == State::MOVING) {
            if (i == current_focus_idx_) {
                text_size_dots = 36.0f + (36.0f * (1.0f - ease_progress));
            } else if (i == target_step_idx_) {
                text_size_dots = 36.0f + (36.0f * ease_progress);
            }
        }

        if (text_size_dots > 36.0f) {
            text_x -= (text_size_dots - 36.0f) * ACTIVE_TEXT_CENTERING_DELTA;
        }

        if (text_x < 0.0f) {
            text_x = 0.0f;
        }

        moveDwinElementsPacked(TEXT_SP_BASE + (i * ADDR_INCREMENT), static_cast<uint16_t>(text_x), static_cast<uint16_t>(text_y_calculated), true);

        if (allow_property_update) {
            uint16_t text_color = 0x0000; 
            uint16_t font_size  = 0x2424; 

            if (state_ == State::IDLE) {
                if (i == current_focus_idx_) {
                    text_color = 0xFFE0; 
                    font_size  = 0x7272; 
                }
            } else if (state_ == State::MOVING) {
                float calculated_weight = 0.0f;
                if (i == current_focus_idx_) {
                    calculated_weight = 1.0f - ease_progress; 
                } else if (i == target_step_idx_) {
                    calculated_weight = ease_progress;        
                }

                if (calculated_weight > 0.0f) {
                    uint8_t interpolated_r = static_cast<uint8_t>(31.0f * calculated_weight);
                    uint8_t interpolated_g = static_cast<uint8_t>(63.0f * calculated_weight);
                    text_color = (interpolated_r << 11) | (interpolated_g << 5);

                    uint8_t size_dots = static_cast<uint8_t>(36.0f + (36.0f * calculated_weight));
                    font_size = (size_dots << 8) | size_dots;
                }
            }

            sendDwinWriteSingle(TEXT_SP_BASE + (i * ADDR_INCREMENT) + 3, text_color);
            sendDwinWriteSingle(TEXT_SP_BASE + (i * ADDR_INCREMENT) + 0x0A, font_size);
        }

        uint16_t target_frame = 0;
        if (state_ == State::IDLE) {
            target_frame = (i == current_focus_idx_) ? 7 : 0;
        } else if (state_ == State::MOVING) {
            if (i == current_focus_idx_) {
                target_frame = static_cast<uint16_t>(std::round((1.0f - ease_progress) * 7.0f));
            } else if (i == target_step_idx_) {
                target_frame = static_cast<uint16_t>(std::round(ease_progress * 7.0f));
            } else {
                target_frame = 0;
            }
        }

        if (target_frame > 7) target_frame = 7;

        if (target_frame != last_sent_frames_[i]) {
            last_sent_frames_[i] = target_frame;
            sendDwinWriteSingle(ICON_VP_BASE + (i * ADDR_INCREMENT), target_frame);
        }
    }

    uint32_t now = esp_timer_get_time() / 1000;
    if (now - last_log_time_ms_ >= 50) {
        last_log_time_ms_ = now;
        ESP_LOGI(TAG, "Offset X: %+.1f | Remaining Balance Delta: %d", g_local_slide_x, (int)stride_balance_delta_);
    }
}

// Steps the carousel state machine and applies the next display frame.
void Screen::update() {
    if (state_ == State::IDLE) {
        if (stride_balance_delta_ > 0) {
            stride_balance_delta_--; 
            current_global_pos_++;
            is_floor_move_ = (current_global_pos_ <= 4); 
            swipeLeft(); 
        } else if (stride_balance_delta_ < 0) {
            stride_balance_delta_++; 
            current_global_pos_--;
            is_floor_move_ = (current_global_pos_ < 4); 
            swipeRight(); 
        } else {
            // CRITICAL UART SATURATION SOLVED: 
            // Do NOT call updateHardwarePositions(0.0f) inside IDLE. 
            // It floods the serial bus with ~480 bytes every 10ms with redundant coordinates.
        }
        return;
    }

    if (state_ == State::MOVING) {
        int64_t elapsed_ms = (esp_timer_get_time() - transition_start_time_us_) / 1000;
        float progress = static_cast<float>(elapsed_ms) / static_cast<float>(cfg_.step_duration_ms);

        if (progress >= 1.0f) {
            g_local_slide_x = 0.0f; 
            
            uint8_t old_focus = current_focus_idx_;
            uint8_t moving_into_active = target_step_idx_;
            
            if (is_floor_move_) {
                current_focus_idx_ = moving_into_active; 
            } else {
                current_focus_idx_ = 4;
            }
            state_ = State::IDLE;

            update_ticker_ = 0; 
            updateHardwarePositions(1.0f);

            if (old_focus != current_focus_idx_) {
                last_sent_frames_[old_focus] = 0;
                sendDwinWriteSingle(ICON_VP_BASE + (old_focus * ADDR_INCREMENT), 0);
            } else {
                last_sent_frames_[moving_into_active] = 0;  
                sendDwinWriteSingle(ICON_VP_BASE + (moving_into_active * ADDR_INCREMENT), 0);
            }
            
            last_sent_frames_[current_focus_idx_] = 7;                   
            sendDwinWriteSingle(ICON_VP_BASE + (current_focus_idx_ * ADDR_INCREMENT), 7);

            if (!is_floor_move_) {
                for (int i = 0; i < 10; ++i) {
                    if (!shifting_right_) {
                        text_values_[i] += 1; 
                    } else {
                        text_values_[i] -= 1; 
                    }
                    
                    char label_str[8];
                    snprintf(label_str, sizeof(label_str), "[%d]", text_values_[i]);
                    sendDwinString(TEXT_VP_BASE + (i * ADDR_INCREMENT), label_str);
                }
            }

            ESP_LOGI(TAG, "[STRIDE COMPLETE] Snap-back warp clear. Remaining steps to process: %d", (int)stride_balance_delta_);
        } else {
            float ease_out = 1.0f - std::pow(1.0f - progress, 3.0f);
            
            if (!shifting_right_) {
                g_local_slide_x = -slot_spacing_ * ease_out; 
            } else {
                g_local_slide_x = slot_spacing_ * ease_out;  
            }
            
            updateHardwarePositions(ease_out);
        }
    }
}

void Screen::sendDwinWriteSingle(uint16_t address, uint16_t value) {
    uint8_t buffer[8];
    buffer[0] = 0x5A; buffer[1] = 0xA5; buffer[2] = 0x05; buffer[3] = 0x82; 
    buffer[4] = static_cast<uint8_t>((address >> 8) & 0xFF);
    buffer[5] = static_cast<uint8_t>(address & 0xFF);
    buffer[6] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buffer[7] = static_cast<uint8_t>(value & 0xFF);
    uart_write_bytes(cfg_.uart_port, reinterpret_cast<const char*>(buffer), 8);
}

void Screen::sendDwinString(uint16_t address, const char* str) {
    uint8_t len = strlen(str);
    uint8_t buffer[128];
    buffer[0] = 0x5A;
    buffer[1] = 0xA5;
    buffer[2] = len + 3; 
    buffer[3] = 0x82;
    buffer[4] = static_cast<uint8_t>((address >> 8) & 0xFF);
    buffer[5] = static_cast<uint8_t>(address & 0xFF);
    memcpy(&buffer[6], str, len);
    
    if (len % 2 != 0) {
        buffer[6 + len] = 0x00;
        buffer[2]++;
        uart_write_bytes(cfg_.uart_port, reinterpret_cast<const char*>(buffer), 7 + len);
    } else {
        uart_write_bytes(cfg_.uart_port, reinterpret_cast<const char*>(buffer), 6 + len);
    }
}

void Screen::moveDwinElementsPacked(uint16_t base_sp_address, uint16_t dynamic_x, uint16_t y, bool is_text) {
    uint16_t target_coord_reg = base_sp_address + 1; 
    uint8_t coord_buffer[10];
    coord_buffer[0] = 0x5A; coord_buffer[1] = 0xA5; coord_buffer[2] = 0x07; coord_buffer[3] = 0x82; 
    coord_buffer[4] = static_cast<uint8_t>((target_coord_reg >> 8) & 0xFF);
    coord_buffer[5] = static_cast<uint8_t>(target_coord_reg & 0xFF);
    coord_buffer[6] = static_cast<uint8_t>((dynamic_x >> 8) & 0xFF);
    coord_buffer[7] = static_cast<uint8_t>(dynamic_x & 0xFF);
    coord_buffer[8] = static_cast<uint8_t>((y >> 8) & 0xFF);
    coord_buffer[9] = static_cast<uint8_t>(y & 0xFF);
    uart_write_bytes(cfg_.uart_port, reinterpret_cast<const char*>(coord_buffer), 10);

    if (is_text) {
        uint16_t target_box_reg = base_sp_address + 4; 
        uint8_t box_buffer[12];
        box_buffer[0] = 0x5A; box_buffer[1] = 0xA5; box_buffer[2] = 0x09; box_buffer[3] = 0x82; 
        box_buffer[4] = static_cast<uint8_t>((target_box_reg >> 8) & 0xFF);
        box_buffer[5] = static_cast<uint8_t>(target_box_reg & 0xFF);
        box_buffer[6] = static_cast<uint8_t>((dynamic_x >> 8) & 0xFF);
        box_buffer[7] = static_cast<uint8_t>(dynamic_x & 0xFF);
        box_buffer[8] = static_cast<uint8_t>((y >> 8) & 0xFF);
        box_buffer[9] = static_cast<uint8_t>(y & 0xFF);
        box_buffer[10] = static_cast<uint8_t>(((dynamic_x + TEXT_BOX_WIDTH) >> 8) & 0xFF);
        box_buffer[11] = static_cast<uint8_t>((dynamic_x + TEXT_BOX_WIDTH) & 0xFF);
        uart_write_bytes(cfg_.uart_port, reinterpret_cast<const char*>(box_buffer), 12);
    }
}

// ============================================================================
// CHECKPOINT DEBUG ENGINE IMPLEMENTATION
// ============================================================================

bool Screen::waitForDwinAck(uint32_t timeout_ms) {
    uint8_t rx_byte;
    uint8_t match_seq[6] = {0x5A, 0xA5, 0x03, 0x82, 0x4F, 0x4B}; // "5A A5 03 82 OK"
    size_t match_idx = 0;
    
    int64_t start_time = esp_timer_get_time() / 1000;
    
    while (((esp_timer_get_time() / 1000) - start_time) < timeout_ms) {
        int len = uart_read_bytes(cfg_.uart_port, &rx_byte, 1, pdMS_TO_TICKS(2));
        if (len > 0) {
            // Dump raw back-channel bytes sequentially
            printf("%02X ", (unsigned int)rx_byte); 
            fflush(stdout);

            if (rx_byte == match_seq[match_idx]) {
                match_idx++;
                if (match_idx == 6) {
                    printf(" -> [MATCH ACK OK]\n");
                    return true;
                }
            } else {
                match_idx = (rx_byte == 0x5A) ? 1 : 0;
            }
        }
    }
    printf(" -> [TIMEOUT ERROR]\n");
    return false;
}

bool Screen::sendJpegImageOnTheFly(const uint8_t* jpeg_data, size_t jpeg_size) {
    // --- JPEG INTEGRITY SANITY CHECK ---
    // Standard JPEGs MUST start with 0xFFD8 and end with 0xFFD9. If they don't, the compilation
    // asset pipeline has corrupted the file structure or it's not a real image.
    if (jpeg_size < 4) {
        ESP_LOGE("CHECKPOINT", "[FATAL] Image payload is way too small to be a JPEG.");
        return false;
    }
    
    ESP_LOGI("CHECKPOINT", "JPEG Header Signature: %02X %02X (Expected: FF D8)", jpeg_data[0], jpeg_data[1]);
    ESP_LOGI("CHECKPOINT", "JPEG Footer Signature: %02X %02X (Expected: FF D9)", jpeg_data[jpeg_size - 2], jpeg_data[jpeg_size - 1]);
    
    if (jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        ESP_LOGW("CHECKPOINT", "[WARNING] File does NOT begin with a valid JPEG SOI header! Decompressor will fail.");
    }
    if (jpeg_data[jpeg_size - 2] != 0xFF || jpeg_data[jpeg_size - 1] != 0xD9) {
        ESP_LOGW("CHECKPOINT", "[WARNING] File does NOT end with a valid JPEG EOI footer! Decompressor will fail.");
    }

    // --- STEP 2: STREAM JPEG DATA STARTING EXACTLY AT VP+2 (0x8002) ---
    uint16_t current_vp = 0x8002; 
    size_t bytes_sent = 0;
    const size_t CHUNK_SIZE = 240; 
    uint32_t chunk_counter = 0;

    ESP_LOGI("CHECKPOINT", "[START] Total JPEG payload size to transmit: %u bytes", (unsigned int)jpeg_size);
    
    // Flush input buffers clean
    uart_flush_input(cfg_.uart_port);

    while (bytes_sent < jpeg_size) {
        size_t remaining = jpeg_size - bytes_sent;
        size_t current_chunk_size = std::min(CHUNK_SIZE, remaining);
        
        uint16_t dwin_packet_length = 3 + current_chunk_size;
        bool pad_byte = (current_chunk_size % 2 != 0);
        if (pad_byte) dwin_packet_length += 1;

        uint8_t* tx_buf = static_cast<uint8_t*>(malloc(4 + dwin_packet_length));
        if (tx_buf == nullptr) {
            return false;
        }

        tx_buf[0] = 0x5A; tx_buf[1] = 0xA5;
        tx_buf[2] = static_cast<uint8_t>((dwin_packet_length >> 8) & 0xFF);
        tx_buf[3] = static_cast<uint8_t>(dwin_packet_length & 0xFF);
        tx_buf[4] = 0x82; 
        tx_buf[5] = static_cast<uint8_t>((current_vp >> 8) & 0xFF);
        tx_buf[6] = static_cast<uint8_t>(current_vp & 0xFF);

        std::memcpy(&tx_buf[7], &jpeg_data[bytes_sent], current_chunk_size);
        if (pad_byte) tx_buf[7 + current_chunk_size] = 0x00;

        printf("[CHECKPOINT] Sending Chunk #%u (Address 0x%04X, Size %u): ", 
               (unsigned int)chunk_counter, 
               (unsigned int)current_vp, 
               (unsigned int)current_chunk_size);
        fflush(stdout);

        size_t total_packet_size = 4 + dwin_packet_length;
        uart_write_bytes(cfg_.uart_port, reinterpret_cast<const char*>(tx_buf), total_packet_size);
        free(tx_buf);

        // Wait up to 100ms for screen response before sending next packet
        if (!waitForDwinAck(100)) {
            ESP_LOGE("CHECKPOINT", "[FAIL] Screen dropped chunk #%u or rejected address! Aborting stream.", (unsigned int)chunk_counter);
            return false; 
        }

        bytes_sent += current_chunk_size;
        uint16_t words_sent = (current_chunk_size + (pad_byte ? 1 : 0)) / 2;
        current_vp += words_sent;
        chunk_counter++;
    }

    ESP_LOGI("CHECKPOINT", "[DATA SUCCESS] All file chunks acknowledged. Giving T5L core 50ms to settle cache memory...");
    vTaskDelay(pdMS_TO_TICKS(50));

    // --- STEP 3: ACTIVATION TRIGGER PACKET AT VP (0x8000) ---
    // Writes 0x5AA5 (Enable Function) to 0x8000
    // Writes 0x8000 (Allocates full 64KB buffer limit) to 0x8001
    uint8_t trigger_cmd[11] = {
        0x5A, 0xA5, // Frame Header
        0x07,       // Remaining Length
        0x82,       // Write command
        0x80, 0x00, // Target VP: 0x8000 (Your widget control address)
        0x5A, 0xA5, // Open function flag written to 0x8000
        0x80, 0x00  // Buffer allocation length parameter written to 0x8001 (64KB limits)
    };

    printf("[CHECKPOINT] Blasting Activation Execution to Address 0x8000: ");
    fflush(stdout);
    
    uart_write_bytes(cfg_.uart_port, reinterpret_cast<const char*>(trigger_cmd), 11);
    
    if (!waitForDwinAck(100)) {
        ESP_LOGE("CHECKPOINT", "[FAIL] Screen rejected the final activation command token flag!");
        return false;
    }

    ESP_LOGI("CHECKPOINT", "[COMPLETE] Pipeline completed cleanly. Giving screen decompressor 100ms processing window...");
    vTaskDelay(pdMS_TO_TICKS(100));
    return true;
}

// ============================================================================
// Local Embedded Resource Handling Function
// ============================================================================

/**
 * @brief Loads the compiled JPEG asset and streams it to the DWIN panel.
 */
void loadAndSendEmbeddedJpeg(Screen& carousel) {
    size_t image_size = (size_t)(image_end - image_start);

    if (image_size <= 0) {
        ESP_LOGE("CHECKPOINT", "[FATAL] Embedded program resource image has a null size allocation");
        return;
    }

    ESP_LOGI("CHECKPOINT", "Flash-mapped embedded JPEG asset detected successfully.");
    ESP_LOGI("CHECKPOINT", "Size: %u bytes. Initializing communication pipeline...", (unsigned int)image_size);

    carousel.sendJpegImageOnTheFly(image_start, image_size);
}