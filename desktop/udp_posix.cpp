#include "udp_posix.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace desktop {

UdpPosix::~UdpPosix() {
    if (fd_ >= 0) ::close(fd_);
}

bool UdpPosix::bind(const char* bind_addr, uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        std::fprintf(stderr, "[udp] socket() failed: %s\n", std::strerror(errno));
        return false;
    }

    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (bind_addr == nullptr || std::strcmp(bind_addr, "") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        std::fprintf(stderr, "[udp] bad bind address: %s\n", bind_addr);
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "[udp] bind(%s:%u) failed: %s\n",
                     bind_addr ? bind_addr : "0.0.0.0", port,
                     std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

bool UdpPosix::enable_broadcast() {
    if (fd_ < 0) return false;
    int yes = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) < 0) {
        std::fprintf(stderr, "[udp] enable_broadcast failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    return true;
}

int UdpPosix::recv(uint8_t* buf, size_t len, uint32_t timeout_ms,
                   uint32_t* src_ip) {
    if (fd_ < 0) return -1;

    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in from{};
    socklen_t   from_len = sizeof(from);
    ssize_t     n = ::recvfrom(fd_, buf, len, 0,
                               reinterpret_cast<sockaddr*>(&from), &from_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return -1;
    }
    if (src_ip) *src_ip = ntohl(from.sin_addr.s_addr);  // host-order out
    return static_cast<int>(n);
}

bool UdpPosix::send(const uint8_t* buf, size_t len,
                    uint32_t ip, uint16_t port) {
    if (fd_ < 0) return false;
    sockaddr_in to{};
    to.sin_family      = AF_INET;
    to.sin_port        = htons(port);
    to.sin_addr.s_addr = htonl(ip);  // we treat ip as host-order uint32 for clarity
    ssize_t n = ::sendto(fd_, buf, len, 0,
                         reinterpret_cast<sockaddr*>(&to), sizeof(to));
    return n == static_cast<ssize_t>(len);
}

}  // namespace desktop
