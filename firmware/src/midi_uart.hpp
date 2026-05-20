#pragma once

// Firmware IMidiOut. Will eventually wrap HardwareSerial(1) at 31250 baud
// on GPIO17 → 220 Ω → DIN pin 5. For Phase 2 bring-up we use a counter
// stub so we can verify the clock is emitting bytes without needing the
// DIN jack wired yet.

#include "clock.hpp"

#include <atomic>
#include <cstdint>

namespace firmware {

class MidiUartStub : public prolink::IMidiOut {
public:
    void send_byte(uint8_t b) override;

    uint64_t bytes_sent()       const { return bytes_.load();        }
    uint64_t clock_ticks_sent() const { return clock_ticks_.load();  }
    uint64_t start_messages()   const { return start_messages_.load(); }
    uint64_t stop_messages()    const { return stop_messages_.load(); }

private:
    std::atomic<uint64_t> bytes_{0};
    std::atomic<uint64_t> clock_ticks_{0};
    std::atomic<uint64_t> start_messages_{0};
    std::atomic<uint64_t> stop_messages_{0};
};

}  // namespace firmware
