// Cursor on Target in (dect2/net/cot_rx.h): contacts kept by uid until stale, the map markers they become, and a real UDP round trip.
#include "dect2/net/cot_rx.h"
#include "../../core/src/net/netcompat.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static std::string event(const char* uid, const char* call, double lat, const char* stale) {
    char b[1024];
    std::snprintf(b, sizeof b, "<event version='2.0' uid='%s' type='a-f-G-U-C' time='2026-10-23T12:00:00Z' start='2026-10-23T12:00:00Z' stale='%s' how='h-e'>"
                  "<point lat='%.4f' lon='-77.0365' hae='17.5' ce='9999999.0' le='9999999.0'/><detail><contact callsign='%s'/></detail></event>", uid, stale, lat, call);
    return b;
}
static const uint8_t* u8(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

int main() {
    CotListener& L = CotListener::get();
    const int64_t t0 = 1792756800000;   // 2026-10-23T12:00:00Z
    const std::string a = event("ANDROID-1", "ZOR-1", 38.8977, "2026-10-23T12:06:00Z");
    CHECK(L.take(u8(a), a.size(), t0), "an ATAK event is taken");
    CHECK(!L.take(u8(std::string("not xml")), 7, t0) && !L.take(nullptr, 0, t0), "junk is not");

    auto m = L.markers(t0 + 1000);
    CHECK(m.size() == 1, "one contact: %zu", m.size());
    if (!m.empty()) {
        CHECK(m[0].layer == LayerCot && m[0].id == "ANDROID-1" && m[0].label == "ZOR-1" && m[0].detail == "a-f-G-U-C", "marker fields");
        CHECK(std::fabs(m[0].lat - 38.8977) < 1e-9 && m[0].hasAlt && std::fabs(m[0].altM - 17.5) < 1e-9 && std::fabs(m[0].ageSec - 1.0) < 1e-9, "position, altitude, age");
    }
    // the same uid again: replaced, not added
    const std::string a2 = event("ANDROID-1", "ZOR-1", 38.9, "2026-10-23T12:06:00Z");
    L.take(u8(a2), a2.size(), t0 + 2000);
    m = L.markers(t0 + 2000);
    CHECK(m.size() == 1 && std::fabs(m[0].lat - 38.9) < 1e-9, "updated in place");
    CHECK(L.markers(t0 + 6 * 60 * 1000).empty(), "gone after its stale time");

    // a real socket: unicast on the loopback
    std::string err;
    const int port = 47000 + (int)(std::chrono::steady_clock::now().time_since_epoch().count() % 1000);
    CHECK(L.start("", port, err) && L.running(), "listening: %s", err.c_str());
    const uint64_t before = L.received();
    netInit();
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons((uint16_t)port); inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    const std::string b = event("WINTAK-7", "ZOR-7", 40.0, "2099-01-01T00:00:00Z");
    for (int i = 0; i < 50 && L.received() == before; i++) { sockSendTo(s, b.data(), b.size(), to); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    sockClose(s);
    CHECK(L.received() > before, "a datagram arrived");
    bool found = false;
    for (const MapMarker& k : L.markers(t0)) found |= k.id == "WINTAK-7" && k.label == "ZOR-7";
    CHECK(found, "the UDP contact is on the map");
    L.stop();
    CHECK(!L.running(), "stopped");
    CHECK(!L.start("", 0, err) && !err.empty(), "bad port refused");

    std::printf(fails ? "cot rx tests FAILED (%d)\n" : "cot rx tests passed\n", fails);
    return fails ? 1 : 0;
}
