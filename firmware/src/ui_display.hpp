#pragma once

// SSD1306 OLED status screen via U8g2. One immutable snapshot is rendered
// per frame so the producer (UI task) owns all the state and the display
// layer stays pure. Hardware I2C on explicit pins — see ui_display.cpp for
// why software I2C is unusable here.

#include <cstdint>

namespace firmware {

// Front-panel UI mode. Drives which screen the OLED renders and how input is
// routed.
enum class UiMode : uint8_t {
    kNormal = 0,    // live status screen
    kSourceSelect,  // choosing the clock source (follower/player/sync/off)
    kMenu,          // settings list
    kMenuEdit,      // editing the highlighted setting's value
};

// Settings-menu items.
enum MenuItem : uint8_t {
    kMenuItemActAsPlayer = 0, // join the link as a player (unlocks sync master)
    kMenuItemBpmStep,         // BPM per detent when the BOX owns the tempo
    kMenuItemOffsetStep,      // offset-button step per press
    kMenuItemKeepPlaying,     // hold the clock when the decks stop
    kMenuItemCount,
};

// BPM step: encoder detent size when the box owns the tempo (sync master/off).
inline constexpr float   kBpmStepValues[] = {0.1f, 0.25f, 0.5f, 1.0f};
inline constexpr uint8_t kBpmStepCount    = 4;
inline constexpr uint8_t kBpmStepDefault  = 0;  // 0.1 BPM

// Offset-button step options (ms per nudge press).
inline constexpr float   kOffsetStepValues[] = {0.1f, 0.5f, 1.0f};
inline constexpr uint8_t kOffsetStepCount    = 3;
inline constexpr uint8_t kOffsetStepDefault  = 2;  // index of 1.0 ms

// Clock sources — "what drives the clock":
//   0      = follower master : follow whichever deck holds the DJ-Link master role
//   1..4   = player 1..4     : pin to that deck
//   5      = sync master     : the BOX claims the master role; decks sync to it
//   6      = off             : standalone manual tempo, link ignored
inline constexpr uint8_t kSourceCount  = 7;
inline constexpr uint8_t kSourceMaster = 5;
inline constexpr uint8_t kSourceOff    = 6;

// `sync master` is hidden unless "Act as player" is on, so a stray spin can
// never hijack the master role mid-set. Claiming the role also means claiming
// a player slot on the link, which is exactly what that setting authorises.
inline constexpr uint8_t source_count(bool act_as_player) {
    return act_as_player ? kSourceCount : kSourceCount - 1;
}
// Map a cursor index to a source id, skipping `sync master` when it is hidden.
inline constexpr uint8_t source_at(uint8_t index, bool act_as_player) {
    return (!act_as_player && index >= kSourceMaster)
               ? static_cast<uint8_t>(index + 1)   // jump over sync master
               : index;
}

// Short clock-source label for the status line (<= 4 chars, fits beside `src`).
const char* ui_source_label(uint8_t src);
// Full label for the source-select list, where there is room to spell it out.
const char* ui_source_label_long(uint8_t src);

struct UiSnapshot {
    bool        link_up       = false;
    bool        playing       = false;
    bool        clock_running = false;
    bool        ignore_master = false;  // "Off" source — standalone tempo
    float       bpm           = 0.0f;
    float       pitch_pct     = 0.0f;
    uint8_t     master_dev    = 0;     // device # currently driving the clock
    uint8_t     selected_src  = 0;     // see the source table above
    uint8_t     beat_in_bar   = 0;     // 1..4, 0 = unknown
    float       offset_ms     = 0.0f;  // total lead-time compensation
    bool        resync_flash  = false;  // brief "RSYNC" confirmation after a tap
    bool        is_master     = false;  // box holds the DJ-Link tempo-master role
    bool        master_wanted = false;  // master requested, handshake in flight
    const char* usb_state     = "--";  // short tag: rdy/wait/nomid/off/diag
    UiMode      ui_mode       = UiMode::kNormal;
    uint8_t     proposed_src  = 0;     // source-select cursor (kSourceSelect only)
    uint8_t     menu_index    = 0;     // highlighted item (kMenu / kMenuEdit)
    int32_t     menu_edit     = 0;     // working value while editing
    bool        act_as_player = false;  // link presence: unlocks sync master
    uint8_t     bpm_step_idx  = 0;     // index into kBpmStepValues
    uint8_t     offset_step_idx = 0;   // index into kOffsetStepValues
    bool        keep_playing  = false;  // hold the clock when the decks stop
    bool        holding       = false;  // clock running with no deck driving it
};

// Initialize the panel (call from the task that will render). Shows a splash.
void ui_display_begin();

// Draw one frame.
void ui_display_render(const UiSnapshot& s);

}  // namespace firmware
