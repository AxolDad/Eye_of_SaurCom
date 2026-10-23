// WiFi sensing in (dect2/net/wifisense_rx.h): RuView ESP32 CSI frames, one sensor per node, breathing found, and a real UDP round trip.
#include "dect2/net/wifisense_rx.h"
#include "../../core/src/net/netcompat.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

// One ADR-018 frame: 52 subcarriers of one antenna, amplitude amp, int8 I/Q.
static std::vector<uint8_t> frame(uint8_t node, uint32_t seq, double amp) {
    std::vector<uint8_t> b;
    auto le = [&](uint32_t v, int n) { for (int i = 0; i < n; i++) b.push_back((uint8_t)(v >> (8 * i))); };
    le(0xC5110001u, 4);
    b.push_back(node);
    b.push_back(1);
    le(52, 2);
    le(2437, 4);
    le(seq, 4);
    b.push_back((uint8_t)(int8_t)-45);
    b.push_back((uint8_t)(int8_t)-92);
    b.push_back(0);
    b.push_back(0);
    for (int k = 0; k < 52; k++) {
        const double ph = 0.3 + 0.05 * k;
        b.push_back((uint8_t)(int8_t)std::lround(amp * std::cos(ph)));
        b.push_back((uint8_t)(int8_t)std::lround(amp * std::sin(ph)));
    }
    return b;
}

static const WifiSenseListener::Node* find(const std::vector<WifiSenseListener::Node>& v, int id) {
    for (const auto& n : v) if (n.s.node_id == id) return &n;
    return nullptr;
}

int main() {
    WifiSenseListener& L = WifiSenseListener::get();
    CHECK(onair_wifisense_struct_size() == sizeof(OnairWifiSense), "struct layout matches Rust: %zu vs %zu", onair_wifisense_struct_size(), sizeof(OnairWifiSense));
    const int64_t t0 = 1792756800000;
    auto f = frame(3, 1, 60);
    CHECK(L.take(f.data(), f.size(), t0, "10.0.0.3"), "a CSI frame is taken");
    f[0] ^= 1;
    CHECK(!L.take(f.data(), f.size(), t0) && !L.take(nullptr, 0, t0), "junk is not");

    // node 3: someone breathing 18 times a minute for 40 s at 50 frames a second; node 4: an empty room
    for (int i = 0; i < 2000; i++) {
        const double t = i / 50.0;
        auto a = frame(3, 2 + i, 60 + 6 * std::sin(2 * M_PI * 0.3 * t));
        auto b = frame(4, i, 60);
        L.take(a.data(), a.size(), t0 + i * 20);
        L.take(b.data(), b.size(), t0 + i * 20);
    }
    auto v = L.nodes();
    CHECK(v.size() == 2, "two nodes: %zu", v.size());
    if (const auto* n = find(v, 3)) {
        CHECK(n->s.presence && std::fabs(n->s.breathing_bpm - 18) < 2, "node 3 breathing %.1f bpm, presence %d", n->s.breathing_bpm, n->s.presence);
        CHECK(n->s.n_subcarriers == 52 && n->s.freq_mhz == 2437 && n->s.rssi == -45 && n->s.noise == -92 && n->from == "10.0.0.3", "node 3 fields");
        CHECK(n->heardMs == t0 + 1999 * 20 && n->s.frames == 2001, "node 3 last heard, frames %llu", (unsigned long long)n->s.frames);
    } else CHECK(false, "node 3 missing");
    if (const auto* n = find(v, 4)) CHECK(!n->s.presence && n->s.breathing_bpm == 0, "node 4 empty");
    else CHECK(false, "node 4 missing");
    L.clear();
    CHECK(L.nodes().empty(), "cleared");

    // a real socket on the loopback
    std::string err;
    const int port = 48000 + (int)(std::chrono::steady_clock::now().time_since_epoch().count() % 1000);
    CHECK(L.start(port, err) && L.running(), "listening: %s", err.c_str());
    const uint64_t before = L.received();
    netInit();
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons((uint16_t)port); inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    const auto g = frame(9, 1, 50);
    for (int i = 0; i < 50 && L.received() == before; i++) { sockSendTo(s, g.data(), g.size(), to); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    sockClose(s);
    CHECK(L.received() > before, "a datagram arrived");
    v = L.nodes();
    const auto* n9 = find(v, 9);
    CHECK(n9 && n9->from == "127.0.0.1", "node 9 heard over UDP from the loopback");
    L.stop();
    CHECK(!L.running(), "stopped");
    CHECK(!L.start(0, err) && !err.empty(), "bad port refused");

    std::printf(fails ? "wifisense rx tests FAILED (%d)\n" : "wifisense rx tests passed\n", fails);
    return fails ? 1 : 0;
}
