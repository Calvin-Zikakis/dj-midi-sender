#pragma once

// SSD1306 OLED status screen via U8g2. One immutable snapshot is rendered
// per frame so the producer (UI task) owns all the state and the display
// layer stays pure. Software I2C on explicit pins — see ui_display.cpp for
// why hardware I2C is unusable here.

#include <cstdint>

namespace firmware {

struct UiSnapshot {
    bool        link_up       = false;
    bool        playing       = false;
    bool        clock_running = false;
    float       bpm           = 0.0f;
    float       pitch_pct     = 0.0f;
    uint8_t     master_dev    = 0;     // device # currently driving the clock
    uint8_t     selected_src  = 0;     // encoder selection: 0 = auto, 1..4
    uint8_t     beat_in_bar   = 0;     // 1..4, 0 = unknown
    float       offset_ms     = 0.0f;  // total lead-time compensation
    int32_t     phase_err_us  = 0;
    float       tapped_bpm    = 0.0f;
    const char* usb_state     = "--";  // short tag: rdy/wait/nomid/off/diag
};

// Initialize the panel (call from the task that will render). Shows a splash.
void ui_display_begin();

// Draw one frame.
void ui_display_render(const UiSnapshot& s);

}  // namespace firmware
