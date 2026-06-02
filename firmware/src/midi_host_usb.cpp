#include "midi_host_usb.hpp"

#include "types.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include "usb/usb_host.h"
}

namespace firmware {

namespace {

// USB MIDI Class spec: Audio class (0x01) / MIDIStreaming subclass (0x03)
// Prefixed kUsb… because ESP-IDF's usb_types_ch9.h already defines
// USB_CLASS_AUDIO as a macro.
constexpr uint8_t kUsbClassAudio            = 0x01;
constexpr uint8_t kUsbSubclassMidiStreaming = 0x03;

// USB MIDI Event Packet — 4 bytes, fixed.
// Byte 0: high nibble = cable number (we use 0), low nibble = Code Index
// Number. CIN 0xF = single-byte system-realtime (clock, start, stop, etc).
constexpr uint8_t kUsbMidiCinSingleByte = 0x0F;

// Bytes per USB MIDI Event Packet — always 4.
constexpr size_t  kUsbMidiEventPacketSize = 4;

// FreeRTOS byte queue depth. At 50 PPS (24 PPQN × 126 BPM / 60), even a
// 64-byte burst would only be ~1.3 s of clock — and bulk transfers
// complete in well under 1 ms, so this should never fill except during
// device-disconnect windows.
constexpr UBaseType_t kByteQueueLen = 64;

// Endpoint / interface descriptor type codes.
constexpr uint8_t kUsbDescTypeEndpoint  = 0x05;
constexpr uint8_t kUsbDescTypeInterface = 0x04;

// Transfer attributes for bulk endpoints.
constexpr uint8_t kUsbEpBulkMask    = 0x03;
constexpr uint8_t kUsbEpBulkValue   = 0x02;
constexpr uint8_t kUsbEpDirOutMask  = 0x80;  // address bit 7 clear = OUT

}  // namespace

MidiHostUsb::~MidiHostUsb() {
    // Tasks are FreeRTOS-managed and will be deleted when their loops
    // exit; we don't try to join them explicitly because the destructor
    // typically runs at the end of process lifetime on firmware (never).
    if (byte_queue_) {
        vQueueDelete(byte_queue_);
        byte_queue_ = nullptr;
    }
}

bool MidiHostUsb::begin() {
    if (installed_.load()) return true;

    byte_queue_ = xQueueCreate(kByteQueueLen, sizeof(uint8_t));
    if (!byte_queue_) {
        printf("[usb-midi] failed to create byte queue\n");
        return false;
    }

    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags     = ESP_INTR_FLAG_LEVEL1;
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        printf("[usb-midi] usb_host_install failed: 0x%x\n", err);
        return false;
    }
    installed_.store(true);

    // Host library event loop — must be running for any USB activity.
    BaseType_t ok = xTaskCreatePinnedToCore(
        &MidiHostUsb::host_lib_task, "usb-host-lib", 4096, this, 6,
        &host_task_, 0);
    if (ok != pdPASS) {
        printf("[usb-midi] failed to spawn host_lib_task\n");
        return false;
    }

    // Register a client *after* the host library task is alive so the
    // client's events have somewhere to flow.
    usb_host_client_config_t client_config = {};
    client_config.is_synchronous                = false;
    client_config.max_num_event_msg             = 5;
    client_config.async.client_event_callback   =
        [](const usb_host_client_event_msg_t* event_msg, void* arg) {
            auto* self = static_cast<MidiHostUsb*>(arg);
            switch (event_msg->event) {
                case USB_HOST_CLIENT_EVENT_NEW_DEV:
                    self->on_device_connect(event_msg->new_dev.address);
                    break;
                case USB_HOST_CLIENT_EVENT_DEV_GONE:
                    self->on_device_disconnect();
                    break;
                default:
                    break;
            }
        };
    client_config.async.callback_arg = this;

    usb_host_client_handle_t hdl = nullptr;
    err = usb_host_client_register(&client_config, &hdl);
    if (err != ESP_OK) {
        printf("[usb-midi] usb_host_client_register failed: 0x%x\n", err);
        return false;
    }
    client_hdl_ = hdl;

    // Client + sender tasks.
    ok = xTaskCreatePinnedToCore(
        &MidiHostUsb::client_task, "usb-host-client", 4096, this, 5,
        &client_task_h_, 0);
    if (ok != pdPASS) {
        printf("[usb-midi] failed to spawn client_task\n");
        return false;
    }
    ok = xTaskCreatePinnedToCore(
        &MidiHostUsb::sender_task, "usb-midi-tx", 4096, this, 5,
        &sender_task_h_, 0);
    if (ok != pdPASS) {
        printf("[usb-midi] failed to spawn sender_task\n");
        return false;
    }

    state_.store(HostState::kWaiting);
    printf("[usb-midi] host driver installed; waiting for device on USB-A\n");
    return true;
}

void MidiHostUsb::send_byte(uint8_t b) {
    if (!byte_queue_) return;

    // Update counters regardless of whether a device is attached — these
    // are the same counters the old MidiUartStub exposed, useful for the
    // [status] log even when nothing is plugged in.
    switch (b) {
        case prolink::MIDI_CLOCK:    clock_ticks_.fetch_add(1, std::memory_order_relaxed); break;
        case prolink::MIDI_START:    start_messages_.fetch_add(1, std::memory_order_relaxed); break;
        case prolink::MIDI_CONTINUE: start_messages_.fetch_add(1, std::memory_order_relaxed); break;
        case prolink::MIDI_STOP:     stop_messages_.fetch_add(1, std::memory_order_relaxed); break;
        default: break;
    }

    if (!device_connected_.load()) {
        // Drop silently — we'll re-sync as soon as a device shows up.
        bytes_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Non-blocking enqueue. Dropping a clock byte is preferable to
    // back-pressuring the Clock's tick callback (which would warp the
    // tick cadence). 0 ticks timeout = "try once, then give up."
    if (xQueueSend(byte_queue_, &b, 0) == pdTRUE) {
        bytes_queued_.fetch_add(1, std::memory_order_relaxed);
    } else {
        bytes_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

// ── Task entry trampolines ─────────────────────────────────────────────

void MidiHostUsb::host_lib_task(void* arg) {
    static_cast<MidiHostUsb*>(arg)->run_host_lib_loop();
}
void MidiHostUsb::client_task(void* arg) {
    static_cast<MidiHostUsb*>(arg)->run_client_loop();
}
void MidiHostUsb::sender_task(void* arg) {
    static_cast<MidiHostUsb*>(arg)->run_sender_loop();
}

void MidiHostUsb::run_host_lib_loop() {
    for (;;) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        // USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS / _ALL_FREE happen on
        // teardown — we never tear down, so just keep looping.
    }
}

void MidiHostUsb::run_client_loop() {
    auto hdl = static_cast<usb_host_client_handle_t>(client_hdl_);
    for (;;) {
        usb_host_client_handle_events(hdl, portMAX_DELAY);
    }
}

void MidiHostUsb::run_sender_loop() {
    uint8_t b = 0;
    for (;;) {
        if (xQueueReceive(byte_queue_, &b, portMAX_DELAY) == pdTRUE) {
            if (device_connected_.load() && transfer_) {
                submit_one(b);
            } else {
                // Device disappeared between enqueue and dequeue.
                bytes_dropped_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// ── Device connect / disconnect / interface claim ──────────────────────

void MidiHostUsb::on_device_connect(uint8_t dev_addr) {
    if (device_connected_.load()) {
        // We only handle one device at a time; ignore additional hot-plugs.
        printf("[usb-midi] ignoring extra device at addr %u (one device only)\n", dev_addr);
        return;
    }

    auto client = static_cast<usb_host_client_handle_t>(client_hdl_);
    usb_device_handle_t dev = nullptr;
    esp_err_t err = usb_host_device_open(client, dev_addr, &dev);
    if (err != ESP_OK) {
        printf("[usb-midi] device_open(addr=%u) failed: 0x%x\n", dev_addr, err);
        return;
    }
    device_hdl_ = dev;

    if (!claim_midi_interface()) {
        printf("[usb-midi] device at addr=%u doesn't expose a MIDIStreaming interface; closing\n", dev_addr);
        usb_host_device_close(client, dev);
        device_hdl_ = nullptr;
        // A device enumerated but had no MIDI interface (or the claim
        // failed) — distinct from "nothing plugged in" for LED diagnostics.
        state_.store(HostState::kDeviceNoMidi);
        return;
    }

    // Allocate one bulk-OUT transfer; reused for every byte.
    usb_transfer_t* xfer = nullptr;
    err = usb_host_transfer_alloc(kUsbMidiEventPacketSize, 0, &xfer);
    if (err != ESP_OK) {
        printf("[usb-midi] transfer_alloc failed: 0x%x\n", err);
        usb_host_interface_release(client, dev, interface_num_);
        usb_host_device_close(client, dev);
        device_hdl_ = nullptr;
        state_.store(HostState::kDeviceNoMidi);
        return;
    }
    xfer->device_handle      = dev;
    xfer->bEndpointAddress   = out_ep_addr_;
    xfer->callback           = [](usb_transfer_t* t) {
        auto* self = static_cast<MidiHostUsb*>(t->context);
        if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
            self->bytes_sent_.fetch_add(1, std::memory_order_relaxed);
        } else {
            self->bytes_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    };
    xfer->context = this;
    xfer->timeout_ms = 100;
    transfer_ = xfer;

    device_connected_.store(true);
    state_.store(HostState::kReady);
    printf("[usb-midi] MIDI device attached (addr=%u, intf=%u, ep=0x%02x, mps=%u)\n",
           dev_addr, interface_num_, out_ep_addr_, out_ep_mps_);
}

void MidiHostUsb::on_device_disconnect() {
    if (!device_connected_.load()) return;

    device_connected_.store(false);
    auto client = static_cast<usb_host_client_handle_t>(client_hdl_);
    auto dev    = static_cast<usb_device_handle_t>(device_hdl_);

    if (transfer_) {
        usb_host_transfer_free(static_cast<usb_transfer_t*>(transfer_));
        transfer_ = nullptr;
    }
    if (dev) {
        usb_host_interface_release(client, dev, interface_num_);
        usb_host_device_close(client, dev);
    }
    device_hdl_     = nullptr;
    interface_num_  = 0xFF;
    out_ep_addr_    = 0x00;
    out_ep_mps_     = 0;
    state_.store(HostState::kWaiting);
    printf("[usb-midi] device detached\n");
}

bool MidiHostUsb::claim_midi_interface() {
    auto client = static_cast<usb_host_client_handle_t>(client_hdl_);
    auto dev    = static_cast<usb_device_handle_t>(device_hdl_);

    const usb_config_desc_t* config = nullptr;
    esp_err_t err = usb_host_get_active_config_descriptor(dev, &config);
    if (err != ESP_OK || !config) {
        printf("[usb-midi] get_active_config_descriptor failed: 0x%x\n", err);
        return false;
    }

    // Walk the configuration. usb_parse_next_descriptor_of_type would be
    // cleaner but isn't always exposed; we do it manually with offsets.
    const uint8_t* p   = reinterpret_cast<const uint8_t*>(config);
    const uint8_t* end = p + config->wTotalLength;
    p += config->bLength;  // skip the config descriptor itself

    uint8_t  current_intf      = 0xFF;
    uint8_t  current_intf_alt  = 0;
    bool     in_midi_intf      = false;
    bool     midi_found        = false;
    uint8_t  midi_intf_num     = 0xFF;
    uint8_t  midi_intf_alt     = 0;
    uint8_t  midi_ep_addr      = 0x00;
    uint16_t midi_ep_mps       = 0;

    while (p + 2 <= end) {
        const uint8_t  bLength         = p[0];
        const uint8_t  bDescriptorType = p[1];
        if (bLength < 2 || p + bLength > end) break;

        if (bDescriptorType == kUsbDescTypeInterface && bLength >= 9) {
            current_intf     = p[2];
            current_intf_alt = p[3];
            const uint8_t bInterfaceClass    = p[5];
            const uint8_t bInterfaceSubClass = p[6];
            in_midi_intf = (bInterfaceClass == kUsbClassAudio &&
                            bInterfaceSubClass == kUsbSubclassMidiStreaming);
            if (in_midi_intf && !midi_found) {
                midi_intf_num = current_intf;
                midi_intf_alt = current_intf_alt;
            }
        } else if (bDescriptorType == kUsbDescTypeEndpoint && bLength >= 7
                   && in_midi_intf && !midi_found) {
            const uint8_t bEndpointAddress = p[2];
            const uint8_t bmAttributes     = p[3];
            const uint16_t wMaxPacketSize  =
                static_cast<uint16_t>(p[4]) | (static_cast<uint16_t>(p[5]) << 8);
            const bool is_bulk = (bmAttributes & kUsbEpBulkMask) == kUsbEpBulkValue;
            const bool is_out  = (bEndpointAddress & kUsbEpDirOutMask) == 0;
            if (is_bulk && is_out) {
                midi_ep_addr = bEndpointAddress;
                midi_ep_mps  = wMaxPacketSize;
                midi_found   = true;
            }
        }

        p += bLength;
    }

    if (!midi_found) {
        return false;
    }

    err = usb_host_interface_claim(client, dev, midi_intf_num, midi_intf_alt);
    if (err != ESP_OK) {
        printf("[usb-midi] interface_claim(intf=%u alt=%u) failed: 0x%x\n",
               midi_intf_num, midi_intf_alt, err);
        return false;
    }

    interface_num_ = midi_intf_num;
    out_ep_addr_   = midi_ep_addr;
    out_ep_mps_    = midi_ep_mps;
    return true;
}

bool MidiHostUsb::submit_one(uint8_t midi_byte) {
    auto* xfer = static_cast<usb_transfer_t*>(transfer_);
    if (!xfer) return false;

    // USB MIDI Event Packet for a single-byte system realtime message.
    // Cable 0, CIN F (single byte). Bytes 2 and 3 are unused per the spec
    // when CIN indicates single byte.
    xfer->data_buffer[0] = kUsbMidiCinSingleByte;
    xfer->data_buffer[1] = midi_byte;
    xfer->data_buffer[2] = 0x00;
    xfer->data_buffer[3] = 0x00;
    xfer->num_bytes      = kUsbMidiEventPacketSize;

    esp_err_t err = usb_host_transfer_submit(xfer);
    if (err != ESP_OK) {
        bytes_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

}  // namespace firmware
