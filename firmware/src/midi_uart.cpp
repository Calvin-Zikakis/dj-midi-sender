#include "midi_uart.hpp"

#include "types.hpp"

#include <cstdio>

#include "driver/uart.h"

namespace firmware {

namespace {
constexpr uart_port_t kUartNum      = UART_NUM_1;
constexpr int         kMidiBaud     = 31250;   // divides 80 MHz APB exactly
// The driver insists on an RX ring buffer > the 128-byte hardware FIFO even
// though we never receive; TX buffer 0 = uart_tx_chars writes the hardware
// FIFO directly (no ring buffer, no blocking).
constexpr int         kRxBufDummy   = 256;
}  // namespace

bool MidiUart::begin(int tx_pin) {
    if (ready_.load()) return true;

    uart_config_t cfg = {};
    cfg.baud_rate  = kMidiBaud;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_APB;

    esp_err_t err = uart_driver_install(kUartNum, kRxBufDummy, 0, 0, nullptr, 0);
    if (err != ESP_OK) {
        printf("[midi-din] uart_driver_install failed: 0x%x\n", err);
        return false;
    }
    err = uart_param_config(kUartNum, &cfg);
    if (err != ESP_OK) {
        printf("[midi-din] uart_param_config failed: 0x%x\n", err);
        uart_driver_delete(kUartNum);
        return false;
    }
    err = uart_set_pin(kUartNum, tx_pin, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        printf("[midi-din] uart_set_pin(tx=%d) failed: 0x%x\n", tx_pin, err);
        uart_driver_delete(kUartNum);
        return false;
    }

    ready_.store(true);
    // TX now idles high (UART mark) = MIDI "no current" = logical 1. Good.
    printf("[midi-din] UART%d TX on IO%d @ %d baud — DIN out ready\n",
           static_cast<int>(kUartNum), tx_pin, kMidiBaud);
    return true;
}

void MidiUart::send_byte(uint8_t b) {
    if (!ready_.load()) return;
    const char c = static_cast<char>(b);
    if (uart_tx_chars(kUartNum, &c, 1) == 1) {
        bytes_sent_.fetch_add(1, std::memory_order_relaxed);
    } else {
        // Hardware FIFO full — can't happen at MIDI-clock rates unless the
        // line is wedged; drop rather than block the tick callback.
        bytes_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

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
