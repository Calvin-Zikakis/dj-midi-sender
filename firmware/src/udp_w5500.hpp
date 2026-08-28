#pragma once

// Firmware twin of desktop/udp_posix.hpp. The lwIP BSD-socket API is the
// same shape as POSIX, so the implementations are deliberately line-for-line
// equivalent — when the desktop UdpPosix changes, port the change here too.

#include "bridge.hpp"

#include <cstdint>

namespace firmware {

class UdpW5500 : public prolink::IUdpSocket {
public:
    UdpW5500() = default;
    ~UdpW5500() override;
    UdpW5500(const UdpW5500&) = delete;
    UdpW5500& operator=(const UdpW5500&) = delete;

    // Binds a UDP socket to (bind_addr, port) and enables SO_REUSEADDR.
    // Returns false on failure; the reason is printed to stdout.
    bool bind(const char* bind_addr, uint16_t port);

    // Enables SO_BROADCAST so subsequent sends to subnet broadcast
    // addresses (e.g. 169.254.255.255 on the Pro DJ Link auto-IP network)
    // go out instead of being dropped.
    bool enable_broadcast();

    int  recv(uint8_t* buf, size_t len, uint32_t timeout_ms,
              uint32_t* src_ip = nullptr) override;
    bool send(const uint8_t* buf, size_t len,
              uint32_t ip, uint16_t port) override;

private:
    int fd_ = -1;
};

}  // namespace firmware
