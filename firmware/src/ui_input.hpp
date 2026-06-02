#pragma once

// Front-panel input: one EC11 rotary encoder (A/B + push switch) and three
// momentary buttons (nudge −, nudge +, tap). All active-low with internal
// pull-ups. A 1 ms polling task decodes the encoder with a full-step state
// table (robust against contact bounce) and debounces the buttons; consumers
// drain the accumulated steps / press events lock-free.

#include <cstdint>

namespace firmware {

// Bit flags returned by ui_input_take_button_presses().
enum ButtonBit : uint32_t {
    kBtnEncSw  = 1u << 0,  // encoder push
    kBtnNudgeL = 1u << 1,  // nudge left  (clock earlier)
    kBtnNudgeR = 1u << 2,  // nudge right (clock later)
    kBtnTap    = 1u << 3,  // tap tempo
};

// Spawn the 1 ms input polling task. Call once from setup().
void ui_input_begin();

// Net encoder detents since the last call (signed; +cw / −ccw). Cleared on read.
int32_t ui_input_take_encoder_steps();

// Bitmask (ButtonBit) of buttons that registered a fresh press since the last
// call. Cleared on read.
uint32_t ui_input_take_button_presses();

}  // namespace firmware
