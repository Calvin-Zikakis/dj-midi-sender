"""Pro DJ Link beat packet (type 0x28) parser.

Field offsets are documented in claude/project_handoff_doc.md and were
verified empirically against the captures in wireshark/.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = b"\x51\x73\x70\x74\x31\x57\x6d\x4a\x4f\x4c"
BEAT_PACKET_TYPE = 0x28

# 0x00100000 represents pitch multiplier 1.0× (resting).
PITCH_UNITY = 0x00100000


@dataclass
class BeatPacket:
    device: str
    device_num: int
    track_bpm: float
    pitch_raw: int
    pitch_multiplier: float
    pitch_percent: float
    effective_bpm: float
    beat_in_bar: int  # 1..4
    ms_next_beat: int
    ms_next_bar: int


def parse_beat_packet(payload: bytes) -> BeatPacket | None:
    """Decode a single Pro DJ Link beat packet payload.

    Returns None for any UDP payload that isn't a well-formed beat packet,
    so callers can sieve packets cheaply from a shared socket.
    """
    if not payload.startswith(MAGIC):
        return None
    if payload[10] != BEAT_PACKET_TYPE:
        return None
    if len(payload) < 0x60:
        return None

    device = payload[11:31].rstrip(b"\x00").decode("ascii", errors="replace")
    device_num = payload[0x21]
    ms_next_beat = struct.unpack(">I", payload[0x24:0x28])[0]
    ms_next_bar = struct.unpack(">I", payload[0x2C:0x30])[0]
    pitch_raw = struct.unpack(">I", payload[0x54:0x58])[0]
    track_bpm = struct.unpack(">H", payload[0x5A:0x5C])[0] / 100.0
    beat_in_bar = payload[0x5C]

    pitch_multiplier = pitch_raw / PITCH_UNITY
    effective_bpm = track_bpm * pitch_multiplier

    return BeatPacket(
        device=device,
        device_num=device_num,
        track_bpm=track_bpm,
        pitch_raw=pitch_raw,
        pitch_multiplier=pitch_multiplier,
        pitch_percent=(pitch_multiplier - 1.0) * 100.0,
        effective_bpm=effective_bpm,
        beat_in_bar=beat_in_bar,
        ms_next_beat=ms_next_beat,
        ms_next_bar=ms_next_bar,
    )
