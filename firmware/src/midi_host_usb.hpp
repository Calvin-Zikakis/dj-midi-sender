#pragma once

// USB MIDI host on the ESP32-S3's native USB-OTG peripheral. The box acts
// as the USB host; class-compliant USB MIDI devices (OP-XY, Digitakt,
// etc.) plug in as peripherals on the USB-C jack.
//
// IMidiOut::send_byte is non-blocking — bytes are pushed into a FreeRTOS
// queue and a dedicated sender task drains the queue, packs each byte
// into a 4-byte USB MIDI Event Packet, and submits a bulk-OUT transfer
// to the connected device. If no device is connected yet (or has been
// unplugged), bytes are silently dropped — losing a few clock ticks at
// startup is preferable to blocking the Clock's tick callback.
//
// Counters are exposed so the main loop's [status] line stays useful
// for the brief window before we switch the USB peripheral into host
// mode (which kills the serial console).

#include "clock.hpp"

#include <atomic>
#include <cstdint>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
}

namespace firmware {

class MidiHostUsb : public prolink::IMidiOut {
public:
    // Coarse host-side state, surfaced so the firmware can encode it onto
    // the WS2812 LED — our only diagnostic channel once host mode kills the
    // serial console. Distinguishes "nothing plugged in" from "device
    // plugged in but it has no MIDI interface" from "ready".
    enum class HostState : uint8_t {
        kUninstalled = 0,  // begin() not called or failed to install driver
        kWaiting,          // driver up, waiting for a device to be plugged in
        kDeviceNoMidi,     // a device enumerated but exposed no MIDIStreaming interface
        kReady,            // device enumerated, MIDI interface claimed
    };

    MidiHostUsb() = default;
    ~MidiHostUsb() override;
    MidiHostUsb(const MidiHostUsb&) = delete;
    MidiHostUsb& operator=(const MidiHostUsb&) = delete;

    // Install the USB host driver and spawn the host/client/sender tasks.
    // Must be called once from setup() (or any task context) before any
    // call to send_byte(). Returns false if the host driver can't be
    // installed — typically because the OTG peripheral is already claimed
    // (e.g. arduino-esp32's CDC). Logs the reason to stdout.
    bool begin();

    // IMidiOut — called from the Clock's tick callback (esp_timer task
    // context). Non-blocking. Returns immediately on queue full instead
    // of dropping the clock cadence.
    void send_byte(uint8_t b) override;

    HostState host_state()         const { return state_.load(); }
    bool     is_device_connected() const { return device_connected_.load(); }
    uint64_t bytes_queued()        const { return bytes_queued_.load(); }
    uint64_t bytes_sent()          const { return bytes_sent_.load(); }
    uint64_t bytes_dropped()       const { return bytes_dropped_.load(); }
    uint64_t clock_ticks_sent()    const { return clock_ticks_.load(); }
    uint64_t start_messages()      const { return start_messages_.load(); }
    uint64_t stop_messages()       const { return stop_messages_.load(); }

private:
    // The three task entry points. All FreeRTOS-task signatures, but we
    // route through these statics to get back to the instance.
    static void host_lib_task(void* arg);
    static void client_task(void* arg);
    static void sender_task(void* arg);

    void run_host_lib_loop();
    void run_client_loop();
    void run_sender_loop();

    // Called from the client event callback when a device is connected /
    // disconnected. Both run on the client task context.
    void on_device_connect(uint8_t dev_addr);
    void on_device_disconnect();

    // Walks the active configuration descriptor of `dev_addr`, finds an
    // Audio/MIDIStreaming interface and its first bulk-OUT endpoint, and
    // claims the interface. Returns true on success and stores the
    // endpoint info into out_ep_addr_ + out_ep_mps_.
    bool claim_midi_interface();

    // Submits one already-prepared transfer; called from sender_task.
    bool submit_one(uint8_t midi_byte);

    // FreeRTOS handles for the three tasks. Stored so the destructor can
    // tear them down cleanly — useful for graceful failure paths.
    TaskHandle_t host_task_  = nullptr;
    TaskHandle_t client_task_h_ = nullptr;
    TaskHandle_t sender_task_h_ = nullptr;

    // Opaque to consumers; implementation details live in the .cpp.
    void* client_hdl_ = nullptr;   // usb_host_client_handle_t
    void* device_hdl_ = nullptr;   // usb_device_handle_t
    void* transfer_   = nullptr;   // usb_transfer_t*

    uint8_t  interface_num_ = 0xFF;
    uint8_t  out_ep_addr_   = 0x00;
    uint16_t out_ep_mps_    = 0;

    QueueHandle_t byte_queue_ = nullptr;

    std::atomic<bool>     device_connected_{false};
    std::atomic<bool>     installed_{false};
    std::atomic<HostState> state_{HostState::kUninstalled};

    std::atomic<uint64_t> bytes_queued_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> bytes_dropped_{0};
    std::atomic<uint64_t> clock_ticks_{0};
    std::atomic<uint64_t> start_messages_{0};
    std::atomic<uint64_t> stop_messages_{0};
};

}  // namespace firmware
