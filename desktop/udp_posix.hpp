#pragma once

#include "bridge.hpp"

#include <cstdint>
#include <string>

namespace desktop {

class UdpPosix : public prolink::IUdpSocket {
public:
    UdpPosix() = default;
    ~UdpPosix() override;
    UdpPosix(const UdpPosix&) = delete;
    UdpPosix& operator=(const UdpPosix&) = delete;

    // Binds a UDP socket to (bind_addr, port) and enables SO_REUSEADDR.
    // Returns false on failure; check errno or stderr for the reason.
    bool bind(const char* bind_addr, uint16_t port);

    // Enables SO_BROADCAST so subsequent sends to 255.255.255.255 (and
    // link-local broadcast addresses) go out.
    bool enable_broadcast();

    int  recv(uint8_t* buf, size_t len, uint32_t timeout_ms,
              uint32_t* src_ip = nullptr) override;
    bool send(const uint8_t* buf, size_t len,
              uint32_t ip, uint16_t port) override;

private:
    int fd_ = -1;
};

}  // namespace desktop
