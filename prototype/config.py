"""Persisted MIDI clock-lead calibration, keyed by output port name.

Only stores the physical-chain offset (USB transit + slave processing),
which is constant per output device. Per-track beat-grid offsets are
session state and not persisted here.
"""
from __future__ import annotations

import json
import os
from typing import Dict

CONFIG_PATH = os.path.expanduser("~/.config/dj-midi-sender.json")


def _load_all() -> Dict[str, float]:
    try:
        with open(CONFIG_PATH) as f:
            data = json.load(f)
        offsets = data.get("clock_offsets_ms", {})
        return {str(k): float(v) for k, v in offsets.items()}
    except (FileNotFoundError, json.JSONDecodeError, ValueError, TypeError):
        return {}


def get_clock_offset_ms(port_name: str, default: float = 0.0) -> float:
    return _load_all().get(port_name, default)


def save_clock_offset_ms(port_name: str, offset_ms: float) -> None:
    offsets = _load_all()
    offsets[port_name] = float(offset_ms)
    os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
    tmp = CONFIG_PATH + ".tmp"
    with open(tmp, "w") as f:
        json.dump({"clock_offsets_ms": offsets}, f, indent=2, sort_keys=True)
    os.replace(tmp, CONFIG_PATH)
