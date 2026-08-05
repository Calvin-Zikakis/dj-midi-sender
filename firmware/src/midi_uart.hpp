#pragma once

// Firmware IMidiOut sinks for the DIN-5 MIDI jack (plus the diag stub).
//
// MidiUart — the real DIN output: IDF UART1, TX-only, 31250 baud 8N1 on
// MIDI_DIN_TX_PIN (IO17 → 10 Ω → DIN pin 5; 3V3 → 47 Ω → DIN pin 4;
// GND → DIN pin 2). Uses the IDF uart driver directly rather than Arduino
// HardwareSerial: this build falls back to the generic `esp32` Arduino
// variant whose RX1/TX1 defaults (IO9/IO10) collide with the W5500's
// RST/INT — the same wrong-default-pins trap that bit the I²C bus.
//
// send_byte() pushes straight into the 128-byte hardware TX FIFO and never
// blocks: at 24 PPQN a byte drains in ~320 µs and ticks are ~20 ms apart,
// so the FIFO is empty by the next tick. If it ever isn't, the byte is
// dropped and counted instead of stalling the Clock's tick callback —
// the same policy as MidiHostUsb's queue.
//
// MidiUartStub — counter-only stand-in the diag build uses in place of the
// USB host (so usb_host_install() is skipped and USB-C serial stays alive).

#include "clock.hpp"

#include <atomic>
#include <cstdint>

namespace firmware {

class MidiUart : public prolink::IMidiOut {
public:
    // Install UART1 on `tx_pin` (TX-only; RX stays unrouted). Call once
    // from setup() before the Clock can tick. Returns false — and leaves
    // the sink inert (send_byte becomes a no-op) — if the driver fails.
    bool begin(int tx_pin);

    // IMidiOut — called from the Clock's tick callback (esp_timer task)
    // and start/stop (bridge task). uart_tx_chars serializes concurrent
    // writers internally; single bytes make contention negligible.
    void send_byte(uint8_t b) override;

    bool     is_ready()      const { return ready_.load(); }
    uint64_t bytes_sent()    const { return bytes_sent_.load(); }
    uint64_t bytes_dropped() const { return bytes_dropped_.load(); }

private:
    std::atomic<bool>     ready_{false};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> bytes_dropped_{0};
};

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
