#pragma once

// Fan one MIDI byte stream out to two sinks. There is exactly one PLL /
// tick generator in the box; the USB host output and the DIN jack are just
// two destinations its bytes are written to (docs/phases.md, Phase 3).
// Each sink is independently non-blocking and does its own drop-counting,
// so a stalled or absent device on one output can't disturb the other.

#include "clock.hpp"

namespace firmware {

class MidiFanOut : public prolink::IMidiOut {
public:
    MidiFanOut(prolink::IMidiOut& a, prolink::IMidiOut& b) : a_(a), b_(b) {}

    void send_byte(uint8_t byte) override {
        a_.send_byte(byte);
        b_.send_byte(byte);
    }

private:
    prolink::IMidiOut& a_;
    prolink::IMidiOut& b_;
};

}  // namespace firmware
