#include "midi_uart.hpp"

#include "types.hpp"

namespace firmware {

void MidiUartStub::send_byte(uint8_t b) {
    bytes_.fetch_add(1, std::memory_order_relaxed);
    switch (b) {
        case prolink::MIDI_CLOCK:    clock_ticks_.fetch_add(1, std::memory_order_relaxed); break;
        case prolink::MIDI_START:    start_messages_.fetch_add(1, std::memory_order_relaxed); break;
        case prolink::MIDI_CONTINUE: start_messages_.fetch_add(1, std::memory_order_relaxed); break;
        case prolink::MIDI_STOP:     stop_messages_.fetch_add(1, std::memory_order_relaxed); break;
        default: break;
    }
}

}  // namespace firmware
