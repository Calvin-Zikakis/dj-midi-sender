// xdj_clockmon — MIDI clock *input* analyzer.
//
// Point the box's DIN (or any MIDI out) at an audio interface's MIDI IN and
// this prints what actually arrives: tick rate → derived BPM, inter-tick
// interval min/avg/max + stddev (jitter), and Start/Stop/Continue events.
// Built to validate the firmware's DIN output quantitatively — an LFO wobble
// tells you "maybe"; this tells you 132.02 BPM ±0.1 ms.
//
//   ./build/desktop/xdj_clockmon --list
//   ./build/desktop/xdj_clockmon --port "Scarlett" [--seconds 30]
//
// Intervals come from RtMidi's delta timestamps (CoreMIDI packet timestamps
// on macOS — driver-level, far tighter than userspace callback timing).

#include "types.hpp"

#include <RtMidi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ClockStats {
    // 4 beats of intervals @ 24 PPQN — long enough to average packet noise,
    // short enough to follow a pitch-fader sweep in the readout.
    static constexpr size_t kWindow = 96;

    std::mutex mu;
    double   since_last_tick_s = -1.0;  // <0 = no reference tick yet
    std::vector<double> window;         // ring of recent intervals (seconds)
    size_t   widx  = 0;
    uint64_t ticks = 0, starts = 0, stops = 0, continues = 0;
};

ClockStats g_stats;
std::atomic<bool> g_run{true};

void on_signal(int) { g_run.store(false); }

void on_midi(double dt, std::vector<unsigned char>* msg, void*) {
    if (!msg || msg->empty()) return;
    const uint8_t b = (*msg)[0];

    std::lock_guard<std::mutex> lock(g_stats.mu);
    // dt is the time since the previous message of any type; accumulate so
    // interleaved non-clock messages don't corrupt tick intervals.
    if (g_stats.since_last_tick_s >= 0.0) g_stats.since_last_tick_s += dt;

    switch (b) {
        case prolink::MIDI_CLOCK: {
            ++g_stats.ticks;
            const double iv = g_stats.since_last_tick_s;
            // Skip the reference tick and anything spanning a gap (>1 s —
            // slower than 2.5 BPM is a stall, not an interval).
            if (iv > 0.0 && iv < 1.0) {
                if (g_stats.window.size() < ClockStats::kWindow) {
                    g_stats.window.push_back(iv);
                } else {
                    g_stats.window[g_stats.widx] = iv;
                    g_stats.widx = (g_stats.widx + 1) % ClockStats::kWindow;
                }
            }
            g_stats.since_last_tick_s = 0.0;
            break;
        }
        case prolink::MIDI_START:
            ++g_stats.starts;
            g_stats.window.clear();
            g_stats.widx = 0;
            g_stats.since_last_tick_s = -1.0;   // fresh run, fresh stats
            std::printf(">>> START\n");
            std::fflush(stdout);
            break;
        case prolink::MIDI_CONTINUE:
            ++g_stats.continues;
            std::printf(">>> CONTINUE\n");
            std::fflush(stdout);
            break;
        case prolink::MIDI_STOP:
            ++g_stats.stops;
            g_stats.since_last_tick_s = -1.0;   // don't measure across a stop
            std::printf(">>> STOP\n");
            std::fflush(stdout);
            break;
        default:
            break;  // notes/CC from other gear on the same IN — ignore
    }
}

void print_stats_line(uint64_t& last_ticks) {
    std::lock_guard<std::mutex> lock(g_stats.mu);
    const uint64_t delta = g_stats.ticks - last_ticks;
    last_ticks = g_stats.ticks;

    if (g_stats.window.empty()) {
        std::printf("[clockmon] ticks=%llu (+%llu/s)  bpm=--       (no clock stream)\n",
                    (unsigned long long)g_stats.ticks, (unsigned long long)delta);
        std::fflush(stdout);
        return;
    }

    double sum = 0.0, mn = 1e9, mx = 0.0;
    for (double v : g_stats.window) {
        sum += v;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    const double avg = sum / g_stats.window.size();
    double var = 0.0;
    for (double v : g_stats.window) var += (v - avg) * (v - avg);
    const double sd  = std::sqrt(var / g_stats.window.size());
    const double bpm = 60.0 / (avg * prolink::TICKS_PER_BEAT);

    std::printf("[clockmon] ticks=%llu (+%llu/s)  bpm=%7.2f  interval avg=%6.3fms  "
                "min=%6.3f  max=%6.3f  sd=%5.3fms\n",
                (unsigned long long)g_stats.ticks, (unsigned long long)delta,
                bpm, avg * 1e3, mn * 1e3, mx * 1e3, sd * 1e3);
    std::fflush(stdout);
}

void print_usage() {
    std::printf(
        "Usage: xdj_clockmon [options]\n"
        "  --list             List MIDI input ports and exit\n"
        "  --port <substr>    Input port name, case-insensitive substring\n"
        "                     (default: the only port, if exactly one exists)\n"
        "  --seconds <n>      Exit after n seconds (default: run until Ctrl+C)\n"
        "  -h / --help        This help\n");
}

bool contains_ci(const std::string& hay, const std::string& needle) {
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != hay.end();
}

}  // namespace

int main(int argc, char** argv) {
    std::string port_substr;
    bool list_only = false;
    int  seconds   = 0;

    for (int i = 1; i < argc; ++i) {
        std::string opt = argv[i];
        if (opt == "--list") list_only = true;
        else if (opt == "--port" && i + 1 < argc)    port_substr = argv[++i];
        else if (opt == "--seconds" && i + 1 < argc) seconds = std::atoi(argv[++i]);
        else if (opt == "-h" || opt == "--help") { print_usage(); return 0; }
        else { std::fprintf(stderr, "unknown option: %s\n", opt.c_str()); print_usage(); return 2; }
    }

    std::unique_ptr<RtMidiIn> in;
    try {
        in = std::make_unique<RtMidiIn>();
    } catch (RtMidiError& e) {
        std::fprintf(stderr, "RtMidi init failed: %s\n", e.getMessage().c_str());
        return 1;
    }

    const unsigned n = in->getPortCount();
    if (list_only) {
        for (unsigned i = 0; i < n; ++i)
            std::printf("%s\n", in->getPortName(i).c_str());
        return 0;
    }
    if (n == 0) {
        std::fprintf(stderr, "no MIDI input ports found\n");
        return 1;
    }

    int chosen = -1;
    if (port_substr.empty()) {
        if (n == 1) {
            chosen = 0;
        } else {
            std::fprintf(stderr, "multiple MIDI inputs — pick one with --port:\n");
            for (unsigned i = 0; i < n; ++i)
                std::fprintf(stderr, "  %s\n", in->getPortName(i).c_str());
            return 2;
        }
    } else {
        for (unsigned i = 0; i < n; ++i) {
            if (contains_ci(in->getPortName(i), port_substr)) { chosen = (int)i; break; }
        }
        if (chosen < 0) {
            std::fprintf(stderr, "no MIDI input matching '%s'; available:\n", port_substr.c_str());
            for (unsigned i = 0; i < n; ++i)
                std::fprintf(stderr, "  %s\n", in->getPortName(i).c_str());
            return 2;
        }
    }

    in->openPort(chosen);
    // RtMidi filters clock (and sysex/sense) by default — unmute timing.
    in->ignoreTypes(/*sysex*/ true, /*time*/ false, /*sense*/ true);
    in->setCallback(&on_midi, nullptr);

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::printf("[clockmon] listening on '%s' — Ctrl+C to quit\n",
                in->getPortName(chosen).c_str());
    std::fflush(stdout);

    uint64_t last_ticks = 0;
    const auto t_end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (g_run.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        print_stats_line(last_ticks);
        if (seconds > 0 && std::chrono::steady_clock::now() >= t_end) break;
    }

    std::lock_guard<std::mutex> lock(g_stats.mu);
    std::printf("[clockmon] summary: ticks=%llu starts=%llu stops=%llu continues=%llu\n",
                (unsigned long long)g_stats.ticks, (unsigned long long)g_stats.starts,
                (unsigned long long)g_stats.stops, (unsigned long long)g_stats.continues);
    return 0;
}
