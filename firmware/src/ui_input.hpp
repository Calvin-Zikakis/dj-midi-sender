#pragma once

// Front-panel input: one EC11 rotary encoder (A/B + push switch) and three
// momentary buttons (nudge −, nudge +, tap). All active-low with internal
// pull-ups. A 1 ms polling task decodes the encoder with a full-step state
// table (robust against contact bounce) and debounces the buttons; consumers
// drain the accumulated steps / press events lock-free.

#include <cstdint>

namespace firmware {

// Bit flags returned by ui_input_take_button_presses(). The nudge buttons are
// NOT here — they auto-repeat, so they're drained as signed steps instead.
enum ButtonBit : uint32_t {
    kBtnEncSw  = 1u << 0,  // encoder push
    kBtnTap    = 1u << 3,  // tap tempo
};

// Spawn the 1 ms input polling task. Call once from setup().
void ui_input_begin();

// Net encoder detents since the last call (signed; +cw / −ccw). Cleared on read.
int32_t ui_input_take_encoder_steps();

// Net nudge steps since the last call (signed; +right / −left), including
// hold-to-repeat with acceleration: one step per press, then after a hold
// delay it auto-repeats and speeds up. Cleared on read.
int32_t ui_input_take_nudge_steps();

// Count of free-run toggle events (both nudge buttons held ~1 s) since the
// last call. Normally 0 or 1. Cleared on read.
uint32_t ui_input_take_menu_holds();

// True while the tap button is currently held down (for the hold-tap + spin
// BPM modifier). Level, not an edge — not cleared on read.
bool ui_input_tap_held();

// millis() at which the tap button last settled, measured in the 1 ms input
// task. Tap-tempo must use this rather than the UI frame clock, which would
// quantise every interval by up to a frame (40 ms).
uint32_t ui_input_tap_press_ms();

// Bitmask (ButtonBit) of single-press buttons (encoder push, tap) that
// registered a fresh press since the last call. Cleared on read.
uint32_t ui_input_take_button_presses();

}  // namespace firmware
