// Integration test for the POSIX socket layer.
//
// The unit suite drives Bridge through a fake socket, so nothing else covers
// the real implementation — in particular `recv`'s `src_ip` out-parameter,
// which the tempo-master handshake depends on: the takeover request has to be
// unicast back to the exact address the master's status came from.
//
// Only compiled where udp_posix.cpp builds (POSIX). It needs no RtMidi or
// libpcap, so it runs in CI alongside the core tests.

#include "udp_posix.hpp"

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;
int g_checks   = 0;

void check(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s:%d: %s\n", file, line, expr);
    }
}
#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

void section(const char* name) { std::printf("%s\n", name); std::fflush(stdout); }

// A fixed loopback port; high enough to need no privileges.
constexpr uint16_t kTestPort = 45999;
constexpr uint32_t kLoopback = 0x7F000001u;   // 127.0.0.1, host order

void test_reports_the_sender_address() {
    section("udp: recv reports the sender's address in host byte order");
    desktop::UdpPosix rx, tx;
    CHECK(rx.bind("127.0.0.1", kTestPort));
    CHECK(tx.bind("127.0.0.1", 0));

    const uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    CHECK(tx.send(payload, sizeof payload, kLoopback, kTestPort));

    uint8_t  buf[64] = {0};
    uint32_t src     = 0;
    const int n = rx.recv(buf, sizeof buf, 500, &src);
    CHECK(n == static_cast<int>(sizeof payload));
    CHECK(std::memcmp(buf, payload, sizeof payload) == 0);
    // Host order, matching what send() takes — a byte-swapped value here would
    // send the handoff request to a nonexistent address.
    CHECK(src == kLoopback);
}

void test_source_argument_is_optional() {
    section("udp: recv works without the source-address argument");
    desktop::UdpPosix rx, tx;
    CHECK(rx.bind("127.0.0.1", kTestPort + 1));
    CHECK(tx.bind("127.0.0.1", 0));
    const uint8_t payload[2] = {0x01, 0x02};
    CHECK(tx.send(payload, sizeof payload, kLoopback, kTestPort + 1));
    uint8_t buf[16] = {0};
    CHECK(rx.recv(buf, sizeof buf, 500) == static_cast<int>(sizeof payload));
}

void test_timeout_returns_zero_not_an_error() {
    section("udp: an idle socket times out rather than reporting an error");
    desktop::UdpPosix rx;
    CHECK(rx.bind("127.0.0.1", kTestPort + 2));
    uint8_t buf[16];
    // The run loop treats <0 as a socket error and backs off; a timeout must
    // stay 0 or normal idling would look like a fault.
    CHECK(rx.recv(buf, sizeof buf, 20) == 0);
}

void test_bind_failure_is_reported() {
    section("udp: an unusable bind address fails rather than half-working");
    desktop::UdpPosix s;
    CHECK(!s.bind("not-an-ip", kTestPort + 3));
    // 203.0.113.0/24 is TEST-NET-3: never assigned to a local interface.
    desktop::UdpPosix t;
    CHECK(!t.bind("203.0.113.1", kTestPort + 4));
}

}  // namespace

int main() {
    test_reports_the_sender_address();
    test_source_argument_is_optional();
    test_timeout_returns_zero_not_an_error();
    test_bind_failure_is_reported();
    std::printf("\nudp: %d checks, %d failure%s\n", g_checks, g_failures,
                g_failures == 1 ? "" : "s");
    std::printf("%s\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
