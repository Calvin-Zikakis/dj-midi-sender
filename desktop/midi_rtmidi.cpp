#include "midi_rtmidi.hpp"

#include <RtMidi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace desktop {

namespace {
bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != haystack.end();
}
}  // namespace

MidiRtMidi::MidiRtMidi() = default;
MidiRtMidi::~MidiRtMidi() = default;

std::vector<std::string> MidiRtMidi::list_ports() {
    std::vector<std::string> out;
    try {
        RtMidiOut probe;
        unsigned n = probe.getPortCount();
        out.reserve(n);
        for (unsigned i = 0; i < n; ++i) {
            out.push_back(probe.getPortName(i));
        }
    } catch (const RtMidiError& e) {
        std::fprintf(stderr, "[midi] enumeration error: %s\n", e.what());
    }
    return out;
}

bool MidiRtMidi::open(const std::string& name_substring) {
    try {
        out_ = std::make_unique<RtMidiOut>();
        unsigned n = out_->getPortCount();
        if (n == 0) {
            std::fprintf(stderr, "[midi] no output ports available\n");
            return false;
        }
        unsigned chosen = n;  // sentinel
        for (unsigned i = 0; i < n; ++i) {
            std::string nm = out_->getPortName(i);
            if (icontains(nm, name_substring)) {
                chosen = i;
                opened_name_ = nm;
                break;
            }
        }
        if (chosen >= n) {
            std::fprintf(stderr, "[midi] no port matched %s\n",
                         name_substring.c_str());
            return false;
        }
        out_->openPort(chosen);
        std::fprintf(stderr, "[midi] opened %s\n", opened_name_.c_str());
        return true;
    } catch (const RtMidiError& e) {
        std::fprintf(stderr, "[midi] open failed: %s\n", e.what());
        return false;
    }
}

void MidiRtMidi::send_byte(uint8_t b) {
    if (!out_) return;
    try {
        // RtMidi expects a complete MIDI message per sendMessage call. Real-time
        // messages (0xF8/0xFA/0xFC) are 1-byte and valid on their own.
        std::vector<unsigned char> msg{b};
        out_->sendMessage(&msg);
    } catch (const RtMidiError& e) {
        std::fprintf(stderr, "[midi] send failed: %s\n", e.what());
    }
}

}  // namespace desktop
