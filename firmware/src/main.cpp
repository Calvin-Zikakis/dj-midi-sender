// Phase 2 firmware — full Bridge integration on the Waveshare ESP32-S3-ETH.
//
// Same lib/prolink/ core that drives the desktop bridge, wrapped in firmware
// I/O implementations:
//   - UdpW5500  : IUdpSocket  ← lwIP BSD sockets over the onboard W5500
//   - TimerEsp  : ITimer      ← esp_timer hardware-timer one-shot loop
//   - MidiFanOut: IMidiOut    ← one PLL, two outputs: USB MIDI host + DIN UART
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
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bridge.hpp"
#include "clock.hpp"
#include "packets.hpp"
#include "types.hpp"

#include "midi_fanout.hpp"
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

// DIN-5 MIDI out — UART1 TX on IO17 → 10 Ω → DIN pin 5 (3V3 → 47 Ω → pin 4).
// Active in BOTH builds: UART1 is independent of the USB PHY, so the diag
// build can validate the DIN jack with the serial console alive.
firmware::MidiUart g_midi_din;

// One PLL, two outputs. The Clock writes here; the fan-out forwards each
// byte to the USB host (or diag stub) AND the DIN jack. Each sink is
// non-blocking with its own drop counters, so DIN keeps clocking even with
// no USB device attached (and vice versa).
firmware::MidiFanOut g_midi_out(g_midi, g_midi_din);

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
std::atomic<uint8_t> g_selected_src{0};



// Boot default for the clock offset (lead-time compensation for USB + OP-XY
// latency). Measured by ear ~+30 ms. This is only the *first-boot fallback* —
// once the user nudges and we persist to NVS, the saved value wins on boot.
constexpr float kDefaultClockOffsetMs = 30.0f;

// ── Clock-offset persistence (NVS) ─────────────────────────────────────
// The nudged offset survives reboots. Stored as integer milliseconds (the
// nudge step is 1 ms, so that's full resolution). Writes are debounced by the
// UI task so we don't wear the flash on every button press.
constexpr const char* kNvsNamespace  = "xdjbridge";
constexpr const char* kNvsKeyOffset   = "clk_off_ms";   // legacy: whole ms
constexpr const char* kNvsKeyOffset10 = "clk_off_dms";  // tenths of a ms
constexpr const char* kNvsKeyActPlayer  = "actplayer";// join the link as a player
constexpr const char* kNvsKeyBpmStep    = "bpmstep";  // index into kBpmStepValues
constexpr const char* kNvsKeyOffsetStep = "offstep";  // index into kOffsetStepValues
constexpr const char* kNvsKeyKeepPlaying = "keepplay"; // hold clock when decks stop
// (Keys "mode" and "finestep" were retired with the Sync/Free toggle and the
// second BPM-step setting; any stale values on existing units are ignored.)

void nvs_init_once() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

// Load the saved offset, or `fallback` if nothing's stored yet.
// Stored in tenths of a millisecond: the offset step can be 0.1 ms, so whole
// milliseconds would silently discard a trim across a reboot. Falls back to the
// old whole-millisecond key so an existing unit keeps its calibration.
float nvs_load_offset_ms(float fallback) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return fallback;
    int32_t v = 0;
    float   out = fallback;
    if (nvs_get_i32(h, kNvsKeyOffset10, &v) == ESP_OK) {
        out = static_cast<float>(v) / 10.0f;
    } else if (nvs_get_i32(h, kNvsKeyOffset, &v) == ESP_OK) {
        out = static_cast<float>(v);           // migrate from the legacy key
    }
    nvs_close(h);
    return out;
}

void nvs_save_offset_ms(float ms) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    const float tenths = ms * 10.0f;
    const int32_t rounded =
        static_cast<int32_t>(tenths < 0.0f ? tenths - 0.5f : tenths + 0.5f);
    nvs_set_i32(h, kNvsKeyOffset10, rounded);
    nvs_commit(h);
    nvs_close(h);
}

// Generic i32 helpers for the menu settings (mode, BPM-step index).
int32_t nvs_load_i32(const char* key, int32_t def) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return def;
    int32_t v = def;
    esp_err_t err = nvs_get_i32(h, key, &v);
    nvs_close(h);
    return (err == ESP_OK) ? v : def;
}

void nvs_save_i32(const char* key, int32_t val) {
    nvs_handle_t h;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

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
    // Tempo-master mode broadcasts beat/status packets from these sockets.
    beat_sock.enable_broadcast();
    status_sock.enable_broadcast();

    firmware::TimerEsp timer;
    prolink::Clock clock(g_midi_out, timer, /* gain_divisor */ 16);

    prolink::BridgeConfig cfg;
    // Two device numbers, because the constraints conflict (both verified live
    // against an XDJ-XZ + XDJ-700):
    //   - Only PLAYER slots (1-4) can hold tempo master. On mixer slots (5/6)
    //     the current master offers the handoff (sets its Mh to us) but never
    //     completes it.
    //   - But a 4-channel unit like the XDJ-XZ treats all four player slots as
    //     its own. Sitting on one permanently breaks its deck-to-deck master
    //     handoff — even with a proper claim handshake.
    // So idle on a harmless number and only claim a player slot for as long as
    // we are actually master (you don't need deck-to-deck handoff while the box
    // is the master anyway). Overridable for other rigs.
    #ifndef DEVICE_NUM
    #define DEVICE_NUM 7          // idle: out of every player/mixer slot
    #endif
    #ifndef MASTER_DEVICE_NUM
    #define MASTER_DEVICE_NUM 4   // claimed only while acting as tempo master
    #endif
    cfg.device_num        = DEVICE_NUM;
    cfg.master_device_num = MASTER_DEVICE_NUM;
    std::strncpy(cfg.device_name, "xdj-bridge", sizeof(cfg.device_name) - 1);
    std::memcpy(cfg.mac, g_mac, 6);
    cfg.local_ip            = kLocalIpHost;
    cfg.broadcast_ip        = kBroadcastIpHost;
    cfg.send_vcdj_announce  = true;
    // Verbose logs the master handoff (0x26 sends, yields) and other bridge
    // events. Serial only exists in the diag build, so enable it there.
#ifdef DIAG_SERIAL_STUB
    cfg.verbose             = true;
#else
    cfg.verbose             = false;
#endif
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

#ifdef DIAG_SERIAL_STUB
    // Raw status/beat hex dump for tempo-master protocol RE. Our captures have
    // no status packets, so this is how we get a real byte template from live
    // hardware (XDJ-XZ + any CDJs on the link). Budget-limited so serial isn't
    // flooded; a reboot re-arms it. Prints device number + master flag so both
    // devices are distinguishable in the log.
    // Compact master-handoff logger: print the master-relevant fields for a
    // device only when they change, so a real master handoff between decks
    // shows its exact Mm/Mh/Syncn/flags transitions without flooding serial.
    cb.on_status_raw = [](const uint8_t* buf, size_t len) {
        if (len < 0xA7) return;
        const uint8_t  dev    = buf[0x21];
        const uint8_t  flags  = buf[0x89];
        const uint8_t  mm     = buf[0x9E];
        const uint8_t  mh     = buf[0x9F];
        const uint32_t syncn  = (uint32_t(buf[0x84]) << 24) | (uint32_t(buf[0x85]) << 16) |
                                (uint32_t(buf[0x86]) << 8) | buf[0x87];
        struct HS { uint8_t flags, mm, mh; uint32_t syncn; bool seen; };
        static HS prev[8] = {};
        if (dev >= 8) return;
        HS& p = prev[dev];
        if (p.seen && p.flags == flags && p.mm == mm && p.mh == mh && p.syncn == syncn) return;
        p = {flags, mm, mh, syncn, true};
        const uint16_t bpm = (uint16_t(buf[0x92]) << 8) | buf[0x93];
        printf("[hs] dev=%u flags=0x%02x master=%d mm=0x%02x mh=0x%02x syncn=%lu bpm=%.2f beat=%u\n",
               dev, flags, (flags >> 5) & 1, mm, mh, (unsigned long)syncn, bpm / 100.0, buf[0xA6]);
    };
    cb.on_beat_raw = [](const uint8_t* buf, size_t len) {
        static int budget = 4;   // a couple of beats, to compare a CDJ vs our emitter
        if (budget <= 0) return;
        --budget;
        const uint8_t dev = (len > 0x21) ? buf[0x21] : 0;
        printf("[beat-raw] dev=%u len=%u\n[beat-raw] ", dev, (unsigned)len);
        for (size_t i = 0; i < len; ++i) printf("%02x", buf[i]);
        printf("\n");
    };
    // Unknown packet types (not beat 0x28 / status 0x0a) on 50001/50002 —
    // hunting the master-takeover REQUEST command. Full hex so we can decode it.
    cb.on_raw_datagram = [](uint16_t port, const uint8_t* buf, size_t len) {
        if (len < 0x0B) return;
        const uint8_t type = buf[0x0A];
        if (type == 0x28 || type == 0x0A) return;   // skip known beat/status
        printf("[cmd] port=%u type=0x%02x dev=%u len=%u\n[cmd] ", port, type,
               (len > 0x21) ? buf[0x21] : 0, (unsigned)len);
        for (size_t i = 0; i < len; ++i) printf("%02x", buf[i]);
        printf("\n");
    };
#endif

    prolink::Bridge bridge(beat_sock, status_sock, keepalive_sock,
                           clock, cfg, cb);
    bridge.set_clock_offset_ms(nvs_load_offset_ms(kDefaultClockOffsetMs));  // saved, else +30

    // Publish for the UI task now that both objects exist on this stack.
    g_clock.store(&clock, std::memory_order_release);
    g_bridge.store(&bridge, std::memory_order_release);

    // Fire a Pro DJ Link beat packet on every clock beat when master mode is on
    // (no-op otherwise). Both objects outlive run() on this stack.
    clock.set_on_beat([&bridge]() { bridge.on_master_beat(); });


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
// the clock-critical bridge/timer on Core 1) at 25 fps over hardware I2C;
// unchanged frames are not transmitted (see ui_display.cpp).
// Push a clock-source selection to the bridge. `g_selected_src` is the single
// source of truth for what the panel claims; this is the only thing that makes
// the bridge agree with it, so every path that changes the selection must call
// it — the confirm handler, and the fallbacks that fire when the box loses or
// releases the master role. Skipping it used to leave a deck pinned (or the box
// standalone) while the panel showed `follower master`.
void ui_apply_source(prolink::Bridge* b, uint8_t src) {
    if (!b) return;
    if (src != firmware::kSourceMaster && b->master_mode()) {
        b->set_master_mode(false);   // graceful release, appoints a deck
    }
    if (src == firmware::kSourceMaster) {
        b->set_master_mode(true);
    } else if (src == firmware::kSourceOff) {
        b->set_ignore_master(true);
        b->set_force_master_device(0);
    } else {
        b->set_ignore_master(false);
        b->set_force_master_device(src);   // 0 = auto-track the master flag
    }
}

void ui_task(void*) {
    firmware::ui_display_begin();
#ifdef DIAG_SERIAL_STUB
    printf("[ui] task running; ui_display_begin() returned\n");
#endif

    // Front-panel UI mode + menu state.
    firmware::UiMode   ui_mode          = firmware::UiMode::kNormal;
    uint8_t            proposed_src     = 0;
    // What we have actually pushed to the bridge, so a selection made before
    // the bridge existed still takes effect.
    uint8_t            last_applied_src = 0;
    uint8_t            menu_index       = 0;
    int32_t            edit_value       = 0;
    uint32_t           mode_activity_ms = 0;
    constexpr uint32_t kSourceSelectIdleMs = 10000;
    constexpr uint32_t kMenuIdleMs         = 12000;

    // Step settings (indices into the option arrays), persisted via the menu.
    auto clamp_idx = [](int32_t v, int32_t n) { if (v < 0) v = 0; if (v >= n) v = n - 1; return v; };
    bool act_as_player      = nvs_load_i32(kNvsKeyActPlayer, 0) != 0;
    bool keep_playing       = nvs_load_i32(kNvsKeyKeepPlaying, 0) != 0;
    // One auto-claim per stop episode — see the latch below.
    bool auto_master_armed  = true;
    // Source that was selected before an auto-claim took the master role, so
    // handing the role back restores what the operator actually chose. 0xFF
    // means "the current master selection is the operator's, not ours".
    uint8_t src_before_auto_master = 0xFF;
    uint8_t bpm_step_idx    = static_cast<uint8_t>(clamp_idx(
        nvs_load_i32(kNvsKeyBpmStep, firmware::kBpmStepDefault), firmware::kBpmStepCount));
    uint8_t offset_step_idx = static_cast<uint8_t>(clamp_idx(
        nvs_load_i32(kNvsKeyOffsetStep, firmware::kOffsetStepDefault), firmware::kOffsetStepCount));

    // Classic tap-tempo state (Off / standalone source). Averages the last few
    // tap intervals (ring buffer) for accuracy; a >2 s gap starts a new series.
    constexpr uint8_t kTapAvgMax = 8;
    uint32_t tap_prev_ms = 0;
    float    tap_intervals[kTapAvgMax] = {0};
    uint8_t  tap_count = 0;   // valid intervals stored (caps at kTapAvgMax)
    uint8_t  tap_head  = 0;   // ring write index

    // Beat re-sync gesture (sync/free sources). A clean short tap-and-release
    // with no spin = "realign the beat" (Bridge::request_resync). Tracked on
    // release so it's distinguishable from the hold-tap+spin BPM modifier.
    uint32_t tap_down_ms       = 0;
    bool     tap_moved         = false;  // spun while tap held → it's the modifier
    bool     tap_consumed      = false;  // used as back/cancel by a sub-mode
    bool     tap_owns_tempo    = false;  // gesture meaning, latched at the press
    bool     prev_tap_held     = false;
    uint32_t resync_flash_until = 0;      // OLED "RSYNC" confirmation window
    bool     resync_flash_armed = false;
    constexpr uint32_t kTapMaxMs = 400;   // longer press = a hold, not a tap

    // Debounced NVS persistence of the clock offset.
    bool     save_init      = false;
    float    save_seen      = 0.0f;  // last offset value observed
    float    save_committed = 0.0f;  // last value written to NVS
    uint32_t save_settle_ms = 0;

    for (;;) {
        const int32_t  steps       = firmware::ui_input_take_encoder_steps();
        const uint32_t btns        = firmware::ui_input_take_button_presses();
        const int32_t  nudge       = firmware::ui_input_take_nudge_steps();
        const uint32_t menu_hold  = firmware::ui_input_take_menu_holds();
        prolink::Bridge* b = g_bridge.load(std::memory_order_acquire);
        const uint32_t now = millis();
        // Let the bridge tell a network fault apart from the music stopping:
        // a link blip must not send MIDI Stop and restart on a downbeat.
        if (b) b->set_link_up(g_link_up);
        if (b) b->set_keep_playing(keep_playing);

        // "Keep playing" with a player slot: when the decks stop, claim the
        // DJ-Link master role too, so a deck that restarts with sync on locks
        // back to the box instead of dragging the gear to its own tempo. It
        // goes through the normal source selection, so the panel shows what
        // the box is actually doing and the release path already works.
        //
        // Fire once per stop episode. Without the latch, a DJ reclaiming
        // master while the decks are still stopped would drop us back to
        // follower master, we would see "holding" again and immediately grab
        // it back — the box and the DJ fighting over the role. Re-arm only
        // once a deck is genuinely driving the clock again: is_playing() is
        // OUR clock, which stays true right through a hold and so cannot
        // answer that question.
        if (b && b->master_deck_playing()) {
            auto_master_armed = true;
        }
        if (b && keep_playing && act_as_player && auto_master_armed &&
            b->holding() && g_selected_src.load() != firmware::kSourceMaster) {
            auto_master_armed = false;
            src_before_auto_master = g_selected_src.load();
            g_selected_src.store(firmware::kSourceMaster);
        }
        const bool tap_held = firmware::ui_input_tap_held();

        // Track the tap gesture across every mode so a clean short tap (no spin)
        // can be recognized on release. Consumed only in Normal / non-off below.
        if (btns & firmware::kBtnTap) {
            tap_down_ms   = now;
            tap_moved     = false;
            tap_consumed  = false;
            // Latch the gesture's meaning at the press. The source can change
            // underneath us (a deck reclaiming master resets it), and without
            // this a tap that began as tap-tempo would also fire a re-sync on
            // release.
            const uint8_t src_at_press = g_selected_src.load();
            tap_owns_tempo = (src_at_press == firmware::kSourceOff ||
                              src_at_press == firmware::kSourceMaster);
        }
        if (tap_held && steps != 0)   tap_moved = true;

#ifdef DIAG_SERIAL_STUB
        if (steps) printf("[ui] encoder steps=%ld (mode=%d)\n", (long)steps, (int)ui_mode);
        if (btns)  printf("[ui] buttons mask=0x%lx (mode=%d)\n", (unsigned long)btns, (int)ui_mode);
#endif

        if (ui_mode == firmware::UiMode::kNormal) {
            // Sources where the BOX owns the tempo: standalone, and tempo-master
            // (as master we drive the decks, so the encoder sets our BPM and tap
            // is tap-tempo rather than a re-sync that would mean nothing).
            const uint8_t cur_src = g_selected_src.load();
            const bool  off_mode  = (cur_src == firmware::kSourceOff ||
                                     cur_src == firmware::kSourceMaster);
            const float bpm_step  = static_cast<float>(firmware::kBpmStepValues[bpm_step_idx]);

            if (off_mode) {
                // Box owns the tempo: plain spin adjusts BPM, tap = tap-tempo.
                if (b && steps != 0) b->nudge_manual_bpm(static_cast<float>(steps) * bpm_step);
                if (btns & firmware::kBtnTap) {
                    // Timed at the button settle, not at this frame: the UI
                    // runs at 25 fps, so frame-clock timing would quantise
                    // every interval by up to 40 ms (~8% of a beat at 128 BPM).
                    const uint32_t press = firmware::ui_input_tap_press_ms();
                    const uint32_t dt = press - tap_prev_ms;
                    tap_prev_ms = press;
                    if (dt > 250 && dt < 2000) {          // 30..240 BPM
                        tap_intervals[tap_head] = static_cast<float>(dt);
                        tap_head = (tap_head + 1) % kTapAvgMax;
                        if (tap_count < kTapAvgMax) ++tap_count;
                        float sum = 0.0f;
                        for (uint8_t i = 0; i < tap_count; ++i) sum += tap_intervals[i];
                        if (b && tap_count > 0) b->set_manual_bpm(60000.0f / (sum / tap_count));
                    } else {
                        tap_count = 0;                    // gap → start a new series
                        tap_head  = 0;
                    }
                }
            } else {
                // Following a deck: a clean short tap re-syncs the slave on the
                // master's next downbeat.
                if (prev_tap_held && !tap_held && !tap_moved && !tap_consumed &&
                    !tap_owns_tempo &&
                    (now - tap_down_ms) < kTapMaxMs && b) {
                    b->request_resync(/*immediate*/ false);
                    resync_flash_until = now + 900;
                    resync_flash_armed = true;
                }
            }

            // Nudge buttons → clock-offset trim (accelerating hold-repeat),
            // by the menu-configured step.
            if (b && nudge != 0)
                b->adjust_clock_offset_ms(static_cast<float>(nudge) *
                                          firmware::kOffsetStepValues[offset_step_idx]);
            // Both nudges held → open the settings menu.
            if (menu_hold > 0) {
                menu_index       = 0;
                mode_activity_ms = now;
                ui_mode          = firmware::UiMode::kMenu;
            }
            // Encoder push → enter source-select.
            if (btns & firmware::kBtnEncSw) {
                proposed_src     = g_selected_src.load();
                // If the active source is hidden (the gate was turned off while
                // it was selected) the list would render with no cursor at all.
                if (proposed_src == firmware::kSourceMaster && !act_as_player) {
                    proposed_src = 0;
                }
                mode_activity_ms = now;
                ui_mode          = firmware::UiMode::kSourceSelect;
            }
        } else if (ui_mode == firmware::UiMode::kSourceSelect) {
            // Spin moves the proposed cursor; the active source stays put until
            // we confirm.
            if (steps != 0) {
                // Walk cursor positions, not source ids: `sync master` is absent
                // from the list unless the box is allowed on the link.
                const int32_t n = firmware::source_count(act_as_player);
                int32_t cursor = 0;
                for (int32_t i = 0; i < n; ++i)
                    if (firmware::source_at(static_cast<uint8_t>(i), act_as_player) == proposed_src) {
                        cursor = i;
                        break;
                    }
                int32_t v = (cursor + steps) % n;
                if (v < 0) v += n;
                proposed_src     = firmware::source_at(static_cast<uint8_t>(v), act_as_player);
                mode_activity_ms = now;
            }
            if (btns & firmware::kBtnEncSw) {              // push = confirm
                // Never act on the gated entry, even if stale state left the
                // cursor on it (e.g. the setting was turned off meanwhile).
                if (proposed_src == firmware::kSourceMaster && !act_as_player) {
                    proposed_src = 0;
                }
                const uint8_t previous = g_selected_src.load();
                // A deliberate choice supersedes any auto-claim, so there is
                // nothing left to restore.
                src_before_auto_master = 0xFF;
                g_selected_src.store(proposed_src);
                // Re-confirming the *same* source is a no-op. Re-running it for
                // `sync master` would drop mm=1 and renegotiate the whole
                // handoff mid-set.
                if (proposed_src != previous) {
                    ui_apply_source(b, proposed_src);
                    last_applied_src = proposed_src;
                }
                ui_mode = firmware::UiMode::kNormal;
            } else if (btns & firmware::kBtnTap) {         // tap = cancel/back
                tap_consumed = true;   // don't let the release fire a re-sync
                ui_mode = firmware::UiMode::kNormal;
            } else if (now - mode_activity_ms >= kSourceSelectIdleMs) {
                ui_mode = firmware::UiMode::kNormal;        // idle auto-cancel
            }
        } else if (ui_mode == firmware::UiMode::kMenu) {
            // Scroll items; push to edit the highlighted one; tap to exit.
            if (steps != 0) {
                int32_t v = (static_cast<int32_t>(menu_index) + steps) % firmware::kMenuItemCount;
                if (v < 0) v += firmware::kMenuItemCount;
                menu_index       = static_cast<uint8_t>(v);
                mode_activity_ms = now;
            }
            if (btns & firmware::kBtnEncSw) {
                edit_value = (menu_index == firmware::kMenuItemActAsPlayer) ? (act_as_player ? 1 : 0)
                           : (menu_index == firmware::kMenuItemBpmStep)      ? bpm_step_idx
                           : (menu_index == firmware::kMenuItemOffsetStep)   ? offset_step_idx
                                                                            : (keep_playing ? 1 : 0);
                mode_activity_ms = now;
                ui_mode          = firmware::UiMode::kMenuEdit;
            } else if (btns & firmware::kBtnTap) {
                tap_consumed = true;
                ui_mode = firmware::UiMode::kNormal;
            } else if (now - mode_activity_ms >= kMenuIdleMs) {
                ui_mode = firmware::UiMode::kNormal;
            }
        } else if (ui_mode == firmware::UiMode::kMenuEdit) {
            // Spin changes the working value; push commits + saves; tap cancels.
            if (steps != 0) {
                const int32_t n = (menu_index == firmware::kMenuItemActAsPlayer)  ? 2
                                : (menu_index == firmware::kMenuItemKeepPlaying) ? 2
                                : (menu_index == firmware::kMenuItemBpmStep)
                                      ? firmware::kBpmStepCount
                                      : firmware::kOffsetStepCount;
                int32_t v = (edit_value + steps) % n;
                if (v < 0) v += n;
                edit_value = v;
                mode_activity_ms = now;
            }
            if (btns & firmware::kBtnEncSw) {                  // confirm + persist
                if (menu_index == firmware::kMenuItemActAsPlayer) {
                    act_as_player = (edit_value != 0);
                    nvs_save_i32(kNvsKeyActPlayer, edit_value);
                    // Turning it off must never leave `sync master` selected —
                    // including when the bridge does not exist yet, or the
                    // selection would survive to be applied later.
                    if (!act_as_player &&
                        g_selected_src.load() == firmware::kSourceMaster) {
                        src_before_auto_master = 0xFF;
                        g_selected_src.store(0);
                        ui_apply_source(b, 0);
                        last_applied_src = 0;
                    }
                } else if (menu_index == firmware::kMenuItemBpmStep) {
                    bpm_step_idx = static_cast<uint8_t>(edit_value);
                    nvs_save_i32(kNvsKeyBpmStep, edit_value);
                } else if (menu_index == firmware::kMenuItemOffsetStep) {
                    offset_step_idx = static_cast<uint8_t>(edit_value);
                    nvs_save_i32(kNvsKeyOffsetStep, edit_value);
                } else {
                    keep_playing = (edit_value != 0);
                    nvs_save_i32(kNvsKeyKeepPlaying, edit_value);
                }
                mode_activity_ms = now;
                ui_mode          = firmware::UiMode::kMenu;
            } else if (btns & firmware::kBtnTap) {             // cancel
                tap_consumed     = true;
                mode_activity_ms = now;
                ui_mode          = firmware::UiMode::kMenu;
            } else if (now - mode_activity_ms >= kMenuIdleMs) {
                ui_mode = firmware::UiMode::kNormal;
            }
        }

        // Debounced persistence: write the offset to NVS ~2 s after it stops
        // changing, so a burst of nudges (or a hold-repeat sweep) results in a
        // single flash write. The boot value (loaded or default) seeds
        // save_committed, so we never rewrite it on startup.
        if (b) {
            const float cur = b->clock_offset_ms();
            if (!save_init) {
                save_init = true;
                save_seen = cur;
                save_committed = cur;
            } else if (cur != save_seen) {
                save_seen = cur;
                save_settle_ms = millis();
            } else if (cur != save_committed && (millis() - save_settle_ms) >= 2000) {
                nvs_save_offset_ms(cur);
                save_committed = cur;
            }
        }

        // If a deck reclaimed master from us, the bridge drops master mode on
        // its own — fall the panel back to `follower master`, and actually
        // apply it: the bridge may still be pinned to a deck or standalone from
        // whatever was selected before master mode.
        if (b && g_selected_src.load() == firmware::kSourceMaster && !b->master_mode()) {
            // If WE took the role because the decks stopped, give back the
            // source the operator had chosen — dropping a deliberate `player N`
            // pin on the floor because the box borrowed master for a track
            // change is a surprise they never asked for.
            const uint8_t back = (src_before_auto_master == 0xFF)
                                     ? 0 : src_before_auto_master;
            src_before_auto_master = 0xFF;
            g_selected_src.store(back);
            ui_apply_source(b, back);
            last_applied_src = back;
        }

        // The bridge does not exist until Ethernet is up, so a source chosen
        // before then reached the panel but never the bridge. Apply it once the
        // bridge appears (and after any change that could not be pushed).
        if (b && last_applied_src != g_selected_src.load()) {
            last_applied_src = g_selected_src.load();
            ui_apply_source(b, last_applied_src);
        }

        // Snapshot live state for the display.
        firmware::UiSnapshot s;
        s.link_up = g_link_up;
        prolink::Clock* c = g_clock.load(std::memory_order_acquire);
        if (b && c) {
            s.playing       = b->is_playing();
            s.ignore_master = b->ignore_master();
            s.clock_running = c->is_running();
            s.bpm           = c->current_bpm();
            s.master_dev    = b->current_master_num();
            s.beat_in_bar   = b->master_mode() ? b->master_beat_in_bar()
                                              : c->current_beat_in_bar();
            s.offset_ms     = b->total_offset_ms();
        }
        s.selected_src = g_selected_src.load();
        s.proposed_src = proposed_src;
        s.ui_mode      = ui_mode;
        s.menu_index      = menu_index;
        s.menu_edit       = edit_value;
        s.act_as_player   = act_as_player;
        s.keep_playing    = keep_playing;
        s.holding         = b ? b->holding() : false;
        s.bpm_step_idx    = bpm_step_idx;
        s.offset_step_idx = offset_step_idx;
        s.pitch_pct    = g_pitch_pct.load();
        // Unsigned difference, like every other timer here: an absolute
        // comparison misbehaves once per millis() wrap (~49 days).
        s.resync_flash = resync_flash_armed &&
                         static_cast<int32_t>(now - resync_flash_until) < 0;
        if (b) {
            s.master_wanted = b->master_mode();
            s.is_master     = b->is_tempo_master();
        }
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
        prev_tap_held = tap_held;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

}  // namespace

void setup() {
    delay(500);
    printf("\n[xdj-bridge] firmware — full Bridge integration + USB MIDI host\n");

    nvs_init_once();  // persisted clock offset; must precede bridge_task

#ifdef DIN_SELFTEST
    // Flag-gated DIN jack self-test (PLATFORMIO_BUILD_FLAGS='-DDIN_SELFTEST').
    // Blasts MIDI clock (0xF8) out the DIN jack at ~50/s (≈125 BPM) forever,
    // through the *production* MidiUart path — no XZ or network needed. Point a
    // MIDI-clock analyzer (desktop/xdj_clockmon) at the jack to prove the DIN
    // wiring end-to-end. Never proceeds to normal boot. Zero cost in normal
    // builds. (Used 2026-07-16 to confirm the jack after the pin-4/5 fix.)
    g_midi_din.begin(MIDI_DIN_TX_PIN);
    printf("[din-selftest] blasting 0xF8 @ 50/s out IO%d (~125 BPM) — no XZ needed\n",
           MIDI_DIN_TX_PIN);
    for (uint32_t n = 1;; ++n) {
        g_midi_din.send_byte(0xF8);
        if (n % 250 == 0) printf("[din-selftest] sent=%lu\n", (unsigned long)n);
        delay(20);
    }
#endif

    // DIN MIDI out. Bring up before the bridge task exists so TX idles high
    // (MIDI "no current") from the start. On failure the sink stays inert —
    // USB output is unaffected.
    g_midi_din.begin(MIDI_DIN_TX_PIN);

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
        printf("[status] link=%s clocks=%llu starts=%llu stops=%llu bytes=%llu din=%llu/%llu\n",
               g_link_up ? "up" : "down",
               g_midi.clock_ticks_sent(),
               g_midi.start_messages(),
               g_midi.stop_messages(),
               g_midi.bytes_sent(),
               g_midi_din.bytes_sent(),
               g_midi_din.bytes_dropped());
#else
        printf("[status] link=%s usb_dev=%s clocks=%llu starts=%llu stops=%llu sent=%llu dropped=%llu din=%llu/%llu\n",
               g_link_up ? "up" : "down",
               g_midi.is_device_connected() ? "attached" : "none",
               g_midi.clock_ticks_sent(),
               g_midi.start_messages(),
               g_midi.stop_messages(),
               g_midi.bytes_sent(),
               g_midi.bytes_dropped(),
               g_midi_din.bytes_sent(),
               g_midi_din.bytes_dropped());
#endif
    }
    delay(100);
}
