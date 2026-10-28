// StationLinks against a real RemoteApiServer on loopback: events arrive tagged with the site name, a resume after a restart of the server sends
// since = the last seq (no duplicates), a stopped server turns the link to "retrying" with an error, a wrong token gives 401, stop() is quick.
#include "../common/test_util.h"
#include "../../core/src/net/tcp_client.h"
#include "dect2/automation/engine.h"
#include "dect2/net/remote_api.h"
#include "dect2/net/station_link.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#ifndef _WIN32
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#endif
using namespace dect2;
using Clock = std::chrono::steady_clock;

struct TestHost : RemoteApiHost {
    std::mutex mu;
    std::vector<std::string> evs;          // seq i+1 is evs[i]
    uint64_t firstCursor = 0;              // the cursor of the last eventLines call that started a subscription
    bool withPos = true;
    void add(const std::string& type, double time) {
        std::lock_guard<std::mutex> lk(mu);
        evs.push_back("{\"type\":\"" + type + "\",\"time\":" + jsonNum(time) + ",\"mode\":\"ADS-B\",\"freq\":1090000000,\"f\":{\"n\":" + std::to_string(evs.size() + 1) + "}}");
    }
    uint64_t eventHead() override { std::lock_guard<std::mutex> lk(mu); return evs.size(); }
    size_t eventLines(uint64_t& cursor, std::vector<std::string>& lines, size_t max, uint64_t& missed) override {
        missed = 0;
        std::lock_guard<std::mutex> lk(mu);
        if (cursor == ~0ull) { cursor = 0; firstCursor = ~0ull; }   // a test host: "from now on" means from the start
        size_t n = 0;
        while (cursor < evs.size() && n < max) { lines.push_back(evs[cursor]); cursor++; n++; }
        return n;
    }
    std::string statusJson() override { return withPos ? "{\"running\":false,\"lat\":51.5,\"lon\":-0.12}" : "{\"running\":false}"; }
    std::string devicesJson() override { return "[]"; }
    std::string modesJson() override { return "[]"; }
    std::string telemetryJson(uint64_t& seq) override { seq = 1; return "{}"; }
    std::string spectrumJson(int, uint64_t& seq) override { seq = 1; return "{}"; }
    bool start(int, std::string&) override { return true; }
    bool stop(std::string&) override { return true; }
    bool setMode(int, const std::string&, std::string&) override { return true; }
    bool setFrequencyHz(double, std::string&) override { return true; }
    bool setSampleRateHz(double, std::string&) override { return true; }
    bool setGainDb(double, std::string&) override { return true; }
};

#ifndef _WIN32
static std::string lanAddress() {
    ifaddrs* ifs = nullptr;
    std::string r;
    if (getifaddrs(&ifs) != 0) return r;
    for (ifaddrs* a = ifs; a && r.empty(); a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        char b[64];
        inet_ntop(AF_INET, &((sockaddr_in*)a->ifa_addr)->sin_addr, b, sizeof b);
        if (strncmp(b, "169.254.", 8) != 0) r = b;
    }
    freeifaddrs(ifs);
    return r;
}
#else
static std::string lanAddress() { return ""; }
#endif

template <class F> static bool waitFor(F f, int ms) {
    const auto t0 = Clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < ms) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}

int main() {
    std::mutex gotMu;
    std::vector<automation::Event> got;
    const int lid = automation::global().addListener([&](const automation::Event& e) { std::lock_guard<std::mutex> lk(gotMu); got.push_back(e); }, ~0u);
    auto gotCount = [&] { std::lock_guard<std::mutex> lk(gotMu); return got.size(); };

    TestHost host;
    const double now0 = automation::wallNow();
    host.add("adsb.aircraft", now0 - 10);
    host.add("adsb.aircraft", now0 - 10);
    RemoteApiServer srv(host);
    RemoteApiConfig c; c.on = true; c.port = 0;
    CHECK(srv.configure(c) && srv.status().listening, "server starts: %s", srv.status().lastError.c_str());
    const int port = srv.config().port;

    StationLinks links;
    SiteConfig sc; sc.name = "test"; sc.host = "127.0.0.1"; sc.port = port;
    {   // an off site stays off, and the empty list is fine
        links.configure({});
        CHECK(links.status().empty(), "no sites");
        SiteConfig off = sc; off.name = "off"; off.on = false;
        links.configure({off});
        auto st = links.status();
        CHECK(st.size() == 1 && st[0].second.state == 0, "an off site has state 0");
    }
    links.configure({sc});
    CHECK(waitFor([&] { return gotCount() >= 2; }, 5000), "two events arrive (got %zu)", gotCount());
    {
        std::lock_guard<std::mutex> lk(gotMu);
        for (size_t i = 0; i < got.size() && i < 2; i++) {
            CHECK(got[i].type == "adsb.aircraft" && got[i].f.count("site") && got[i].f["site"].s == "test", "event %zu tagged with the site", i);
            CHECK(got[i].f.count("n") && got[i].f["n"].n == (double)(i + 1), "event %zu keeps its fields and order", i);
        }
    }
    auto st = links.status();
    CHECK(st.size() == 1 && st[0].second.state == 2, "live (state %d, %s)", st.empty() ? -1 : st[0].second.state, st.empty() ? "" : st[0].second.lastError.c_str());
    CHECK(st[0].second.events == 2 && st[0].second.missed == 0, "2 events counted (%llu)", (unsigned long long)st[0].second.events);
    CHECK(std::fabs(st[0].second.lat - 51.5) < 1e-9 && std::fabs(st[0].second.lon + 0.12) < 1e-9, "position from /api/v1/status");
    CHECK(std::fabs(st[0].second.clockSkewSec - 10) < 3, "clock skew about 10 s (%f)", st[0].second.clockSkewSec);
    CHECK(st[0].second.lastEventAt > now0, "lastEventAt set");

    // a live event after the subscription
    host.add("adsb.aircraft", automation::wallNow());
    CHECK(waitFor([&] { return gotCount() >= 3; }, 5000), "a live event arrives (got %zu)", gotCount());

    // the server goes away: retrying with an error
    c.on = false;
    srv.configure(c);
    CHECK(waitFor([&] { auto s = links.status(); return !s.empty() && s[0].second.state == 3 && !s[0].second.lastError.empty(); }, 5000), "state 3 with an error after the server stops");

    // the server comes back on the same port with one more event: the link resumes with since = 3, so exactly one new event
    host.add("adsb.aircraft", automation::wallNow());
    { std::lock_guard<std::mutex> lk(host.mu); host.firstCursor = 1; }   // the fake sets ~0 when a subscription comes without since
    c.on = true; c.port = port;
    bool back = srv.configure(c);
    CHECK(back, "server restarts on the same port: %s", srv.status().lastError.c_str());
    if (back) {
        CHECK(waitFor([&] { return gotCount() >= 4; }, 8000), "event 4 arrives after the reconnect (got %zu)", gotCount());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(gotCount() == 4, "no duplicates after the resume (got %zu)", gotCount());
        std::lock_guard<std::mutex> lk(host.mu);
        CHECK(host.firstCursor != ~0ull, "the reconnect sent since (cursor was ~0)");
        auto s2 = links.status();
        CHECK(s2[0].second.state == 2 && s2[0].second.events == 4, "live again, 4 events (%d, %llu)", s2[0].second.state, (unsigned long long)s2[0].second.events);
    }

    // stop() is quick
    {
        const auto t0 = Clock::now();
        links.stop();
        const long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        CHECK(ms < 2500, "stop took %ld ms", ms);
        CHECK(links.status().empty() || links.status()[0].second.state == 0, "state 0 after stop");
    }

    // configure() never waits for a thread stuck in connect(): a non-routable address hangs until the connect timeout
    {
        StationLinks l3;
        SiteConfig hang; hang.name = "hang"; hang.host = "10.255.255.1"; hang.port = 9;
        l3.configure({hang});
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto t0 = Clock::now();
        l3.configure({});
        const long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        CHECK(ms < 200, "configure({}) of a hanging site returned in %ld ms", ms);
        CHECK(l3.status().empty(), "the removed site is gone from the status");
        l3.stop();   // joins the parked thread
    }

    // a wrong token: a non-loopback peer is judged by the token, so go through this computer's own LAN address with the server on lan
    {
        const std::string lan = lanAddress();
        if (lan.empty()) printf("SKIP: no non-loopback address, the 401 case was not exercised\n");
        else {
            RemoteApiServer srv2(host);
            RemoteApiConfig c2; c2.on = true; c2.port = 0; c2.lan = true; c2.token = "0123456789abcdef0123";
            CHECK(srv2.configure(c2) && srv2.status().listening, "lan server starts: %s", srv2.status().lastError.c_str());
            const int p2 = srv2.config().port;
            StationLinks l2;
            SiteConfig bad; bad.name = "bad"; bad.host = lan; bad.port = p2; bad.token = "wrong-token-0123456789";
            l2.configure({bad});
            CHECK(waitFor([&] { auto s = l2.status(); return !s.empty() && s[0].second.state == 3 && s[0].second.lastError.find("401") != std::string::npos; }, 5000),
                  "wrong token: state 3 and 401 in the error (%s)", l2.status().empty() ? "" : l2.status()[0].second.lastError.c_str());
            // the right token works
            const size_t before = gotCount();
            SiteConfig ok = bad; ok.name = "lan"; ok.token = c2.token;
            l2.configure({ok});
            CHECK(waitFor([&] { auto s = l2.status(); return !s.empty() && s[0].second.state == 2; }, 5000), "right token: live");
            CHECK(waitFor([&] { return gotCount() >= before + 4; }, 5000), "the events arrive over the LAN address");
            l2.stop();
        }
    }
    automation::global().removeListener(lid);
    return testSummary("station_link");
}
