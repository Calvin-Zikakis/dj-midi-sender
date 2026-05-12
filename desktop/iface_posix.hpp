#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace desktop {

struct InterfaceInfo {
    std::string name;
    uint8_t     mac[6];
    uint32_t    ipv4;        // host-order uint32 (e.g. 169.254.182.10 → 0xA9FEB60A)
    uint32_t    broadcast;   // host-order uint32 of the directed broadcast
};

// Look up MAC + IPv4 + broadcast for the named interface. Pass an empty name
// to auto-pick the first non-loopback IPv4 interface that has a MAC.
std::optional<InterfaceInfo> find_interface(const std::string& name = "");

// Convenience: format a host-order uint32 IPv4 as "a.b.c.d".
std::string format_ipv4(uint32_t host_order);

}  // namespace desktop
