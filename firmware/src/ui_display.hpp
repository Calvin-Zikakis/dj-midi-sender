#pragma once

// SSD1306 OLED status screen via U8g2. One immutable snapshot is rendered
// per frame so the producer (UI task) owns all the state and the display
// layer stays pure. Software I2C on explicit pins — see ui_display.cpp for
// why hardware I2C is unusable here.

#include <cstdint>

namespace firmware {

// Front-panel UI mode. Drives which screen the OLED renders and how input is
// routed.
enum class UiMode : uint8_t {
    kNormal = 0,    // live status screen
    kSourceSelect,  // choosing the clock source (Master / P1..P4)
    kMenu,          // settings list
    kMenuEdit,      // editing the highlighted setting's value
};

// Settings-menu items.
enum MenuItem : uint8_t {
    kMenuItemMode = 0,    // Sync / Free
    kMenuItemBpmStep,     // player-mode manual-BPM step (hold-tap + spin)
    kMenuItemFineStep,    // Off-mode tap-tempo fine-tune step (spin)
    kMenuItemOffsetStep,  // offset-button step per press
    kMenuItemCount,
};

// Player-mode manual-BPM step options (BPM per encoder detent).
inline constexpr float   kBpmStepValues[] = {0.1f, 0.5f, 1.0f, 5.0f};
inline constexpr uint8_t kBpmStepCount    = 4;
inline constexpr uint8_t kBpmStepDefault  = 2;  // index of 1.0 BPM

// Off-mode tap-tempo fine-tune step options.
inline constexpr float   kFineStepValues[] = {0.1f, 0.25f, 0.5f, 1.0f};
inline constexpr uint8_t kFineStepCount    = 4;
inline constexpr uint8_t kFineStepDefault  = 0;  // 0.1 BPM

// Offset-button step options (ms per nudge press).
inline constexpr float   kOffsetStepValues[] = {0.1f, 0.5f, 1.0f};
inline constexpr uint8_t kOffsetStepCount    = 3;
inline constexpr uint8_t kOffsetStepDefault  = 2;  // index of 1.0 ms

// Clock sources: 0 = Master (auto-track), 1..4 = deck Pn, 5 = Off (standalone).
inline constexpr uint8_t kSourceCount = 6;
inline constexpr uint8_t kSourceOff   = 5;

// Clock-source label (see above).
const char* ui_source_label(uint8_t src);

struct UiSnapshot {
    bool        link_up       = false;
    bool        playing       = false;
    bool        clock_running = false;
    bool        free_run      = false;
    bool        manual_bpm    = false;  // manual tempo latched (Free mode)
    bool        ignore_master = false;  // "Off" source — standalone tempo
    float       bpm           = 0.0f;
    float       pitch_pct     = 0.0f;
    uint8_t     master_dev    = 0;     // device # currently driving the clock
    uint8_t     selected_src  = 0;     // encoder selection: 0 = auto, 1..4
    uint8_t     beat_in_bar   = 0;     // 1..4, 0 = unknown
    float       offset_ms     = 0.0f;  // total lead-time compensation
    int32_t     phase_err_us  = 0;
    float       tapped_bpm    = 0.0f;
    bool        resync_flash  = false;  // brief "RSYNC" confirmation after a tap
    const char* usb_state     = "--";  // short tag: rdy/wait/nomid/off/diag
    UiMode      ui_mode       = UiMode::kNormal;
    uint8_t     proposed_src  = 0;     // source-select cursor (kSourceSelect only)
    uint8_t     menu_index    = 0;     // highlighted item (kMenu / kMenuEdit)
    int32_t     menu_edit     = 0;     // working value while editing
    uint8_t     bpm_step_idx  = 0;     // index into kBpmStepValues
    uint8_t     fine_step_idx = 0;     // index into kFineStepValues
    uint8_t     offset_step_idx = 0;   // index into kOffsetStepValues
};

// Initialize the panel (call from the task that will render). Shows a splash.
void ui_display_begin();

// Draw one frame.
void ui_display_render(const UiSnapshot& s);

}  // namespace firmware
