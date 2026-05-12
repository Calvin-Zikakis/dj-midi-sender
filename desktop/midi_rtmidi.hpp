#pragma once

#include "clock.hpp"

#include <memory>
#include <string>
#include <vector>

// Forward declaration so the header doesn't pull <RtMidi.h> into everything.
class RtMidiOut;

namespace desktop {

class MidiRtMidi : public prolink::IMidiOut {
public:
    MidiRtMidi();
    ~MidiRtMidi() override;
    MidiRtMidi(const MidiRtMidi&) = delete;
    MidiRtMidi& operator=(const MidiRtMidi&) = delete;

    // Opens an output port by name (substring match, case-insensitive). Pass
    // an empty string to open the first available port. Returns false if no
    // port matched.
    bool open(const std::string& name_substring);

    // Enumerates available output ports.
    static std::vector<std::string> list_ports();

    const std::string& opened_port_name() const { return opened_name_; }

    void send_byte(uint8_t b) override;

private:
    std::unique_ptr<RtMidiOut> out_;
    std::string opened_name_;
};

}  // namespace desktop
