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

#include "midi_uart.hpp"
#include "timer_esp.hpp"
#include "udp_w5500.hpp"

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

// ── Bridge task ────────────────────────────────────────────────────────
// Holds the I/O implementations and the Clock, builds a BridgeConfig that
// mirrors what desktop/main.cpp builds, and calls Bridge::run(). The run
// loop polls both UDP sockets with 5 ms timeouts, so this task is
// always-on but not pinned at 100% CPU.
firmware::MidiUartStub g_midi;

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
    };
    cb.on_status = [](const prolink::StatusPacket& p) {
        printf("[stat] dev=%u name='%s' bpm=%.2f pitch=%+.2f%% master=%d play=%d sync=%d on_air=%d Mv=0x%04x\n",
               p.device_num, p.device_name,
               p.effective_bpm(), p.pitch_percent(),
               p.is_master(), p.is_playing(), p.is_synced(), p.is_on_air(), p.mv);
    };
    cb.on_log = [](const char* msg) {
        printf("[bridge] %s\n", msg);
    };

    prolink::Bridge bridge(beat_sock, status_sock, keepalive_sock,
                           clock, cfg, cb);

    printf("[bridge] starting — vCDJ device %u, broadcast %s\n",
           cfg.device_num, "169.254.255.255");
    bridge.run();   // blocks until stop()
    printf("[bridge] run() returned\n");
    vTaskDelete(nullptr);
}

}  // namespace

void setup() {
    delay(500);
    printf("\n[xdj-bridge] firmware — full Bridge integration\n");
    init_ethernet();

    // Bridge task on Core 1 alongside the Arduino loop. 8 KB stack is
    // generous — keepalive packet build, parsers, and EMA math all live
    // on this stack.
    xTaskCreatePinnedToCore(bridge_task, "bridge", 8192, nullptr, 5, nullptr, 1);
}

void loop() {
    static uint32_t last = 0;
    const uint32_t now = millis();
    if (now - last >= 5000) {
        last = now;
        printf("[status] link=%s midi_clocks=%llu midi_starts=%llu midi_stops=%llu\n",
               g_link_up ? "up" : "down",
               g_midi.clock_ticks_sent(),
               g_midi.start_messages(),
               g_midi.stop_messages());
    }
    delay(100);
}
