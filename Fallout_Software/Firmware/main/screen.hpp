#pragma once

#include <cstdint>
#include <cstddef>
#include "driver/uart.h"

/**
 * @brief Manages the DWIN carousel display and workspace transitions.
 * @details The class translates swipe state into hardware writes for the UI panel.
 */
class Screen {
public:
    /**
     * @brief Hardware settings used to talk to the display controller.
     */
    struct Config {
        uart_port_t uart_port;
        uint32_t step_duration_ms;
    };

    explicit Screen(const Config& config);
    ~Screen() = default;

    void swipeLeft();
    void swipeRight();
    void scrollToWorkspace(uint8_t target_idx);
    void update();
    bool sendJpegImageOnTheFly(const uint8_t* jpeg_data, size_t jpeg_size);

private:
    enum class State { IDLE, MOVING };
    Config cfg_;
    State state_;
    uint8_t current_focus_idx_;
    uint8_t target_step_idx_;
    uint8_t ultimate_target_idx_;
    int8_t stride_balance_delta_;    
    uint8_t current_global_pos_;     
    uint8_t queued_workspace_;       
    bool is_floor_move_;             
    float g_local_slide_x;
    bool shifting_right_;            
    float starting_x_;
    float slot_spacing_;
    uint16_t uniform_slots_x_[10];
    
    static constexpr uint16_t TEXT_BOX_WIDTH = 300;  
    static constexpr float CUSHION_PADDING = 80.0f; 
    static constexpr float ACTIVE_TEXT_CENTERING_DELTA = 0.5f; 

    uint16_t last_sent_frames_[10];
    int64_t transition_start_time_us_;
    uint32_t last_log_time_ms_;
    uint8_t update_ticker_;          
    int text_values_[10];

    static constexpr uint16_t FORCED_Y_BASELINE = 40;    
    static constexpr int16_t REST_TEXT_Y_OFFSET = 58;  
    static constexpr int16_t ACTIVE_ICON_Y_OFFSET = 20; 
    static constexpr int16_t ACTIVE_TEXT_Y_OFFSET = -10; 

    void sendDwinWriteSingle(uint16_t address, uint16_t value);
    void sendDwinString(uint16_t address, const char* str);
    void moveDwinElementsPacked(uint16_t base_sp_address, uint16_t dynamic_x, uint16_t y, bool is_text);
    void updateHardwarePositions(float ease_progress);
    bool waitForDwinAck(uint32_t timeout_ms);
};

// Expose the image loader function so main can trigger the boot JPEG
void loadAndSendEmbeddedJpeg(Screen& carousel);