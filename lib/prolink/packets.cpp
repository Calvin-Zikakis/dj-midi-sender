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

void w16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

void w32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >>  8) & 0xFF);
    p[3] = static_cast<uint8_t>( v        & 0xFF);
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
    p.syncn          = r32(buf + 0x84);   // master-generation counter.
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

size_t build_beat_packet(uint8_t* out, size_t out_len,
                         const char* device_name,
                         uint8_t device_num,
                         float bpm,
                         uint8_t beat_in_bar) {
    constexpr size_t PKT_LEN = 0x60;  // 96 bytes, matching the XDJ-XZ
    if (out_len < PKT_LEN) return 0;
    if (!(bpm > 0.0f))     return 0;
    if (beat_in_bar < 1 || beat_in_bar > 4) beat_in_bar = 1;

    std::memset(out, 0, PKT_LEN);
    std::memcpy(out, MAGIC, sizeof(MAGIC));  // 0x00..0x09
    out[0x0A] = PKT_TYPE_BEAT;               // 0x28

    // Device name, 20 bytes null-padded ASCII at 0x0B (same field the parser
    // reads back).
    for (size_t i = 0; i < 20; ++i) {
        if (!device_name || device_name[i] == '\0') break;
        out[0x0B + i] = static_cast<uint8_t>(device_name[i]);
    }

    out[0x1F] = 0x01;          // subtype / proto version (from capture)
    // 0x20 = 0x00
    out[0x21] = device_num;    // device number
    w16(out + 0x22, 0x003C);   // fixed length field (from capture)

    // Timing prediction fields (uint32 BE ms until each upcoming boundary).
    // Derived empirically from captures/xdj-xz-export-mode.pcapng: as multiples
    // of the beat interval, by beat_in_bar b:
    //   next=1  2nd=2  next-bar=(5-b)  4th=4  2nd-bar=(9-b)  8th=8
    const double interval = 60000.0 / static_cast<double>(bpm);
    auto ms = [&](int mult) {
        return static_cast<uint32_t>(interval * mult + 0.5);
    };
    w32(out + 0x24, ms(1));                        // ms to next beat
    w32(out + 0x28, ms(2));                        // ms to 2nd beat
    w32(out + 0x2C, ms(5 - beat_in_bar));          // ms to next bar (downbeat)
    w32(out + 0x30, ms(4));                        // ms to 4th beat
    w32(out + 0x34, ms(9 - beat_in_bar));          // ms to 2nd bar
    w32(out + 0x38, ms(8));                        // ms to 8th beat

    std::memset(out + 0x3C, 0xFF, 0x18);           // 0x3C..0x53 reserved (0xFF)
    w32(out + 0x54, PITCH_UNITY);                  // pitch 1.0x — we are the tempo
    // 0x58..0x59 reserved = 0
    w16(out + 0x5A, static_cast<uint16_t>(bpm * 100.0f + 0.5f));  // BPM x100
    out[0x5C] = beat_in_bar;                       // beat-within-bar (1..4)
    // 0x5D..0x5E = 0
    out[0x5F] = device_num;                        // device-number echo
    return PKT_LEN;
}

namespace {
// A real XDJ-700 tempo-master status packet (284 bytes) captured off a live
// link (see docs/architecture.md). Used as a template so the fields we don't
// model stay structurally valid; build_status_packet overwrites the dynamic +
// master fields. Track/metadata bytes carry the template device's state, which
// is harmless for tempo sync.
const uint8_t kStatusTemplate[] = {
    0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c, 0x0a, 0x58,
    0x44, 0x4a, 0x2d, 0x37, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x03, 0x00, 0xf8,
    0x03, 0x00, 0x01, 0x00, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00, 0x08, 0x98,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x85,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x04, 0x04,
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x31, 0x2e, 0x31, 0x33, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x05, 0x00, 0xec, 0xff, 0xfa, 0x00, 0x10, 0x00, 0x00,
    0x80, 0x00, 0x3f, 0x48, 0x7f, 0xff, 0xff, 0xff, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x09, 0x01, 0xff, 0x00, 0x00, 0x01, 0x9a, 0x01, 0xff, 0x02, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x06, 0xc7,
    0x0f, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
}  // namespace

size_t build_status_packet(uint8_t* out, size_t out_len,
                           const char* device_name,
                           uint8_t device_num,
                           float bpm,
                           uint8_t beat_in_bar,
                           bool is_master,
                           uint32_t syncn) {
    const size_t PKT_LEN = sizeof(kStatusTemplate);  // 284 bytes
    if (out_len < PKT_LEN) return 0;
    if (!(bpm > 0.0f))     return 0;
    if (beat_in_bar < 1 || beat_in_bar > 4) beat_in_bar = 1;

    std::memcpy(out, kStatusTemplate, PKT_LEN);
    w32(out + 0x84, syncn);    // master-generation counter (must exceed peers')

    // Device name (0x0B, 20 bytes null-padded).
    std::memset(out + 0x0B, 0, 20);
    for (size_t i = 0; i < 20; ++i) {
        if (!device_name || device_name[i] == '\0') break;
        out[0x0B + i] = static_cast<uint8_t>(device_name[i]);
    }
    out[0x21] = device_num;    // device number
    out[0x24] = device_num;    // device-number echo (subtype area)

    // Flags at 0x89: base 0x84 + playing 0x40 + (master 0x20 when asserting).
    uint8_t flags = 0x84 | 0x40;
    if (is_master) flags |= 0x20;
    out[0x88] = 0x00;          // high byte of state
    out[0x89] = flags;

    w32(out + 0x8C, PITCH_UNITY);                            // Pitch1 = 1.0x
    w16(out + 0x90, MV_REKORDBOX);                           // Mv = 0x8000
    w16(out + 0x92, static_cast<uint16_t>(bpm * 100.0f + 0.5f));  // BPM x100
    w32(out + 0x98, PITCH_UNITY);                            // Pitch2 = 1.0x
    out[0x9E] = is_master ? 0x01 : 0x00;                     // Mm: I am master
    out[0x9F] = 0xFF;                                        // Mh: no handoff
    out[0xA6] = beat_in_bar;                                 // beat-within-bar
    return PKT_LEN;
}

size_t build_sync_control_packet(uint8_t* out, size_t out_len,
                                 const char* device_name,
                                 uint8_t device_num,
                                 uint8_t cmd) {
    constexpr size_t HDR_LEN = 0x1F;                 // magic + type + 20B name
    constexpr size_t PAYLOAD_LEN = 13;
    constexpr size_t PKT_LEN = HDR_LEN + PAYLOAD_LEN;  // 0x2C = 44 bytes
    if (out_len < PKT_LEN) return 0;

    std::memset(out, 0, PKT_LEN);
    std::memcpy(out, MAGIC, sizeof(MAGIC));  // 0x00..0x09
    out[0x0A] = PKT_TYPE_SYNC_CONTROL;       // 0x2A
    for (size_t i = 0; i < 20; ++i) {
        if (!device_name || device_name[i] == '\0') break;
        out[0x0B + i] = static_cast<uint8_t>(device_name[i]);
    }
    // Payload at 0x1F: 01 00 <dev> 00 08 00 00 00 <dev> 00 00 00 <cmd>
    uint8_t* p = out + HDR_LEN;
    p[0x00] = 0x01;
    p[0x02] = device_num;
    p[0x04] = 0x08;
    p[0x08] = device_num;
    p[0x0C] = cmd;
    return PKT_LEN;
}

}  // namespace prolink
