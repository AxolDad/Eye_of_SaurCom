// Internet radio (dect2/audio/web_radio.h): the decoder finds and decodes MP3 (also after an ID3 tag, in odd chunk sizes), refuses what is
// not audio, the player refuses addresses that are not http(s), and plays an MP3 stream from a server on the loopback through curl.
#include "dect2/audio/web_radio.h"
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

// `secs` of a 440 Hz stereo tone at 48 kHz as MP3
static std::string mp3Tone(double secs) {
    std::string out;
    Mp3Enc e;
    if (!e.begin([&](const unsigned char* d, size_t n) { out.append((const char*)d, n); }, 48000, 128, 2)) return out;
    std::vector<int16_t> pcm(4800 * 2);
    double ph = 0;
    for (int b = 0; b < (int)(secs * 10); b++) {
        for (int i = 0; i < 4800; i++) { const int16_t v = (int16_t)(8000 * std::sin(ph)); ph += 2 * M_PI * 440 / 48000; pcm[2 * i] = pcm[2 * i + 1] = v; }
        e.write(pcm.data(), 4800);
    }
    e.finish();
    return out;
}

static void decoderChecks(const std::string& mp3) {
    {
        WebRadioDecoder d;
        uint64_t frames = 0; int rate = 0;
        size_t at = 0, step = 1;
        while (at < mp3.size()) {   // chunks of 1, 2, 3 ... 997 bytes: frames cut anywhere
            const size_t n = std::min(step, mp3.size() - at);
            CHECK(d.feed((const uint8_t*)mp3.data() + at, n, [&](const float*, int f, int r) { frames += (uint64_t)f; rate = r; }), "feed");
            at += n; step = step % 997 + 1;
        }
        CHECK(std::string(d.codec()) == "MP3" && rate == 48000, "MP3 at 48 kHz: '%s' %d", d.codec(), rate);
        CHECK(frames > 2.6 * 48000 && frames < 3.1 * 48000, "about 3 s decoded: %.2f s", frames / 48000.0);
    }
    {   // an ID3v2 tag in front whose bytes look like an AAC sync
        std::string tagged = std::string("ID3\x03\x00\x00\x00\x00\x00\x14", 10) + std::string("\xFF\xF1\x50\x80", 4) + std::string(16, '\0') + mp3;
        WebRadioDecoder d;
        uint64_t frames = 0;
        d.feed((const uint8_t*)tagged.data(), tagged.size(), [&](const float*, int f, int) { frames += (uint64_t)f; });
        CHECK(std::string(d.codec()) == "MP3" && frames > 2.6 * 48000, "ID3 skipped: '%s', %.2f s", d.codec(), frames / 48000.0);
    }
    {   // an HTML error page or a playlist: no frame sync
        std::string junk(70 * 1024, 'x');
        WebRadioDecoder d;
        CHECK(!d.feed((const uint8_t*)junk.data(), junk.size(), [](const float*, int, int) {}) && std::string(d.codec()).empty(), "not audio: refused");
    }
    CHECK(WebRadio::urlOk("https://stream.example.net/live.mp3") && WebRadio::urlOk("http://1.2.3.4:8000/x"), "http(s) ok");
    CHECK(!WebRadio::urlOk("file:///etc/passwd") && !WebRadio::urlOk("-o /tmp/x") && !WebRadio::urlOk("http://a b") && !WebRadio::urlOk("ftp://x"), "others refused");
}

// a server on 127.0.0.1 that answers one GET with `body` as audio/mpeg
struct FakeStation {
    sock_t ls = kBadSock; int port = 0; std::string body; std::thread th; std::atomic<bool> got{false};
    explicit FakeStation(std::string b) : body(std::move(b)) {
        netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(ls, (sockaddr*)&a, sizeof a); listen(ls, 1);
        socklen_t n = sizeof a; getsockname(ls, (sockaddr*)&a, &n); port = ntohs(a.sin_port);
        th = std::thread([this] {
            sock_t c = accept(ls, nullptr, nullptr);
            if (c == kBadSock) return;
            sockRecvTimeout(c, 3000);
            char buf[2048]; std::string req;
            while (req.find("\r\n\r\n") == std::string::npos) { const long long k = sockRecv(c, buf, sizeof buf); if (k <= 0) break; req.append(buf, (size_t)k); }
            got = req.rfind("GET /live HTTP/1.1\r\n", 0) == 0 && req.find("User-Agent: EyeOfSaurCom") != std::string::npos;
            const std::string h = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nicy-name: SaurCom FM\r\nConnection: close\r\n\r\n";
            sockSendAll(c, h.data(), h.size());
            sockSendAll(c, body.data(), body.size());
            sockClose(c);
        });
    }
    ~FakeStation() { sockShutdown(ls); sockClose(ls); if (th.joinable()) th.join(); }
};

static void playerChecks(const std::string& mp3) {
    WebRadio& R = WebRadio::get();
    std::string err;
    CHECK(!R.play("file:///etc/passwd", err) && !err.empty(), "file:// refused: %s", err.c_str());
    FakeStation st(mp3);
    CHECK(R.play("http://127.0.0.1:" + std::to_string(st.port) + "/live", err), "play: %s", err.c_str());
    WebRadioStats s;
    for (int i = 0; i < 100; i++) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); s = R.stats(); if (!s.playing) break; }
    R.stop();
    CHECK(st.got, "the server got the GET with our User-Agent");
    CHECK(s.codec == "MP3" && s.rate == 48000 && s.bytes >= mp3.size() && s.seconds > 2.5, "played: '%s' %d Hz, %llu bytes, %.2f s, '%s'",
          s.codec.c_str(), s.rate, (unsigned long long)s.bytes, s.seconds, s.error.c_str());
}

int main() {
    if (!Mp3Enc::available()) { std::printf("SKIP: libmp3lame is not installed (needed to make the test stream)\n"); return 0; }
    const std::string mp3 = mp3Tone(3);
    CHECK(mp3.size() > 20000, "test stream: %zu bytes", mp3.size());
    decoderChecks(mp3);
    playerChecks(mp3);
    std::printf(fails ? "web radio tests FAILED (%d)\n" : "web radio tests passed\n", fails);
    return fails ? 1 : 0;
}
