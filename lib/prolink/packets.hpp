#pragma once

#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace prolink {

// Beat packet (type 0x28, port 50001). Field offsets verified against
// captures/xdj-xz-export-mode*.pcapng — see docs/architecture.md.
struct BeatPacket {
    uint8_t  device_num     = 0;
    char     device_name[21] = {0};   // 20 bytes + null terminator
    uint32_t ms_next_beat   = 0;
    uint32_t ms_next_bar    = 0;
    uint32_t pitch_raw      = 0;
    uint16_t track_bpm_x100 = 0;
    uint8_t  beat_in_bar    = 0;   // 1..4

    float pitch_multiplier() const {
        return static_cast<float>(pitch_raw) / static_cast<float>(PITCH_UNITY);
    }
    float pitch_percent() const {
        return (pitch_multiplier() - 1.0f) * 100.0f;
    }
    float track_bpm() const {
        return track_bpm_x100 / 100.0f;
    }
    float effective_bpm() const {
        return track_bpm() * pitch_multiplier();
    }
};

// Status packet (type 0x0A, port 50002). Only sent after we announce a
// virtual CDJ on port 50000. Field offsets per deep-symmetry +
// prolink-cpp + python-prodj-link.
struct StatusPacket {
    uint8_t  device_num     = 0;
    char     device_name[21] = {0};
    uint32_t pitch1_raw     = 0;   // effective pitch — USE THIS
    uint32_t pitch2_raw     = 0;   // local fader only — IGNORE
    uint16_t track_bpm_x100 = 0;
    uint8_t  flags          = 0;
    uint16_t mv             = 0;
    uint8_t  beat_in_bar    = 0;
    uint32_t syncn          = 0;   // master-generation counter (0x84); the box
                                   //   must exceed the max seen to take master

    bool is_playing() const { return (flags >> 6) & 1; }
    bool is_master()  const { return (flags >> 5) & 1; }
    bool is_synced()  const { return (flags >> 4) & 1; }
    bool is_on_air()  const { return (flags >> 3) & 1; }
    bool bpm_valid()  const { return mv == MV_REKORDBOX; }

    float pitch_multiplier() const {
        return static_cast<float>(pitch1_raw) / static_cast<float>(PITCH_UNITY);
    }
    float pitch_percent() const {
        return (pitch_multiplier() - 1.0f) * 100.0f;
    }
    float track_bpm() const {
        return track_bpm_x100 / 100.0f;
    }
    float effective_bpm() const {
        return track_bpm() * pitch_multiplier();
    }
};

// Returns nullopt if the buffer is not a well-formed packet of the given type.
std::optional<BeatPacket>   parse_beat_packet(const uint8_t* buf, size_t len);
std::optional<StatusPacket> parse_status_packet(const uint8_t* buf, size_t len);

// Build a virtual-CDJ keep-alive packet (Pro DJ Link type 0x06, 54 bytes).
// `ip_host_order` is host-byte-order uint32 (e.g. 0xC0A80101 for 192.168.1.1).
// Layout matches python-prodj-link / dysentery. `device_num` 5 works with
// XDJ-XZ; CDJ-3000 reserves 1–4 for decks and 5–6 for mixers, but a single-XZ
// network has 5 free.
size_t build_keepalive_packet(uint8_t* out, size_t out_len,
                              const char* device_name,
                              uint8_t device_num,
                              const uint8_t mac[6],
                              uint32_t ip_host_order);

// Build a Pro DJ Link beat packet (type 0x28, port 50001) advertising our own
// beat grid — the foundation of tempo-master mode, where the box becomes the
// tempo authority CDJs sync to. Byte layout mirrors a real XDJ-XZ beat packet
// (captures/xdj-xz-export-mode.pcapng); the six timing prediction fields are
// filled from a formula derived empirically from those captures (see the .cpp).
// We broadcast our own tempo, so pitch is fixed at unity and BPM carries the
// effective tempo. `beat_in_bar` is 1..4. Returns bytes written (96) or 0.
size_t build_beat_packet(uint8_t* out, size_t out_len,
                         const char* device_name,
                         uint8_t device_num,
                         float bpm,
                         uint8_t beat_in_bar);

// Build a Pro DJ Link status packet (type 0x0A, port 50002) for tempo-master
// mode. Built from a real XDJ-700 master status packet captured live (see
// docs/architecture.md): the template's unknown/device fields are kept and the
// dynamic + master fields overwritten. Set `is_master` to assert the master
// role (flags 0x89 bit 5 + Mm 0x9E = 0x01). Returns bytes written (284) or 0.
size_t build_status_packet(uint8_t* out, size_t out_len,
                           const char* device_name,
                           uint8_t device_num,
                           float bpm,
                           uint8_t beat_in_bar,
                           bool is_master,
                           uint32_t syncn);

// Build a Pro DJ Link sync-control command (type 0x2A, port 50001) — the packet
// that requests a tempo-master takeover. Format from Deep Symmetry's beat-link:
// header + 13-byte payload `01 00 <dev> 00 08 00 00 00 <dev> 00 00 00 <cmd>`,
// where cmd is SYNC_CMD_BECOME_MASTER (0x01) to request master. Sent to the
// current master (unicast in beat-link; broadcast also reaches it). Returns
// bytes written (44) or 0.
size_t build_sync_control_packet(uint8_t* out, size_t out_len,
                                 const char* device_name,
                                 uint8_t device_num,
                                 uint8_t cmd);

}  // namespace prolink
