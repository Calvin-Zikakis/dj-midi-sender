// xdj_bridge — Phase 1 desktop binary.
//
// Listens on UDP 50001 (beat) and 50002 (status), announces a virtual CDJ on
// UDP 50000 every 1500 ms, and emits 24 PPQN MIDI clock out the chosen MIDI
// port. With --visualize, opens a Dear ImGui debug panel for live offset
// tuning. See docs/architecture.md for protocol + clock design,
// docs/phases.md for what counts as Phase 1 "done".
#include "bridge.hpp"
#include "clock.hpp"
#include "config_posix.hpp"
#include "iface_posix.hpp"
#include "midi_rtmidi.hpp"
#include "timer_posix.hpp"
#include "udp_posix.hpp"

#ifdef XDJ_HAVE_GUI
#include "gui_imgui.hpp"
#endif

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

namespace {

struct Args {
    std::string bind_addr      = "0.0.0.0";
    std::string interface;
    std::string midi_port;
    std::string device_name    = "xdj-bridge";
    uint8_t     device_num     = 5;
    uint32_t    gain           = 32;
    float       fallback_bpm   = 120.0f;
    std::optional<float> clock_offset_ms;   // unset = load from config
    float       grid_offset_ms = 0.0f;
    bool        no_vcdj        = false;
    bool        list_midi      = false;
    bool        verbose        = false;
    bool        visualize      = false;
};

void print_usage() {
    std::printf(
        "Usage: xdj_bridge [options]\n"
        "  --bind <addr>          UDP bind address (default 0.0.0.0; use 127.0.0.1 with replay)\n"
        "  --interface <name>     Network interface for the virtual-CDJ announce (default: auto)\n"
        "  --midi-port <substr>   MIDI output port name, case-insensitive substring (default: first)\n"
        "  --device-name <s>      Virtual CDJ device name (default: xdj-bridge)\n"
        "  --device-num <n>       Virtual CDJ device number (default: 7)\n"
        "  --gain <n>             PLL phase correction gain divisor (default: 16)\n"
        "  --bpm <float>          Fallback BPM if no valid signal (default: 120.0)\n"
        "  --clock-offset-ms <f>  Lead the beat by N ms (physical chain latency).\n"
        "                         If omitted, loads the persisted value for the chosen MIDI port.\n"
        "                         In --visualize mode, tune live with [ ] and Shift+[ ].\n"
        "  --grid-offset-ms <f>   Per-track beat-grid offset in ms (session only, not persisted).\n"
        "                         In --visualize mode, tune live with ←/→ and ↑/↓.\n"
        "  --no-vcdj              Skip the virtual-CDJ announce (beat packets only)\n"
        "  --list-midi            List MIDI output ports and exit\n"
        "  --visualize            Open the ImGui debug panel\n"
        "  --verbose              Per-packet debug output\n"
        "  -h / --help            This help\n"
    );
}

bool parse_args(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        std::string opt = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", opt.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (opt == "--bind")                  a.bind_addr       = next();
        else if (opt == "--interface")        a.interface       = next();
        else if (opt == "--midi-port")        a.midi_port       = next();
        else if (opt == "--device-name")      a.device_name     = next();
        else if (opt == "--device-num")       a.device_num      = static_cast<uint8_t>(std::atoi(next().c_str()));
        else if (opt == "--gain")             a.gain            = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (opt == "--bpm")              a.fallback_bpm    = std::atof(next().c_str());
        else if (opt == "--clock-offset-ms")  a.clock_offset_ms = std::atof(next().c_str());
        else if (opt == "--grid-offset-ms")   a.grid_offset_ms  = std::atof(next().c_str());
        else if (opt == "--no-vcdj")          a.no_vcdj         = true;
        else if (opt == "--list-midi")        a.list_midi       = true;
        else if (opt == "--visualize")        a.visualize       = true;
        else if (opt == "--verbose")          a.verbose         = true;
        else if (opt == "-h" || opt == "--help") { print_usage(); return false; }
        else {
            std::fprintf(stderr, "unknown option: %s\n", opt.c_str());
            print_usage();
            std::exit(2);
        }
    }
    return true;
}

std::atomic<prolink::Bridge*> g_bridge{nullptr};

void on_signal(int) {
    auto* b = g_bridge.load();
    if (b) b->stop();
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 0;

    if (args.list_midi) {
        for (const auto& name : desktop::MidiRtMidi::list_ports()) {
            std::printf("%s\n", name.c_str());
        }
        return 0;
    }

    desktop::UdpPosix beat_sock;
    if (!beat_sock.bind(args.bind_addr.c_str(), prolink::PORT_BEAT)) return 1;
    desktop::UdpPosix status_sock;
    if (!status_sock.bind(args.bind_addr.c_str(), prolink::PORT_STATUS)) return 1;

    desktop::MidiRtMidi midi;
    if (!midi.open(args.midi_port)) return 1;
    std::string midi_port_name = midi.opened_port_name();

    desktop::TimerPosix timer;
    prolink::Clock clock(midi, timer, args.gain);

    prolink::BridgeConfig cfg;
    cfg.device_num = args.device_num;
    std::strncpy(cfg.device_name, args.device_name.c_str(),
                 sizeof(cfg.device_name) - 1);
    cfg.device_name[sizeof(cfg.device_name) - 1] = '\0';
    cfg.send_vcdj_announce = !args.no_vcdj;
    cfg.verbose            = args.verbose;
    cfg.fallback_bpm       = args.fallback_bpm;

    // Keepalive socket must be bound to the interface IP (not 0.0.0.0) so
    // macOS routes broadcast sends to the correct interface consistently.
    // Discover the interface first so we have the IP before binding.
    desktop::UdpPosix keepalive_sock;
    if (cfg.send_vcdj_announce) {
        auto iface = desktop::find_interface(args.interface);
        if (!iface) {
            std::fprintf(stderr,
                "[main] no usable network interface found (looking for %s).\n"
                "       use --no-vcdj for replay/loopback testing, or pass\n"
                "       --interface en0 (etc.) explicitly.\n",
                args.interface.empty() ? "auto" : args.interface.c_str());
            return 1;
        }
        std::memcpy(cfg.mac, iface->mac, 6);
        cfg.local_ip     = iface->ipv4;
        cfg.broadcast_ip = iface->broadcast;
        std::fprintf(stderr,
                     "[main] iface %s  mac %02x:%02x:%02x:%02x:%02x:%02x  ip %s  bcast %s\n",
                     iface->name.c_str(),
                     iface->mac[0], iface->mac[1], iface->mac[2],
                     iface->mac[3], iface->mac[4], iface->mac[5],
                     desktop::format_ipv4(iface->ipv4).c_str(),
                     desktop::format_ipv4(iface->broadcast).c_str());
        // Bind to the interface IP so the OS always routes through en<N>.
        if (!keepalive_sock.bind(desktop::format_ipv4(iface->ipv4).c_str(), 0)) return 1;
    } else {
        std::fprintf(stderr, "[main] virtual-CDJ announce disabled (--no-vcdj)\n");
        if (!keepalive_sock.bind("0.0.0.0", 0)) return 1;
    }
    keepalive_sock.enable_broadcast();

    // Resolve the persisted clock offset for this port unless overridden.
    float initial_clock_ms;
    if (args.clock_offset_ms.has_value()) {
        initial_clock_ms = *args.clock_offset_ms;
    } else {
        initial_clock_ms = desktop::load_clock_offset_ms(midi_port_name, 0.0f);
        if (initial_clock_ms != 0.0f) {
            std::fprintf(stderr, "[config] loaded clock offset %+.1f ms for %s\n",
                         initial_clock_ms, midi_port_name.c_str());
        }
    }

    // Forward declare the GUI pointer so the on_beat/on_status callbacks can
    // see it. Owned later by the visualize branch.
#ifdef XDJ_HAVE_GUI
    desktop::GuiVisualizer* gui_ptr = nullptr;
#endif

    prolink::BridgeCallbacks bridge_cb;
#ifdef XDJ_HAVE_GUI
    bridge_cb.on_beat = [&](const prolink::BeatPacket& pkt) {
        if (gui_ptr) gui_ptr->on_beat(pkt);
    };
    bridge_cb.on_status = [&](const prolink::StatusPacket& pkt) {
        if (gui_ptr) gui_ptr->on_status(pkt);
    };
#endif

    prolink::Bridge bridge(beat_sock, status_sock, keepalive_sock, clock, cfg, bridge_cb);
    bridge.set_clock_offset_ms(initial_clock_ms);
    bridge.set_grid_offset_ms(args.grid_offset_ms);
    g_bridge.store(&bridge);
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    if (args.visualize) {
#ifdef XDJ_HAVE_GUI
        desktop::GuiVisualizer::OffsetActions actions;
        actions.adjust_clock_ms = [&](float d) {
            bridge.adjust_clock_offset_ms(d);
            desktop::save_clock_offset_ms(midi_port_name, bridge.clock_offset_ms());
        };
        actions.adjust_grid_ms = [&](float d) { bridge.adjust_grid_offset_ms(d); };
        actions.reset_grid     = [&]() { bridge.reset_grid_offset(); };
        actions.get_clock_ms   = [&]() { return bridge.clock_offset_ms(); };
        actions.get_grid_ms    = [&]() { return bridge.grid_offset_ms(); };

        desktop::GuiVisualizer gui(bridge, clock, actions, midi_port_name);
        if (!gui.init()) {
            std::fprintf(stderr, "[main] GUI failed to open — running headless\n");
            std::fprintf(stderr,
                         "[main] listening on %s:%u (beat) and :%u (status) — Ctrl+C to quit\n",
                         args.bind_addr.c_str(), prolink::PORT_BEAT, prolink::PORT_STATUS);
            bridge.run();
            return 0;
        }
        gui_ptr = &gui;

        std::thread worker([&]() { bridge.run(); });
        gui.run();           // blocks until window closes
        bridge.stop();
        worker.join();
        return 0;
#else
        std::fprintf(stderr,
            "[main] this build has no GUI (XDJ_GUI=OFF or GLFW missing).\n"
            "       `brew install glfw` and reconfigure to enable --visualize.\n");
        return 1;
#endif
    }

    std::fprintf(stderr,
                 "[main] listening on %s:%u (beat) and :%u (status) — Ctrl+C to quit\n",
                 args.bind_addr.c_str(), prolink::PORT_BEAT, prolink::PORT_STATUS);
    bridge.run();
    return 0;
}
