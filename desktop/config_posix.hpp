#pragma once

#include <string>

namespace desktop {

// Persisted MIDI clock-lead calibration, keyed by output port name.
// Only stores the physical-chain offset, which is constant per output device.
// Per-track beat-grid offsets are session state and never persisted.
//
// File format (~/.config/dj-midi-sender.json):
//   {
//     "clock_offsets_ms": {
//       "OP-XY": 25.0,
//       "Sub 27 USB MIDI": 11.5
//     }
//   }

// Path to the config file (~/.config/dj-midi-sender.json — created on first save).
std::string config_path();

// Returns 0.0 if the port has no saved value or the file is missing/corrupt.
float load_clock_offset_ms(const std::string& port_name, float default_ms = 0.0f);

// Atomic write (temp file + rename). Returns false on disk error.
bool save_clock_offset_ms(const std::string& port_name, float offset_ms);

}  // namespace desktop
