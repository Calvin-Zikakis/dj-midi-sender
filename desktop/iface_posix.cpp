#include "iface_posix.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <vector>

#if defined(__APPLE__)
#include <net/if_dl.h>      // AF_LINK / sockaddr_dl
#else
#include <linux/if_packet.h> // AF_PACKET / sockaddr_ll
#endif

namespace desktop {

namespace {
uint32_t to_host(const sockaddr_in* a) {
    return ntohl(a->sin_addr.s_addr);
}
}  // namespace

std::optional<InterfaceInfo> find_interface(const std::string& name) {
    ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) < 0) {
        std::perror("getifaddrs");
        return std::nullopt;
    }

    struct Candidate {
        std::string name;
        bool have_mac = false;
        bool have_ip  = false;
        uint8_t mac[6] = {0};
        uint32_t ip = 0;
        uint32_t mask = 0;
        bool is_loopback = false;
    };

    // Two passes: first scan AF_INET addresses, then patch in MACs from AF_LINK.
    // We keep candidates keyed by interface name.
    std::optional<InterfaceInfo> result;

    auto try_finalize = [&](Candidate& c) -> std::optional<InterfaceInfo> {
        if (!c.have_mac || !c.have_ip) return std::nullopt;
        if (c.is_loopback) return std::nullopt;
        InterfaceInfo info{};
        info.name = c.name;
        std::memcpy(info.mac, c.mac, 6);
        info.ipv4 = c.ip;
        info.broadcast = c.ip | (~c.mask);
        return info;
    };

    // Walk twice — once to collect AF_INET, once for hardware. Using arrays
    // is overkill; just collect into a small vector keyed by name.
    std::vector<Candidate> candidates;

    for (ifaddrs* p = ifaddr; p; p = p->ifa_next) {
        if (!p->ifa_addr) continue;
        if (!(p->ifa_flags & IFF_UP)) continue;
        if (p->ifa_addr->sa_family != AF_INET) continue;

        Candidate* c = nullptr;
        for (auto& cand : candidates) {
            if (cand.name == p->ifa_name) { c = &cand; break; }
        }
        if (!c) {
            candidates.emplace_back();
            c = &candidates.back();
            c->name = p->ifa_name;
            c->is_loopback = (p->ifa_flags & IFF_LOOPBACK) != 0;
        }
        c->have_ip = true;
        c->ip = to_host(reinterpret_cast<sockaddr_in*>(p->ifa_addr));
        if (p->ifa_netmask) {
            c->mask = to_host(reinterpret_cast<sockaddr_in*>(p->ifa_netmask));
        } else {
            c->mask = 0xFFFFFF00u;
        }
    }

    for (ifaddrs* p = ifaddr; p; p = p->ifa_next) {
        if (!p->ifa_addr) continue;
#if defined(__APPLE__)
        if (p->ifa_addr->sa_family != AF_LINK) continue;
        const sockaddr_dl* sdl = reinterpret_cast<const sockaddr_dl*>(p->ifa_addr);
        if (sdl->sdl_alen != 6) continue;
        const uint8_t* mac = reinterpret_cast<const uint8_t*>(LLADDR(sdl));
#else
        if (p->ifa_addr->sa_family != AF_PACKET) continue;
        const sockaddr_ll* sll = reinterpret_cast<const sockaddr_ll*>(p->ifa_addr);
        if (sll->sll_halen != 6) continue;
        const uint8_t* mac = sll->sll_addr;
#endif
        for (auto& cand : candidates) {
            if (cand.name == p->ifa_name) {
                std::memcpy(cand.mac, mac, 6);
                cand.have_mac = true;
                break;
            }
        }
    }

    freeifaddrs(ifaddr);

    if (!name.empty()) {
        for (auto& c : candidates) {
            if (c.name == name) {
                auto info = try_finalize(c);
                if (info) result = info;
                break;
            }
        }
    } else {
        // Prefer link-local IPv4 (169.254/16) since that's what Pro DJ Link
        // lives on; otherwise fall back to the first non-loopback IPv4 with
        // a MAC.
        for (auto& c : candidates) {
            if (c.is_loopback || !c.have_mac || !c.have_ip) continue;
            if ((c.ip & 0xFFFF0000u) == 0xA9FE0000u) {  // 169.254/16
                result = try_finalize(c);
                if (result) return result;
            }
        }
        for (auto& c : candidates) {
            auto info = try_finalize(c);
            if (info) { result = info; break; }
        }
    }
    return result;
}

std::string format_ipv4(uint32_t host_order) {
    char buf[INET_ADDRSTRLEN];
    in_addr a;
    a.s_addr = htonl(host_order);
    if (!inet_ntop(AF_INET, &a, buf, sizeof(buf))) return "?";
    return std::string(buf);
}

}  // namespace desktop
