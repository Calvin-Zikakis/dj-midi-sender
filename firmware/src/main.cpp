// Phase 2 firmware — full Bridge integration on the Waveshare ESP32-S3-ETH.
//
// Same lib/prolink/ core that drives the desktop bridge, wrapped in firmware
// I/O implementations:
//   - UdpW5500  : IUdpSocket  ← lwIP BSD sockets over the onboard W5500
//   - TimerEsp  : ITimer      ← esp_timer hardware-timer one-shot loop
//   - MidiUart… : IMidiOut    ← counter stub for now (HardwareSerial wiring next)
//
// Ethernet bring-up is unchanged from the smoke test: W5500 over SPI, static
// 169.254.42.42/16. Once link is up, a single FreeRTOS task constructs the
// Bridge and calls run() — the blocking poll loop inside lib/prolink/.

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bridge.hpp"
#include "clock.hpp"
#include "packets.hpp"
#include "types.hpp"

#include "midi_host_usb.hpp"
#include "midi_uart.hpp"
#include "timer_esp.hpp"
#include "udp_w5500.hpp"
#include "ui_display.hpp"
#include "ui_input.hpp"

#ifdef DIAG_SERIAL_STUB
#include <Wire.h>   // HW-I2C bus scan, OLED bring-up diagnostics only
#endif

namespace {

// ── Network configuration ──────────────────────────────────────────────
// Static link-local IP (Pro DJ Link uses RFC 3927 auto-IP, no DHCP).
// Stored host-order, the same convention IUdpSocket::send expects.
constexpr uint32_t kLocalIpHost     = 0xA9FE2A2Au;  // 169.254.42.42
constexpr uint32_t kBroadcastIpHost = 0xA9FEFFFFu;  // 169.254.255.255
constexpr const char* kLocalIpStr   = "169.254.42.42";

// Locally-administered MAC (bit 1 of first byte set, OUI bit cleared).
uint8_t g_mac[6] = {0x02, 0x00, 0x00, 0xDB, 0xC3, 0x42};

// ── W5500 SPI ──────────────────────────────────────────────────────────
constexpr int kSpiClockHz = 20 * 1000 * 1000;

esp_netif_t*    g_eth_netif  = nullptr;
esp_eth_handle_t g_eth_handle = nullptr;
volatile bool   g_link_up    = false;
volatile bool   g_got_ip     = false;

void on_eth_event(void*, esp_event_base_t, int32_t event_id, void*) {
    switch (event_id) {
        case ETHERNET_EVENT_START:        printf("[eth] driver started\n"); break;
        case ETHERNET_EVENT_STOP:         printf("[eth] driver stopped\n"); break;
        case ETHERNET_EVENT_CONNECTED:    g_link_up = true;  printf("[eth] link up\n"); break;
        case ETHERNET_EVENT_DISCONNECTED: g_link_up = false; printf("[eth] link down\n"); break;
        default: break;
    }
}

void on_got_ip(void*, esp_event_base_t, int32_t, void* event_data) {
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    const auto& info = event->ip_info;
    g_got_ip = true;
    printf("[eth] got IP: " IPSTR " / mask " IPSTR " / gw " IPSTR "\n",
           IP2STR(&info.ip), IP2STR(&info.netmask), IP2STR(&info.gw));
}

void init_ethernet() {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("[eth] esp_netif_init failed: 0x%x\n", err);
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("[eth] event loop create failed: 0x%x\n", err);
        return;
    }

    spi_bus_config_t buscfg = {};
    buscfg.miso_io_num = ETH_W5500_MISO;
    buscfg.mosi_io_num = ETH_W5500_MOSI;
    buscfg.sclk_io_num = ETH_W5500_SCK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // The W5500 driver hooks the INT pin via the shared GPIO ISR service;
    // it doesn't install the service itself.
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        printf("[eth] gpio_install_isr_service failed: 0x%x\n", isr_err);
    }

    spi_device_interface_config_t devcfg = {};
    devcfg.command_bits = 16;
    devcfg.address_bits = 8;
    devcfg.mode = 0;
    devcfg.clock_speed_hz = kSpiClockHz;
    devcfg.spics_io_num = ETH_W5500_CS;
    devcfg.queue_size = 20;

    // ESP-IDF 4.4 takes a pre-allocated SPI device handle; later IDFs
    // moved to (host, &devcfg).
    spi_device_handle_t spi_handle = nullptr;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle));

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(spi_handle);
    w5500_config.int_gpio_num = ETH_W5500_INT;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = ETH_W5500_RST;

    esp_eth_mac_t* mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t* phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &g_eth_handle));
    ESP_ERROR_CHECK(esp_eth_ioctl(g_eth_handle, ETH_CMD_S_MAC_ADDR, g_mac));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    g_eth_netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(g_eth_netif, esp_eth_new_netif_glue(g_eth_handle)));

    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(g_eth_netif));
    esp_netif_ip_info_t ip_info = {};
    IP4_ADDR(&ip_info.ip,      169, 254, 42, 42);
    IP4_ADDR(&ip_info.netmask, 255, 255, 0, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(g_eth_netif, &ip_info));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_eth_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip, nullptr));

    ESP_ERROR_CHECK(esp_eth_start(g_eth_handle));

    printf("[eth] W5500 init complete — MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    printf("[eth] static IP %s/16, broadcast 169.254.255.255\n", kLocalIpStr);
}

// ── Onboard WS2812B status LED ─────────────────────────────────────────
// One pixel, GRB ordering, 800 kHz protocol. Acts as a downbeat indicator:
// red on beat 1 of every bar, dim white on beats 2–4. With the USB console
// going away once we switch to host mode, this is our only at-a-glance
// "the box is locked to a master and clocking" signal.
//
// Schematic check (2026-06-01): the WS2812 DIN routes through two alternate
// 0 Ω jumpers — R13 (0R/NC, default) → GP4, R15 (NC/0R) → GP21. Boards may
// ship stuffed either way, so we drive BOTH pins with the same color; the
// pin that isn't the LED just toggles a free, unused GPIO. (Pull-up R12 10K
// to 3V3 sits on the data line.)
Adafruit_NeoPixel g_pixel(1, BOARD_RGB_LED_PIN, NEO_GRB + NEO_KHZ800);
#ifdef BOARD_RGB_LED_PIN_ALT
Adafruit_NeoPixel g_pixel_alt(1, BOARD_RGB_LED_PIN_ALT, NEO_GRB + NEO_KHZ800);
#endif

void led_begin() {
    g_pixel.begin();
#ifdef BOARD_RGB_LED_PIN_ALT
    g_pixel_alt.begin();
#endif
}

void led_set(uint32_t color) {
    g_pixel.setPixelColor(0, color);
    g_pixel.show();
#ifdef BOARD_RGB_LED_PIN_ALT
    g_pixel_alt.setPixelColor(0, color);
    g_pixel_alt.show();
#endif
}

#ifndef DIAG_SERIAL_STUB
// USB host state → LED color. This is our only diagnostic channel in host
// mode (serial is dead), so the colors are deliberately distinct:
//   magenta = driver failed to install
//   dim blue = installed, nothing enumerated (check VBUS / D± wiring)
//   red      = a device enumerated but exposed no MIDI interface
//   (green   = ready — handled by the beat callback, not here)
uint32_t led_color_for_host_state(firmware::MidiHostUsb::HostState st) {
    using HS = firmware::MidiHostUsb::HostState;
    switch (st) {
        case HS::kUninstalled:  return g_pixel.Color(80, 0, 80);
        case HS::kWaiting:      return g_pixel.Color(0, 0, 60);
        case HS::kDeviceNoMidi: return g_pixel.Color(120, 0, 0);
        case HS::kReady:        return g_pixel.Color(0, 40, 0);
    }
    return 0;
}
#endif

// ── Bridge task ────────────────────────────────────────────────────────
// Holds the I/O implementations and the Clock, builds a BridgeConfig that
// mirrors what desktop/main.cpp builds, and calls Bridge::run(). The run
// loop polls both UDP sockets with 5 ms timeouts, so this task is
// always-on but not pinned at 100% CPU.
//
// MidiHostUsb installs ESP-IDF's USB host driver in setup() and acts as a
// USB MIDI host. The OP-XY (or any class-compliant USB MIDI device) plugs
// into the USB-A jack as a peripheral. Note: this takes over the USB-OTG
// peripheral and kills the USB-Serial-JTAG console; logs go nowhere until
// a UART breakout is wired to GPIO43/44.
//
// DIAG_SERIAL_STUB build: swap the USB host for a counter-only stub and
// (in setup) skip begin(), so the USB-OTG peripheral is never claimed and
// the USB-Serial-JTAG console over USB-C stays alive. This isolates the
// network→master→clock half of the pipeline for serial debugging without
// the USB host in the way.
#ifdef DIAG_SERIAL_STUB
firmware::MidiUartStub g_midi;
#else
firmware::MidiHostUsb g_midi;
#endif

// ── Front-panel UI plumbing ────────────────────────────────────────────
// The Bridge and Clock are constructed on bridge_task's stack and live for
// the duration of run(); we publish pointers so the UI task can read live
// state (lock-free getters) and drive the nudge/source controls. Null until
// the bridge is up, and cleared if run() ever returns.
std::atomic<prolink::Bridge*> g_bridge{nullptr};
std::atomic<prolink::Clock*>  g_clock{nullptr};

// Small bits the live objects don't expose: latest pitch %, the encoder's
// source selection (0 = auto, 1..4), and a tapped BPM (display-only for now).
std::atomic<float>   g_pitch_pct{0.0f};
std::atomic<float>   g_tapped_bpm{0.0f};
std::atomic<uint8_t> g_selected_src{0};

// Per-press nudge of the persistent clock offset (latency compensation).
constexpr float kNudgeStepMs = 1.0f;

void bridge_task(void*) {
    // Don't start the Bridge until link is up — the keepalive sender
    // would otherwise burn cycles on send() attempts that go nowhere.
    while (!g_link_up) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    firmware::UdpW5500 beat_sock;
    firmware::UdpW5500 status_sock;
    firmware::UdpW5500 keepalive_sock;

    if (!beat_sock.bind("0.0.0.0", prolink::PORT_BEAT)) {
        printf("[bridge] beat bind failed; aborting task\n");
        vTaskDelete(nullptr);
        return;
    }
    if (!status_sock.bind("0.0.0.0", prolink::PORT_STATUS)) {
        printf("[bridge] status bind failed; aborting task\n");
        vTaskDelete(nullptr);
        return;
    }
    // Bind keepalive to our local IP (not 0.0.0.0) so the OS routes the
    // broadcast through the W5500 unambiguously — matches what
    // desktop/main.cpp does.
    if (!keepalive_sock.bind(kLocalIpStr, 0)) {
        printf("[bridge] keepalive bind failed; aborting task\n");
        vTaskDelete(nullptr);
        return;
    }
    keepalive_sock.enable_broadcast();

    firmware::TimerEsp timer;
    prolink::Clock clock(g_midi, timer, /* gain_divisor */ 16);

    prolink::BridgeConfig cfg;
    cfg.device_num = 7;  // safe slot per docs/handoff.md (1–4 = decks, 5–6 = mixers)
    std::strncpy(cfg.device_name, "xdj-bridge", sizeof(cfg.device_name) - 1);
    std::memcpy(cfg.mac, g_mac, 6);
    cfg.local_ip            = kLocalIpHost;
    cfg.broadcast_ip        = kBroadcastIpHost;
    cfg.send_vcdj_announce  = true;
    cfg.verbose             = false;
    cfg.fallback_bpm        = 120.0f;
    cfg.force_master_device = 0;
    cfg.bpm_smoothing_alpha = 0.3f;

    prolink::BridgeCallbacks cb;

    cb.on_beat = [](const prolink::BeatPacket& p) {
        printf("[beat] dev=%u name='%s' bpm=%.2f pitch=%+.2f%% beat=%u/4\n",
               p.device_num, p.device_name,
               p.effective_bpm(), p.pitch_percent(), p.beat_in_bar);
        // Downbeat = red, off-beats = dim white. Stays lit until the next
        // beat overwrites it, so a missed beat is also a missed LED kick.
#ifdef DIAG_SERIAL_STUB
        // Diag: no USB host, so the LED is purely a beat indicator —
        // red downbeat, dim white off-beats.
        led_set((p.beat_in_bar == 1) ? g_pixel.Color(255, 0, 0)
                                      : g_pixel.Color(40, 40, 40));
#else
        // Production: only let beats drive the LED once USB MIDI is ready
        // (green flashes). Before that, loop() keeps the host-state color
        // on the LED so we can see where enumeration is stuck.
        if (g_midi.host_state() == firmware::MidiHostUsb::HostState::kReady) {
            led_set((p.beat_in_bar == 1) ? g_pixel.Color(0, 255, 0)
                                          : g_pixel.Color(0, 40, 0));
        }
#endif
    };

    // Status packets arrive at ~5 Hz — logging every one floods the console
    // and hides the events that matter. Track the fields that actually
    // change state and log only on transition. BPM/pitch deltas come
    // through [beat] already, so we ignore those here.
    struct StatusTrack {
        bool     have_prev = false;
        uint8_t  device_num = 0;
        bool     playing    = false;
        bool     master     = false;
        bool     synced     = false;
        bool     on_air     = false;
        uint16_t mv         = 0;
    };
    static StatusTrack prev;

    cb.on_status = [](const prolink::StatusPacket& p) {
        // Feed the OLED every packet (cheap), regardless of the log gate below.
        g_pitch_pct.store(p.pitch_percent());

        const bool changed =
            !prev.have_prev ||
            prev.device_num != p.device_num ||
            prev.playing    != p.is_playing() ||
            prev.master     != p.is_master()  ||
            prev.synced     != p.is_synced()  ||
            prev.on_air     != p.is_on_air()  ||
            prev.mv         != p.mv;

        if (changed) {
            printf("[stat] dev=%u name='%s' bpm=%.2f pitch=%+.2f%% master=%d play=%d sync=%d on_air=%d Mv=0x%04x%s\n",
                   p.device_num, p.device_name,
                   p.effective_bpm(), p.pitch_percent(),
                   p.is_master(), p.is_playing(), p.is_synced(), p.is_on_air(), p.mv,
                   prev.have_prev ? "" : "  (initial)");
            prev.have_prev   = true;
            prev.device_num  = p.device_num;
            prev.playing     = p.is_playing();
            prev.master      = p.is_master();
            prev.synced      = p.is_synced();
            prev.on_air      = p.is_on_air();
            prev.mv          = p.mv;
        }
    };

    cb.on_log = [](const char* msg) {
        printf("[bridge] %s\n", msg);
    };

    prolink::Bridge bridge(beat_sock, status_sock, keepalive_sock,
                           clock, cfg, cb);

    // Publish for the UI task now that both objects exist on this stack.
    g_clock.store(&clock, std::memory_order_release);
    g_bridge.store(&bridge, std::memory_order_release);

    printf("[bridge] starting — vCDJ device %u, broadcast %s\n",
           cfg.device_num, "169.254.255.255");
    bridge.run();   // blocks until stop()
    printf("[bridge] run() returned\n");

    // These objects are about to go out of scope — stop the UI touching them.
    g_bridge.store(nullptr, std::memory_order_release);
    g_clock.store(nullptr, std::memory_order_release);
    vTaskDelete(nullptr);
}

#ifdef DIAG_SERIAL_STUB
// Diagnostic only: scan the I2C bus on the OLED pins via hardware Wire and
// print every device that ACKs. A blank OLED with a found 0x3C/0x3D means
// the panel is wired/powered fine (software issue); no devices found means
// power / ground / SDA-SCL wiring is the problem. Wire is released after so
// U8g2's software-I2C can drive the same pins.
void i2c_scan_diag() {
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    printf("[i2c] scanning (SDA=%d SCL=%d)...\n", OLED_SDA_PIN, OLED_SCL_PIN);
    int found = 0;
    for (uint8_t addr = 1; addr < 127; ++addr) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            printf("[i2c]   device ACK at 0x%02X\n", addr);
            ++found;
        }
    }
    printf("[i2c] scan done — %d device(s) found\n", found);
    Wire.end();
}
#endif

// ── UI task ────────────────────────────────────────────────────────────
// Drains encoder/button events, drives the nudge + source-select controls
// against the live Bridge, and renders the OLED. Runs on Core 0 (away from
// the clock-critical bridge/timer on Core 1); the SW-I2C full-frame refresh
// is ~10 fps and yields to all timing-critical work.
void ui_task(void*) {
    firmware::ui_display_begin();
#ifdef DIAG_SERIAL_STUB
    printf("[ui] task running; ui_display_begin() returned\n");
#endif

    uint32_t tap_prev_ms  = 0;
    float    tap_period_ms = 0.0f;

    for (;;) {
        const int32_t  steps = firmware::ui_input_take_encoder_steps();
        const uint32_t btns  = firmware::ui_input_take_button_presses();
        prolink::Bridge* b = g_bridge.load(std::memory_order_acquire);

        // Encoder: cycle clock source auto(0) → 1 → 2 → 3 → 4 → auto.
        if (steps != 0) {
            int32_t v = (static_cast<int32_t>(g_selected_src.load()) + steps) % 5;
            if (v < 0) v += 5;
            g_selected_src.store(static_cast<uint8_t>(v));
            if (b) b->set_force_master_device(static_cast<uint8_t>(v));
#ifdef DIAG_SERIAL_STUB
            printf("[ui] encoder steps=%ld -> src=%ld\n", (long)steps, (long)v);
#endif
        }
#ifdef DIAG_SERIAL_STUB
        if (btns) printf("[ui] buttons mask=0x%lx\n", (unsigned long)btns);
#endif

        // Buttons.
        if (b && (btns & firmware::kBtnNudgeL)) b->adjust_clock_offset_ms(-kNudgeStepMs);
        if (b && (btns & firmware::kBtnNudgeR)) b->adjust_clock_offset_ms(+kNudgeStepMs);
        if (b && (btns & firmware::kBtnEncSw))  b->set_clock_offset_ms(0.0f);  // reset nudge
        if (btns & firmware::kBtnTap) {
            const uint32_t now = millis();
            const uint32_t dt  = now - tap_prev_ms;
            tap_prev_ms = now;
            if (dt > 250 && dt < 2000) {  // 30..240 BPM window
                tap_period_ms = (tap_period_ms <= 0.0f)
                    ? static_cast<float>(dt)
                    : tap_period_ms * 0.5f + static_cast<float>(dt) * 0.5f;
                g_tapped_bpm.store(60000.0f / tap_period_ms);
            } else {
                tap_period_ms = 0.0f;  // gap too long/short — restart the series
            }
        }

        // Snapshot live state for the display.
        firmware::UiSnapshot s;
        s.link_up = g_link_up;
        prolink::Clock* c = g_clock.load(std::memory_order_acquire);
        if (b && c) {
            s.playing       = b->is_playing();
            s.clock_running = c->is_running();
            s.bpm           = c->current_bpm();
            s.master_dev    = b->current_master_num();
            s.beat_in_bar   = c->current_beat_in_bar();
            s.offset_ms     = b->total_offset_ms();
            s.phase_err_us  = c->current_phase_error_us();
        }
        s.selected_src = g_selected_src.load();
        s.pitch_pct    = g_pitch_pct.load();
        s.tapped_bpm   = g_tapped_bpm.load();
#ifdef DIAG_SERIAL_STUB
        s.usb_state = "diag";
#else
        switch (g_midi.host_state()) {
            case firmware::MidiHostUsb::HostState::kReady:        s.usb_state = "rdy";   break;
            case firmware::MidiHostUsb::HostState::kWaiting:      s.usb_state = "wait";  break;
            case firmware::MidiHostUsb::HostState::kDeviceNoMidi: s.usb_state = "nomid"; break;
            default:                                              s.usb_state = "off";   break;
        }
#endif
        firmware::ui_display_render(s);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

}  // namespace

void setup() {
    delay(500);
    printf("\n[xdj-bridge] firmware — full Bridge integration + USB MIDI host\n");

    // Initialize the onboard WS2812B and flash a brief identity color so
    // we can tell at a glance that setup() ran (independent of network
    // state). Yellow until link comes up, then the beat callbacks take over.
    led_begin();
    led_set(g_pixel.Color(60, 40, 0));  // yellow: setup() ran

    init_ethernet();

#ifndef DIAG_SERIAL_STUB
    // Install the USB host driver and spawn the host/client/sender tasks.
    // This takes over the USB-OTG peripheral and silences the USB-Serial-
    // JTAG console — anything printed after this line goes nowhere unless
    // a USB-UART breakout is wired to U0TXD/U0RXD on header H1.
    if (!g_midi.begin()) {
        // Failure here means MIDI clocks won't go anywhere over USB. The
        // rest of the bridge still works (counters, LED, status logging
        // before the console died); we just don't clock anything.
        led_set(g_pixel.Color(80, 0, 80));  // magenta = host install failed
    }
#else
    // Diagnostic build: USB host left uninstalled so the serial console
    // survives. g_midi is a counter-only stub; clock bytes are tallied,
    // not transmitted. Everything before this is identical to production.
    printf("[diag] SERIAL STUB build — USB host NOT installed; console stays alive\n");
    i2c_scan_diag();
#endif

    // Front-panel UI: input polling (encoder/buttons) + OLED, both on Core 0
    // so the SW-I2C refresh never competes with the clock/bridge on Core 1.
    firmware::ui_input_begin();
    xTaskCreatePinnedToCore(ui_task, "ui", 4096, nullptr, 3, nullptr, 0);

    // Bridge task on Core 1 alongside the Arduino loop. 8 KB stack is
    // generous — keepalive packet build, parsers, and EMA math all live
    // on this stack.
    xTaskCreatePinnedToCore(bridge_task, "bridge", 8192, nullptr, 5, nullptr, 1);
}

void loop() {
    static uint32_t last = 0;
    const uint32_t now = millis();

#ifndef DIAG_SERIAL_STUB
    // Reflect USB host state on the LED until a device is ready; once
    // ready, the on_beat callback owns the LED with green beat flashes.
    if (g_midi.host_state() != firmware::MidiHostUsb::HostState::kReady) {
        led_set(led_color_for_host_state(g_midi.host_state()));
    }
#endif

    if (now - last >= 5000) {
        last = now;
#ifdef DIAG_SERIAL_STUB
        printf("[status] link=%s clocks=%llu starts=%llu stops=%llu bytes=%llu\n",
               g_link_up ? "up" : "down",
               g_midi.clock_ticks_sent(),
               g_midi.start_messages(),
               g_midi.stop_messages(),
               g_midi.bytes_sent());
#else
        printf("[status] link=%s usb_dev=%s clocks=%llu starts=%llu stops=%llu sent=%llu dropped=%llu\n",
               g_link_up ? "up" : "down",
               g_midi.is_device_connected() ? "attached" : "none",
               g_midi.clock_ticks_sent(),
               g_midi.start_messages(),
               g_midi.stop_messages(),
               g_midi.bytes_sent(),
               g_midi.bytes_dropped());
#endif
    }
    delay(100);
}
