#pragma once

#include <cstdint>

namespace prolink {

inline constexpr uint8_t MAGIC[10] =
    {0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c};  // "Qspt1WmJOL"

inline constexpr uint8_t PKT_TYPE_KEEPALIVE = 0x06;
inline constexpr uint8_t PKT_TYPE_STATUS    = 0x0A;
inline constexpr uint8_t PKT_TYPE_BEAT      = 0x28;

inline constexpr uint16_t PORT_KEEPALIVE = 50000;
inline constexpr uint16_t PORT_BEAT      = 50001;
inline constexpr uint16_t PORT_STATUS    = 50002;

// 0x00100000 represents pitch multiplier 1.0× (resting).
inline constexpr uint32_t PITCH_UNITY = 0x00100000u;

// Status packet `Mv` field — only this value means "rekordbox-analyzed
// track loaded, BPM is trustworthy".
inline constexpr uint16_t MV_REKORDBOX = 0x8000;

// 24 PPQN — one MIDI clock tick is 1/24 of a beat.
inline constexpr uint8_t TICKS_PER_BEAT = 24;

inline constexpr uint8_t MIDI_CLOCK    = 0xF8;
inline constexpr uint8_t MIDI_START    = 0xFA;
inline constexpr uint8_t MIDI_CONTINUE = 0xFB;
inline constexpr uint8_t MIDI_STOP     = 0xFC;

}  // namespace prolink
