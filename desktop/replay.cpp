// xdj_replay — replay UDP payloads from a pcap/pcapng capture to localhost
// at the original inter-packet timing. Lets xdj_bridge run offline against
// the canonical captures in captures/.
//
// Usage:
//   xdj_replay --file captures/xdj-xz-export-mode-pitch-sweep.pcapng [--loop]
//
// Then in another shell:
//   xdj_bridge --bind 127.0.0.1 --no-vcdj --midi-port "OP-XY"
#include "packets.hpp"
#include "types.hpp"

#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <pcap/pcap.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <chrono>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

struct ReplayPacket {
    double      ts;          // seconds, capture-relative
    uint16_t    dport;       // destination UDP port
    std::vector<uint8_t> payload;
};

struct Args {
    std::string file;
    std::string dest_host = "127.0.0.1";
    float       speed     = 1.0f;
    bool        loop      = false;
    bool        verbose   = false;
    int         filter_port = 0;   // 0 = both 50001 and 50002
};

void print_usage() {
    std::printf(
        "Usage: xdj_replay --file <capture.pcapng> [options]\n"
        "  --file <path>       Capture file (.pcap or .pcapng)\n"
        "  --dest-host <addr>  Destination host (default 127.0.0.1)\n"
        "  --filter-port <n>   Replay only this destination UDP port (default: 50001 AND 50002)\n"
        "  --speed <f>         Playback speed multiplier (default 1.0)\n"
        "  --loop              Loop playback\n"
        "  --verbose           Per-packet progress\n"
        "  -h / --help         This help\n"
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
        if (opt == "--file")             a.file        = next();
        else if (opt == "--dest-host")   a.dest_host   = next();
        else if (opt == "--filter-port") a.filter_port = std::atoi(next().c_str());
        else if (opt == "--speed")       a.speed       = std::atof(next().c_str());
        else if (opt == "--loop")        a.loop        = true;
        else if (opt == "--verbose")     a.verbose     = true;
        else if (opt == "-h" || opt == "--help") { print_usage(); return false; }
        else {
            std::fprintf(stderr, "unknown option: %s\n", opt.c_str());
            std::exit(2);
        }
    }
    if (a.file.empty()) {
        std::fprintf(stderr, "--file is required\n");
        print_usage();
        std::exit(2);
    }
    return true;
}

// Walk Ethernet → IPv4 → UDP headers and return (dport, payload start, payload len)
// if this looks like a UDP packet over IPv4. Returns false otherwise.
bool extract_udp(int dlt, const uint8_t* data, uint32_t len,
                 uint16_t& dport_out,
                 const uint8_t*& payload_out, uint32_t& payload_len_out) {
    uint32_t off = 0;
    if (dlt == DLT_EN10MB) {
        if (len < 14) return false;
        uint16_t ethertype = (uint16_t(data[12]) << 8) | data[13];
        if (ethertype != 0x0800) return false;  // not IPv4
        off = 14;
    } else if (dlt == DLT_NULL || dlt == DLT_LOOP) {
        // BSD loopback: 4-byte AF_ family in host order (NULL) or network (LOOP)
        if (len < 4) return false;
        off = 4;
    } else if (dlt == DLT_RAW) {
        off = 0;
    } else {
        // Common 802.11 / radiotap / linux cooked not handled — these captures
        // are Ethernet on macOS USB-Ethernet. If we hit another DLT, skip the
        // packet rather than abort.
        return false;
    }

    if (len < off + 20) return false;
    uint8_t  vihl    = data[off];
    if ((vihl >> 4) != 4) return false;
    uint32_t ip_hlen = (vihl & 0x0F) * 4;
    uint8_t  proto   = data[off + 9];
    if (proto != 17) return false;  // UDP
    if (len < off + ip_hlen + 8) return false;

    uint32_t udp_off = off + ip_hlen;
    dport_out = (uint16_t(data[udp_off + 2]) << 8) | data[udp_off + 3];
    uint32_t ulen = (uint16_t(data[udp_off + 4]) << 8) | data[udp_off + 5];
    if (ulen < 8) return false;
    payload_out    = data + udp_off + 8;
    payload_len_out = ulen - 8;
    if (udp_off + ulen > len) {
        // Capture was truncated; clamp.
        payload_len_out = len - (udp_off + 8);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse_args(argc, argv, args)) return 0;

    char errbuf[PCAP_ERRBUF_SIZE]{};
    pcap_t* pc = pcap_open_offline(args.file.c_str(), errbuf);
    if (!pc) {
        std::fprintf(stderr, "[replay] failed to open %s: %s\n",
                     args.file.c_str(), errbuf);
        return 1;
    }

    int dlt = pcap_datalink(pc);
    std::fprintf(stderr, "[replay] reading %s (datalink %d)\n",
                 args.file.c_str(), dlt);

    std::vector<ReplayPacket> packets;
    pcap_pkthdr* hdr = nullptr;
    const uint8_t* data = nullptr;
    double t0 = -1.0;
    int read_rc;
    while ((read_rc = pcap_next_ex(pc, &hdr, &data)) >= 0) {
        if (read_rc == 0) continue;  // live capture timeout — not expected offline
        double ts = hdr->ts.tv_sec + hdr->ts.tv_usec / 1e6;
        if (t0 < 0) t0 = ts;

        uint16_t dport = 0;
        const uint8_t* payload = nullptr;
        uint32_t plen = 0;
        if (!extract_udp(dlt, data, hdr->caplen, dport, payload, plen)) continue;
        if (args.filter_port != 0 && dport != args.filter_port) continue;
        if (dport != prolink::PORT_BEAT && dport != prolink::PORT_STATUS) continue;

        ReplayPacket rp;
        rp.ts    = ts - t0;
        rp.dport = dport;
        rp.payload.assign(payload, payload + plen);
        packets.push_back(std::move(rp));
    }
    pcap_close(pc);

    if (packets.empty()) {
        std::fprintf(stderr, "[replay] no matching UDP packets in capture\n");
        return 1;
    }

    int beats = 0, status = 0;
    for (const auto& p : packets) {
        if (p.dport == prolink::PORT_BEAT)   ++beats;
        if (p.dport == prolink::PORT_STATUS) ++status;
    }
    std::fprintf(stderr,
                 "[replay] %zu packets (%d beat, %d status), span %.2fs, sending to %s at %.2f×%s\n",
                 packets.size(), beats, status, packets.back().ts,
                 args.dest_host.c_str(), args.speed,
                 args.loop ? " (looping)" : "");

    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return 1;
    }

    sockaddr_in dest_base{};
    dest_base.sin_family = AF_INET;
    if (::inet_pton(AF_INET, args.dest_host.c_str(), &dest_base.sin_addr) != 1) {
        std::fprintf(stderr, "[replay] bad dest host: %s\n", args.dest_host.c_str());
        return 1;
    }

    using clock = std::chrono::steady_clock;
    do {
        auto wall_start = clock::now();
        for (size_t i = 0; i < packets.size(); ++i) {
            const auto& p = packets[i];
            auto target = wall_start +
                          std::chrono::microseconds(static_cast<int64_t>(p.ts * 1e6 / args.speed));
            auto now = clock::now();
            if (target > now) {
                std::this_thread::sleep_for(target - now);
            }
            sockaddr_in dest = dest_base;
            dest.sin_port = htons(p.dport);
            ::sendto(fd, p.payload.data(), p.payload.size(), 0,
                     reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
            if (args.verbose) {
                std::fprintf(stderr, "[replay] t=%.3fs  port=%u  len=%zu\n",
                             p.ts, p.dport, p.payload.size());
            }
        }
    } while (args.loop);

    ::close(fd);
    return 0;
}
