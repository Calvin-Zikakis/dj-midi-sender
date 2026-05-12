"""Replay UDP packets from a pcapng capture at their original inter-packet
timing. Lets you iterate on bridge.py without the XDJ-XZ hooked up.

Usage:
    python replay.py wireshark/xdj-xz-export-mode-pitch-sweep.pcapng

Then in another shell:
    python bridge.py --bind 127.0.0.1
"""
from __future__ import annotations

import argparse
import socket
import sys
import time

from scapy.all import rdpcap
from scapy.layers.inet import IP, UDP

DEFAULT_PORT = 50001


def main() -> None:
    ap = argparse.ArgumentParser(description="Replay UDP packets from a pcapng capture")
    ap.add_argument("capture", help="Path to .pcapng file")
    ap.add_argument("--dest-host", default="127.0.0.1", help="Destination host (default 127.0.0.1)")
    ap.add_argument("--dest-port", type=int, default=DEFAULT_PORT, help="Destination UDP port to send to")
    ap.add_argument(
        "--filter-port",
        type=int,
        default=DEFAULT_PORT,
        help="Only replay packets that were originally destined to this UDP port",
    )
    ap.add_argument("--speed", type=float, default=1.0, help="Playback speed multiplier (default 1.0)")
    ap.add_argument("--loop", action="store_true", help="Loop playback")
    args = ap.parse_args()

    print(f"[replay] reading {args.capture}...", file=sys.stderr)
    packets = rdpcap(args.capture)

    beats: list[tuple[float, bytes]] = []
    for p in packets:
        if not (p.haslayer(IP) and p.haslayer(UDP)):
            continue
        if p[UDP].dport != args.filter_port:
            continue
        payload = bytes(p[UDP].payload)
        if not payload:
            continue
        beats.append((float(p.time), payload))

    if not beats:
        print(f"[replay] no UDP packets to port {args.filter_port} in capture", file=sys.stderr)
        sys.exit(1)

    duration = beats[-1][0] - beats[0][0]
    print(
        f"[replay] {len(beats)} packets, capture span {duration:.2f}s, "
        f"sending to {args.dest_host}:{args.dest_port} at {args.speed}×"
        + (" (looping)" if args.loop else ""),
        file=sys.stderr,
    )

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        while True:
            t0_capture = beats[0][0]
            t0_wall = time.perf_counter()
            for ts, payload in beats:
                target = t0_wall + (ts - t0_capture) / args.speed
                delay = target - time.perf_counter()
                if delay > 0:
                    time.sleep(delay)
                sock.sendto(payload, (args.dest_host, args.dest_port))
            if not args.loop:
                break
    except KeyboardInterrupt:
        print("\n[replay] interrupted", file=sys.stderr)
    finally:
        sock.close()


if __name__ == "__main__":
    main()
