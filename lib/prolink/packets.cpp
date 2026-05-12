#include "packets.hpp"

#include <cstring>

namespace prolink {

namespace {

bool has_magic(const uint8_t* buf, size_t len) {
    return len >= sizeof(MAGIC) && std::memcmp(buf, MAGIC, sizeof(MAGIC)) == 0;
}

uint16_t r16(const uint8_t* p) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(p[0]) << 8) |
         static_cast<uint16_t>(p[1]));
}

uint32_t r32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
            static_cast<uint32_t>(p[3]);
}

void copy_device_name(char dst[21], const uint8_t* src) {
    // Field is 20 bytes ASCII, null-padded at offset 0x0B in every packet type.
    std::memcpy(dst, src, 20);
    dst[20] = '\0';
    // Trim trailing nulls / padding for prettier prints.
    for (int i = 19; i >= 0 && (dst[i] == '\0' || dst[i] == ' '); --i) {
        dst[i] = '\0';
    }
}

}  // namespace

std::optional<BeatPacket> parse_beat_packet(const uint8_t* buf, size_t len) {
    if (!has_magic(buf, len))            return std::nullopt;
    if (len < 0x60)                      return std::nullopt;
    if (buf[0x0A] != PKT_TYPE_BEAT)      return std::nullopt;

    BeatPacket p{};
    p.device_num     = buf[0x21];
    copy_device_name(p.device_name, buf + 0x0B);
    p.ms_next_beat   = r32(buf + 0x24);
    p.ms_next_bar    = r32(buf + 0x2C);
    p.pitch_raw      = r32(buf + 0x54);
    p.track_bpm_x100 = r16(buf + 0x5A);
    p.beat_in_bar    = buf[0x5C];
    return p;
}

std::optional<StatusPacket> parse_status_packet(const uint8_t* buf, size_t len) {
    if (!has_magic(buf, len))            return std::nullopt;
    if (buf[0x0A] != PKT_TYPE_STATUS)    return std::nullopt;
    // Need to reach actual_pitch at 0x98 (4 bytes) and beat at 0xA6.
    if (len < 0xA7)                      return std::nullopt;

    // Field offsets verified against beat-link (CdjStatus.java), dysentery,
    // and python-prodj-link (network/packets.py StatusPacket struct).
    StatusPacket p{};
    p.device_num     = buf[0x21];
    copy_device_name(p.device_name, buf + 0x0B);
    p.pitch1_raw     = r32(buf + 0x8C);   // Pitch1 (effective) — the field
                                          //   the player broadcasts as "what
                                          //   I am playing at right now".
                                          //   In sync mode this tracks the
                                          //   tempo master.
    p.pitch2_raw     = r32(buf + 0x98);   // Pitch2 — local fader only;
                                          //   diverges from Pitch1 when synced.
                                          //   Stored for completeness; never
                                          //   consumed by the bridge.
    p.track_bpm_x100 = r16(buf + 0x92);
    p.flags          = buf[0x89];         // Low byte of state uint16 at 0x88;
                                          //   high byte is always 0 for the
                                          //   bits we care about.
    p.mv             = r16(buf + 0x90);   // BpmState: 0x8000 = rekordbox.
    p.beat_in_bar    = buf[0xA6];
    return p;
}

// Layout matches python-prodj-link's KeepAlivePacket (type_status, stype_status):
//   0x00..0x09  magic
//   0x0A        type (0x06)
//   0x0B        padding
//   0x0C..0x1F  device name (20 bytes, null-padded ASCII)
//   0x20        u1 = 0x01
//   0x21        device_type = 0x02 (CDJ)
//   0x22        padding
//   0x23        subtype = 0x36 (also the total packet length)
//   0x24        player_number (1..6 typical; we default to 5 to match python-prodj-link)
//   0x25        u2 = 0x01
//   0x26..0x2B  MAC address (6 bytes)
//   0x2C..0x2F  IP address (4 bytes, big-endian)
//   0x30        device_count = 0x02 (CDJ-3000 compat)
//   0x31..0x33  padding
//   0x34        flags = 0x01 (is_player_or_mixer)
//   0x35        u4 = 0x64 (CDJ-3000 compat)
size_t build_keepalive_packet(uint8_t* out, size_t out_len,
                              const char* device_name,
                              uint8_t device_num,
                              const uint8_t mac[6],
                              uint32_t ip_host_order) {
    constexpr size_t PKT_LEN = 0x36;  // 54 bytes total
    if (out_len < PKT_LEN) return 0;

    std::memset(out, 0, PKT_LEN);
    std::memcpy(out, MAGIC, sizeof(MAGIC));
    out[0x0A] = PKT_TYPE_KEEPALIVE;
    // 0x0B padding (zero)
    for (size_t i = 0; i < 20; ++i) {
        if (!device_name || device_name[i] == '\0') break;
        out[0x0C + i] = static_cast<uint8_t>(device_name[i]);
    }
    out[0x20] = 0x01;          // u1 constant
    out[0x21] = 0x02;          // device_type = CDJ
    // 0x22 padding
    out[0x23] = 0x36;          // subtype = total packet length
    out[0x24] = device_num;    // player number
    out[0x25] = 0x01;          // u2 constant
    std::memcpy(out + 0x26, mac, 6);
    out[0x2C] = static_cast<uint8_t>((ip_host_order >> 24) & 0xFF);
    out[0x2D] = static_cast<uint8_t>((ip_host_order >> 16) & 0xFF);
    out[0x2E] = static_cast<uint8_t>((ip_host_order >>  8) & 0xFF);
    out[0x2F] = static_cast<uint8_t>( ip_host_order        & 0xFF);
    out[0x30] = 0x02;          // device_count (CDJ-3000 compat)
    // 0x31..0x33 padding
    out[0x34] = 0x01;          // flags = is_player_or_mixer
    out[0x35] = 0x64;          // u4 (CDJ-3000 compat)
    return PKT_LEN;
}

}  // namespace prolink
