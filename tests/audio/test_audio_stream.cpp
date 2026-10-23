// "Stream audio" (dect2/audio/audio_stream.h): the Icecast source request, the server's answer, and a whole stream into a fake server on the loopback.
#include "dect2/audio/audio_stream.h"
#include "dect2/audio/mp3_enc.h"
#include "../../core/src/net/tcp_client.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

// a TCP server on 127.0.0.1 (any free port) that takes one source, answers `reply` and keeps what follows
struct FakeIcecast {
    sock_t ls = kBadSock;
    int port = 0;
    std::string reply, request, body;
    std::thread th;
    std::atomic<bool> done{false};
    explicit FakeIcecast(std::string r) : reply(std::move(r)) {
        netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
        bind(ls, (sockaddr*)&a, sizeof a);
        listen(ls, 1);
        socklen_t n = sizeof a; getsockname(ls, (sockaddr*)&a, &n); port = ntohs(a.sin_port);
        th = std::thread([this] {
            sock_t c = accept(ls, nullptr, nullptr);
            if (c == kBadSock) return;
            sockRecvTimeout(c, 3000);
            char b[4096];
            while (request.find("\r\n\r\n") == std::string::npos) { const long long k = sockRecv(c, b, sizeof b); if (k <= 0) break; request.append(b, (size_t)k); }
            const size_t e = request.find("\r\n\r\n");
            if (e != std::string::npos) { body = request.substr(e + 4); request.resize(e + 4); }
            sockSendAll(c, reply.data(), reply.size());
            for (int i = 0; i < 100 && body.size() < 2000; i++) { const long long k = sockRecv(c, b, sizeof b); if (k <= 0) break; body.append(b, (size_t)k); }
            done = true;
            sockClose(c);
        });
    }
    ~FakeIcecast() { sockShutdown(ls); sockClose(ls); if (th.joinable()) th.join(); }
};

static bool hasMp3Frame(const std::string& b) {
    for (size_t i = 0; i + 1 < b.size(); i++) if ((uint8_t)b[i] == 0xFF && ((uint8_t)b[i + 1] & 0xE0) == 0xE0) return true;
    return false;
}

static void requestChecks() {
    AudioStreamConfig c;
    c.host = "radio.example"; c.port = 8000; c.mount = "zorcom"; c.password = "hackme"; c.name = "Eye\r\nX-Evil: 1"; c.kbps = 96;
    const std::string r = icecastRequest(c);
    CHECK(r.rfind("PUT /zorcom HTTP/1.1\r\n", 0) == 0, "PUT to the mount, '/' added: %s", r.c_str());
    CHECK(r.find("\r\nAuthorization: Basic c291cmNlOmhhY2ttZQ==\r\n") != std::string::npos, "Basic source:hackme");
    CHECK(r.find("\r\nContent-Type: audio/mpeg\r\n") != std::string::npos && r.find("Ice-Audio-Info: bitrate=96") != std::string::npos, "MP3 and its bit rate");
    CHECK(r.find("\r\nX-Evil") == std::string::npos && r.find("Ice-Name: Eye  X-Evil: 1\r\n") != std::string::npos, "a line break cannot add a header");
    CHECK(r.size() > 4 && r.compare(r.size() - 4, 4, "\r\n\r\n") == 0, "headers end with a blank line");
    std::string why;
    CHECK(icecastAccepted("HTTP/1.1 200 OK", why) && icecastAccepted("HTTP/1.0 200 OK", why), "200 accepted");
    CHECK(!icecastAccepted("HTTP/1.1 401 Unauthorized", why) && why.find("password") != std::string::npos, "401: %s", why.c_str());
    CHECK(!icecastAccepted("HTTP/1.1 403 Forbidden", why) && why.find("mount") != std::string::npos, "403: %s", why.c_str());
    CHECK(!icecastAccepted("SSH-2.0-OpenSSH", why) && !icecastAccepted("", why), "not HTTP");
    std::string err;
    AudioStreamConfig bad;
    CHECK(!AudioStreamer::instance().start(bad, err) && !err.empty(), "no host refused: %s", err.c_str());
}

// what AudioOut::write() does: offer the sound, here a 440 Hz tone from one output
static void play(int ms) {
    static double ph = 0;
    static const int src = 0;
    std::vector<float> b(480 * 2);
    for (int t = 0; t < ms / 10; t++) {
        for (int i = 0; i < 480; i++) { const float v = 0.3f * (float)std::sin(ph); ph += 2 * M_PI * 440 / 48000; b[2 * i] = b[2 * i + 1] = v; }
        AudioStreamer::offer(&src, b.data(), 480, 48000);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

static void streamChecks() {
    if (!Mp3Enc::available()) { std::printf("SKIP: libmp3lame is not installed, the live stream is not tested\n"); return; }
    AudioStreamer& S = AudioStreamer::instance();
    {
        FakeIcecast srv("HTTP/1.1 200 OK\r\nServer: Icecast 2.4.4\r\n\r\n");
        AudioStreamConfig c; c.host = "127.0.0.1"; c.port = srv.port; c.mount = "/test"; c.password = "pw"; c.description = "FM 93.900 MHz";
        std::string err;
        CHECK(S.start(c, err), "start: %s", err.c_str());
        for (int i = 0; i < 40 && !srv.done; i++) play(100);
        S.stop();
        CHECK(srv.request.rfind("PUT /test HTTP/1.1\r\n", 0) == 0 && srv.request.find("Ice-Description: FM 93.900 MHz") != std::string::npos, "the server got the source request");
        CHECK(srv.body.size() > 500 && hasMp3Frame(srv.body), "MP3 frames arrived: %zu bytes", srv.body.size());
        const AudioStreamStats st = S.stats();
        CHECK(!st.active && st.bytesSent > 0 && st.seconds > 0.5 && st.error.empty(), "stats: sent %llu, %.1f s, '%s'", (unsigned long long)st.bytesSent, st.seconds, st.error.c_str());
    }
    {
        FakeIcecast srv("HTTP/1.1 401 Unauthorized\r\n\r\n");
        AudioStreamConfig c; c.host = "127.0.0.1"; c.port = srv.port; c.password = "wrong";
        std::string err;
        CHECK(S.start(c, err), "start: %s", err.c_str());
        AudioStreamStats st;
        for (int i = 0; i < 30; i++) { play(100); st = S.stats(); if (!st.error.empty()) break; }
        S.stop();
        CHECK(!st.connected && st.error.find("password") != std::string::npos && st.bytesSent == 0, "refused: '%s'", st.error.c_str());
    }
}

int main() {
    requestChecks();
    streamChecks();
    std::printf(fails ? "audio stream tests FAILED (%d)\n" : "audio stream tests passed\n", fails);
    return fails ? 1 : 0;
}
