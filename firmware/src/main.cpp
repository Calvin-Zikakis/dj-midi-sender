// Phase 2 firmware — Ethernet bring-up + raw UDP listener.
//
// Initializes the onboard W5500 over SPI, attaches it to an lwIP netif
// with a static link-local IP (169.254.42.42/16, matching the Pro DJ Link
// auto-IP subnet), and spawns two UDP listener tasks that hex-dump every
// packet they see. Port 50001 is the beat-packet broadcast, 50000 is the
// keep-alive broadcast every device on the link emits at ~5 Hz — so we
// should see *something* even without a track playing.
//
// Still no lib/prolink integration. This is the "are real Pro DJ Link
// packets reaching the box?" gate before we wire the parsers in.

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
#include "lwip/sockets.h"

namespace {

// W5500 SPI clock. Datasheet allows up to ~80 MHz but real-world margin is
// tighter — 20 MHz is a conservative starting point that leaves headroom
// for board layout and bypass cap quality.
constexpr int kSpiClockHz = 20 * 1000 * 1000;

// Locally-administered MAC (bit 1 of first byte set, OUI bit cleared) so
// we don't collide with anyone's real burned-in address on the Pro DJ Link
// segment. Anything beyond that first byte is arbitrary.
uint8_t g_mac[6] = {0x02, 0x00, 0x00, 0xDB, 0xC3, 0x42};

esp_netif_t* g_eth_netif = nullptr;
esp_eth_handle_t g_eth_handle = nullptr;
volatile bool g_link_up = false;

// "Qspt1WmJOL" — the magic at the start of every Pro DJ Link packet.
constexpr uint8_t kProDjMagic[10] = {
    0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c
};

const char* prodj_packet_type_name(uint8_t type_byte) {
    switch (type_byte) {
        case 0x06: return "keepalive";
        case 0x0A: return "status";
        case 0x28: return "beat";
        default:   return "prodj?";
    }
}

void udp_listener_task(void* arg) {
    const uint16_t port = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(arg));

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        printf("[udp:%u] socket() failed: errno=%d\n", port, errno);
        vTaskDelete(nullptr);
        return;
    }

    sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        printf("[udp:%u] bind() failed: errno=%d\n", port, errno);
        close(sock);
        vTaskDelete(nullptr);
        return;
    }
    printf("[udp:%u] listening\n", port);

    uint8_t buf[1500];
    uint32_t packet_count = 0;
    for (;;) {
        sockaddr_in src = {};
        socklen_t srclen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         reinterpret_cast<sockaddr*>(&src), &srclen);
        if (n < 0) {
            printf("[udp:%u] recvfrom error: errno=%d\n", port, errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const bool is_prodj = (n >= 11 && memcmp(buf, kProDjMagic, 10) == 0);
        const char* type_label = is_prodj ? prodj_packet_type_name(buf[10]) : "non-prodj";

        char src_ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &src.sin_addr, src_ip, sizeof(src_ip));

        // First 16 bytes is enough to confirm magic + type + device ID;
        // we don't want to spam the console with full packet dumps.
        printf("[udp:%u] #%lu from %s:%u len=%d type=%s [",
               port,
               static_cast<unsigned long>(++packet_count),
               src_ip,
               ntohs(src.sin_port),
               n,
               type_label);
        const int dump = n < 16 ? n : 16;
        for (int i = 0; i < dump; i++) {
            printf(" %02x", buf[i]);
        }
        printf(" ]\n");
    }
}

void on_eth_event(void*, esp_event_base_t, int32_t event_id, void*) {
    switch (event_id) {
        case ETHERNET_EVENT_START:
            printf("[eth] driver started\n");
            break;
        case ETHERNET_EVENT_STOP:
            printf("[eth] driver stopped\n");
            break;
        case ETHERNET_EVENT_CONNECTED:
            g_link_up = true;
            printf("[eth] link up\n");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            g_link_up = false;
            printf("[eth] link down\n");
            break;
        default:
            break;
    }
}

void on_got_ip(void*, esp_event_base_t, int32_t, void* event_data) {
    auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    const esp_netif_ip_info_t& info = event->ip_info;
    printf("[eth] got IP: " IPSTR " / mask " IPSTR " / gw " IPSTR "\n",
           IP2STR(&info.ip), IP2STR(&info.netmask), IP2STR(&info.gw));
}

void init_ethernet() {
    // Arduino-esp32 already calls these during boot, but they're idempotent
    // when the second call returns ESP_ERR_INVALID_STATE.
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
    // it doesn't install the service itself. Calling this twice would
    // return ESP_ERR_INVALID_STATE, which is harmless — swallow it.
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
    // moved to (host, &devcfg). When we upgrade platforms, the call
    // below collapses into ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &devcfg).
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

    // Static link-local IP — Pro DJ Link uses RFC 3927 auto-IP (169.254/16),
    // not DHCP. Stop the DHCP client (it's enabled by default on the netif)
    // and force our address.
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(g_eth_netif));
    esp_netif_ip_info_t ip_info = {};
    IP4_ADDR(&ip_info.ip, 169, 254, 42, 42);
    IP4_ADDR(&ip_info.netmask, 255, 255, 0, 0);
    // No gateway — link-local has no upstream
    ESP_ERROR_CHECK(esp_netif_set_ip_info(g_eth_netif, &ip_info));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_eth_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip, nullptr));

    ESP_ERROR_CHECK(esp_eth_start(g_eth_handle));

    printf("[eth] W5500 init complete — MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    printf("[eth] static IP 169.254.42.42/16, no gateway\n");
}

}  // namespace

void setup() {
    // No Serial.begin — printf goes to ESP-IDF's stdio, which is wired to
    // the USB-Serial-JTAG console (same channel as the bootloader logs).
    delay(500);
    printf("\n[xdj-bridge] firmware skeleton — UDP listener\n");
    init_ethernet();

    // Two listeners: 50001 catches XZ beat packets while a track is
    // playing; 50000 catches the keep-alive broadcast every Pro DJ Link
    // device emits at ~5 Hz, so the path is verifiable even with no
    // music playing. Both go to Core 1 (Arduino's loop core).
    xTaskCreatePinnedToCore(udp_listener_task, "udp50001", 4096,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(50001)),
                            5, nullptr, 1);
    xTaskCreatePinnedToCore(udp_listener_task, "udp50000", 4096,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(50000)),
                            5, nullptr, 1);
}

void loop() {
    static uint32_t last = 0;
    const uint32_t now = millis();
    if (now - last >= 2000) {
        last = now;

        esp_netif_ip_info_t info = {};
        if (g_eth_netif) {
            esp_netif_get_ip_info(g_eth_netif, &info);
        }
        printf("[status] link=%s ip=" IPSTR "\n",
               g_link_up ? "up" : "down", IP2STR(&info.ip));
    }
    delay(50);
}
