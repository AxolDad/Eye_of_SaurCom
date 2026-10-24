// The Aaronia source (source_aaronia.cpp) against a fake RTSA-Suite HTTP Server block on localhost: the stream request and its format,
// the PUT /control requests that start the graph and set the frequency, chunked packets turning into samples, the rate taken from the
// span, the dropped-sample gap the server's timestamps show, a server that wants a password, a graph serving spectra instead of IQ, and
// a server that is not there. No Aaronia hardware or software was used: the fake does what Aaronia's endpoint document describes.
#include "../../core/src/net/tcp_client.h"
#include "../../core/src/source/aaronia_proto.h"
#include "../../core/src/source/native_common.h"
#include "dect2/net/netradio.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

template <class F> static bool eventually(F f, int ms = 5000) {
    for (int i = 0; i < ms / 10; i++) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return f();
}

static std::string f32le(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    std::string s(4, '\0');
    s[0] = (char)u; s[1] = (char)(u >> 8); s[2] = (char)(u >> 16); s[3] = (char)(u >> 24);
    return s;
}

// One packet as the HTTP Server block sends it, then wrapped as an HTTP chunk.
static std::string packet(int pairs, double t0, double rate, double centre, float base, const char* payload = "iq") {
    const double dur = pairs / rate, span = rate;
    char hdr[400];
    snprintf(hdr, sizeof hdr,
             "{\"startTime\":%.9f,\"endTime\":%.9f,\"unit\":\"generic\",\"payload\":\"%s\",\"minPower\":-2,\"maxPower\":2,"
             "\"startFrequency\":%.1f,\"endFrequency\":%.1f,\"sampleDepth\":1,\"sampleSize\":2,\"samples\":%d}",
             t0, t0 + dur, payload, centre - span / 2, centre + span / 2, pairs);
    std::string p(hdr);
    p += (char)aaronia::kLineFeed;
    p += (char)aaronia::kRecordSeparator;
    for (int i = 0; i < pairs; i++) { p += f32le(base + (float)i); p += f32le(-(base + (float)i)); }
    return p;
}
static std::string chunk(const std::string& body) {
    char n[32];
    snprintf(n, sizeof n, "%zX\r\n", body.size());
    return std::string(n) + body + "\r\n";
}

struct Fake {
    enum Mode { Normal, NeedAuth, Spectra, NotFound } mode = Normal;
    sock_t ls = kBadSock;
    int port = 0;
    std::thread th;
    std::atomic<bool> run{true};
    std::atomic<int> connections{0}, gapNow{0};
    std::mutex mu;
    std::vector<std::string> requests;     // the request line plus the body of every request
    double rate = 48000, centre = 100e6, t = 1000.0;
    float base = 1.f;

    bool saw(const std::string& needle) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& r : requests) if (r.find(needle) != std::string::npos) return true;
        return false;
    }
    int count(const std::string& needle) {
        std::lock_guard<std::mutex> lk(mu);
        int n = 0;
        for (auto& r : requests) n += r.find(needle) != std::string::npos;
        return n;
    }

    bool start() {
        netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 8) != 0) return false;
        socklen_t len = sizeof a;
        getsockname(ls, (sockaddr*)&a, &len);
        port = ntohs(a.sin_port);
        sockSetNonBlocking(ls, true);
        th = std::thread([this] { loop(); });
        return true;
    }
    void stop() { run = false; if (th.joinable()) th.join(); if (ls != kBadSock) sockClose(ls); }

    void loop() {
        std::vector<std::thread> conns;
        while (run) {
            sock_t c = accept(ls, nullptr, nullptr);
            if (c == kBadSock) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
            sockSetNonBlocking(c, false);
            connections++;
            conns.emplace_back([this, c] { serve(c); sockShutdown(c); sockClose(c); });
        }
        for (auto& t2 : conns) if (t2.joinable()) t2.join();
    }

    void serve(sock_t c) {
        sockRecvTimeout(c, 50);
        std::string head;
        while (head.find("\r\n\r\n") == std::string::npos && head.size() < 8192 && run) {
            char ch;
            if (sockRecv(c, &ch, 1) <= 0) { if (!run) return; continue; }
            head += ch;
        }
        if (!run) return;
        const std::string line = head.substr(0, head.find("\r\n"));
        // read the body when the request has one
        std::string body;
        const size_t cl = head.find("Content-Length: ");
        if (cl != std::string::npos) {
            const size_t want = (size_t)atoi(head.c_str() + cl + 16);
            body.resize(want);
            for (size_t got = 0; got < want;) {
                const long long k = sockRecv(c, &body[got], want - got);
                if (k <= 0) break;
                got += (size_t)k;
            }
        }
        { std::lock_guard<std::mutex> lk(mu); requests.push_back(line + " | " + body + " | " + (head.find("Authorization:") != std::string::npos ? "auth" : "noauth")); }
        if (mode == NeedAuth && head.find("Authorization: Basic ") == std::string::npos) {
            const char* r = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n";
            sockSendAll(c, r, strlen(r));
            return;
        }
        if (line.compare(0, 4, "PUT ") == 0) {
            const char* r = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
            sockSendAll(c, r, strlen(r));
            return;
        }
        if (mode == NotFound || line.find("/stream") == std::string::npos) {
            const char* r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            sockSendAll(c, r, strlen(r));
            return;
        }
        const char* ok = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\n\r\n";
        if (!sockSendAll(c, ok, strlen(ok))) return;
        const int pairs = 512;
        double now = t;
        while (run) {
            if (gapNow.exchange(0)) now += 1.0;              // a second of samples thrown away, as an overfull buffer would
            const std::string p = packet(pairs, now, rate, centre, base, mode == Spectra ? "spectra" : "iq");
            now += pairs / rate;
            if (!sockSendAll(c, chunk(p).data(), chunk(p).size())) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

static size_t drain(IqRing& ring, std::vector<cf32>& out, int ms = 2000) {
    std::vector<cf32> buf(4096);
    size_t total = 0;
    for (int i = 0; i < ms / 10; i++) {
        const size_t n = ring.read(buf.data(), buf.size());
        out.insert(out.end(), buf.begin(), buf.begin() + (ptrdiff_t)n);
        total += n;
        if (total > 8192) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return total;
}

int main() {
    netInit();
    // the device entry and the radio list text
    {
        const DeviceInfo d = native::aaroniaDevice("10.0.0.7", 0, "");
        CHECK(d.kind == DeviceInfo::Native && d.board == "aaronia", "kind");
        CHECK(d.nativeArgs == "10.0.0.7:54664", "the default port: %s", d.nativeArgs.c_str());
        CHECK(d.name.find("Aaronia RTSA-Suite") != std::string::npos && d.name.find("Spectran") != std::string::npos, "name: %s", d.name.c_str());
        CHECK(d.maxRateHz > 1e6 && d.minRateHz > 0 && !d.settings.empty(), "caps and settings");
        bool fmt = false, tune = false;
        for (const auto& s : d.settings) { fmt |= s.key == "format"; tune |= s.key == "tune"; }
        CHECK(fmt && tune, "the format and tune settings are offered");
        CHECK(native::aaroniaDevice("::1", 1234, "").nativeArgs == "[::1]:1234", "an IPv6 address is bracketed");
        // the type is in the network radio list
        CHECK(std::string(netRadioTypeName(NetRadio::Aaronia)).find("Aaronia") != std::string::npos, "type name: %s", netRadioTypeName(NetRadio::Aaronia));
        CHECK(netRadioDefaultPort(NetRadio::Aaronia) == 54664, "default port %d", netRadioDefaultPort(NetRadio::Aaronia));
        std::vector<NetRadio> v = {{NetRadio::Aaronia, "box", 54664, "user:pw"}};
        const std::string text = netRadiosToString(v);
        CHECK(text.find("aaronia,box,54664") == 0, "saved as [%s]", text.c_str());
        const auto back = netRadiosFromString(text);
        CHECK(back.size() == 1 && back[0].type == NetRadio::Aaronia && back[0].host == "box", "read back");
        CHECK(netRadioDeviceName(v[0]).find("Aaronia") != std::string::npos, "list name");
    }
    // a working server: the requests, the samples and the rate
    {
        Fake f;
        CHECK(f.start(), "fake server");
        DeviceInfo d = native::aaroniaDevice("127.0.0.1", f.port, "");
        auto src = native::makeAaronia(d);
        TuneSettings s;
        s.centerHz = 433.92e6;
        s.sampleRate = 48000;
        IqRing ring(1 << 18);
        std::string err;
        CHECK(src->start(s, ring, err), "start: %s", err.c_str());
        std::vector<cf32> got;
        const size_t n = drain(ring, got);
        CHECK(n > 1000, "samples arrived: %zu", n);
        // the first packet counts up from 1 with the imaginary part mirrored
        CHECK(got.size() > 3 && got[0] == cf32(1, -1) && got[1] == cf32(2, -2) && got[2] == cf32(3, -3), "values: %.1f%+.1fi", got.empty() ? 0 : got[0].real(), got.empty() ? 0 : got[0].imag());
        CHECK(f.saw("GET /stream?format=float32"), "the stream request");
        CHECK(f.saw("PUT /control") && f.saw("\"start\":true"), "the graph was started");
        CHECK(f.saw("\"frequencyCenter\":433920000") && f.saw("\"frequencySpan\":48000"), "the frequency was sent");
        CHECK(f.saw("noauth"), "no password was sent when none was set");
        src->stop();
        CHECK(eventually([&] { return f.saw("\"start\":false"); }), "the graph was stopped");
        f.stop();
    }
    // the gap the timestamps show is reported as lost samples
    {
        Fake f;
        f.rate = 48000;
        CHECK(f.start(), "fake server");
        DeviceInfo d = native::aaroniaDevice("127.0.0.1", f.port, "");
        auto src = native::makeAaronia(d);
        TuneSettings s;
        s.centerHz = 100e6;
        s.sampleRate = 48000;
        IqRing ring(1 << 18);
        std::string err;
        CHECK(src->start(s, ring, err), "start: %s", err.c_str());
        std::vector<cf32> got;
        drain(ring, got, 500);
        const uint64_t before = ring.dropped();
        f.gapNow = 1;                                   // the server skips a second
        CHECK(eventually([&] { std::vector<cf32> x; drain(ring, x, 100); return ring.dropped() > before; }, 4000), "the gap was counted");
        CHECK(ring.dropped() - before >= 40000 && ring.dropped() - before <= 56000, "about a second at 48 kHz: %llu", (unsigned long long)(ring.dropped() - before));
        src->stop();
        f.stop();
    }
    // a server that wants a password: without one it says so, with one it works
    {
        Fake f;
        f.mode = Fake::NeedAuth;
        CHECK(f.start(), "fake server");
        DeviceInfo bad = native::aaroniaDevice("127.0.0.1", f.port, "");
        auto s1 = native::makeAaronia(bad);
        TuneSettings s;
        s.centerHz = 100e6;
        s.sampleRate = 48000;
        IqRing r1(1 << 16);
        std::string err;
        CHECK(!s1->start(s, r1, err), "no password: refused");
        CHECK(err.find("user name and password") != std::string::npos, "the message says what to do: %s", err.c_str());
        DeviceInfo good = native::aaroniaDevice("127.0.0.1", f.port, "user:pw");
        auto s2 = native::makeAaronia(good);
        IqRing r2(1 << 18);
        err.clear();
        CHECK(s2->start(s, r2, err), "with a password: %s", err.c_str());
        std::vector<cf32> got;
        CHECK(drain(r2, got) > 1000, "samples with a password");
        CHECK(f.saw("auth"), "the header was sent");
        s2->stop();
        f.stop();
        native::aaroniaSetAuth("127.0.0.1:" + std::to_string(f.port), "");
    }
    // a graph serving spectra instead of IQ: it connects but no samples come, and it does not crash or spin
    {
        Fake f;
        f.mode = Fake::Spectra;
        CHECK(f.start(), "fake server");
        DeviceInfo d = native::aaroniaDevice("127.0.0.1", f.port, "");
        auto src = native::makeAaronia(d);
        TuneSettings s;
        s.centerHz = 100e6;
        s.sampleRate = 48000;
        IqRing ring(1 << 16);
        std::string err;
        CHECK(src->start(s, ring, err), "start: %s", err.c_str());
        std::vector<cf32> got;
        CHECK(drain(ring, got, 600) == 0, "spectra give no IQ: %zu", got.size());
        src->stop();
        f.stop();
    }
    // nothing listening, and a server with no /stream
    {
        DeviceInfo d = native::aaroniaDevice("127.0.0.1", 1, "");
        auto src = native::makeAaronia(d);
        TuneSettings s;
        s.centerHz = 100e6;
        s.sampleRate = 48000;
        IqRing ring(1 << 14);
        std::string err;
        CHECK(!src->start(s, ring, err), "nothing listening");
        CHECK(err.find("Aaronia") != std::string::npos && err.find("RTSA-Suite") != std::string::npos, "the message names RTSA-Suite: %s", err.c_str());
        Fake f;
        f.mode = Fake::NotFound;
        CHECK(f.start(), "fake server");
        DeviceInfo d2 = native::aaroniaDevice("127.0.0.1", f.port, "");
        auto s2 = native::makeAaronia(d2);
        IqRing r2(1 << 14);
        err.clear();
        CHECK(!s2->start(s, r2, err), "no /stream endpoint");
        CHECK(err.find("404") != std::string::npos, "the status is in the message: %s", err.c_str());
        f.stop();
    }
    printf(fails ? "FAILED\n" : "aaronia source ok\n");
    return fails ? 1 : 0;
}
