#include "udp_w5500.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "lwip/sockets.h"

namespace firmware {

UdpW5500::~UdpW5500() {
    if (fd_ >= 0) ::close(fd_);
}

bool UdpW5500::bind(const char* bind_addr, uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        printf("[udp] socket() failed: errno=%d\n", errno);
        return false;
    }

    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (bind_addr == nullptr || std::strcmp(bind_addr, "") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        printf("[udp] bad bind address: %s\n", bind_addr);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        printf("[udp] bind(%s:%u) failed: errno=%d\n",
               bind_addr ? bind_addr : "0.0.0.0", port, errno);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

bool UdpW5500::enable_broadcast() {
    if (fd_ < 0) return false;
    int yes = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) < 0) {
        printf("[udp] enable_broadcast failed: errno=%d\n", errno);
        return false;
    }
    return true;
}

int UdpW5500::recv(uint8_t* buf, size_t len, uint32_t timeout_ms) {
    if (fd_ < 0) return -1;

    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in from{};
    socklen_t   from_len = sizeof(from);
    int n = ::recvfrom(fd_, buf, len, 0,
                       reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return -1;
    }
    return n;
}

bool UdpW5500::send(const uint8_t* buf, size_t len,
                    uint32_t ip, uint16_t port) {
    if (fd_ < 0) return false;
    sockaddr_in to{};
    to.sin_family      = AF_INET;
    to.sin_port        = htons(port);
    to.sin_addr.s_addr = htonl(ip);  // host-order in, like UdpPosix
    int n = ::sendto(fd_, buf, len, 0,
                     reinterpret_cast<sockaddr*>(&to), sizeof(to));
    return n == static_cast<int>(len);
}

}  // namespace firmware
