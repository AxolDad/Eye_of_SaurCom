#include "dect2/audio/audio_stream.h"
#include "dect2/audio/mp3_enc.h"
#include "dect2/common/thread_name.h"
#include "dect2/net/net_proto.h"
#include "../net/tcp_client.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace dect2 {

namespace {
constexpr size_t kRingFrames = 1u << 17;   // 2.7 s at 48 kHz: rides over a short network stall
constexpr int kRetryMs = 5000;
int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
// the mount as a request target: starts with '/', anything but unreserved characters and sub-delims is %XX (a space would end the target)
std::string mountOf(const std::string& m) {
    std::string r = m.empty() || m[0] != '/' ? "/" : "";
    for (size_t i = 0; i < m.size(); i++) {
        const unsigned char ch = (unsigned char)m[i];
        const bool escaped = ch == '%' && i + 2 < m.size() && std::isxdigit((unsigned char)m[i + 1]) && std::isxdigit((unsigned char)m[i + 2]);   // already encoded: kept
        if (ch == 0) continue;
        if ((ch < 0x80 && std::isalnum(ch)) || std::strchr("/-._~!$&'()*+,;=", ch) || escaped) r += (char)ch;
        else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", ch); r += b; }
    }
    return r;
}
// header values: no line breaks (a name with a newline would add a header of its own)
std::string oneLine(std::string s) { for (char& c : s) if (c == '\r' || c == '\n') c = ' '; return s; }
} // namespace

std::string icecastRequest(const AudioStreamConfig& c) {
    std::string r = "PUT " + oneLine(mountOf(c.mount)) + " HTTP/1.1\r\n";
    r += "Host: " + oneLine(c.host) + ":" + std::to_string(c.port) + "\r\n";
    r += "Authorization: Basic " + netp::base64(c.user + ":" + c.password) + "\r\n";
    r += "User-Agent: EyeOfZorCom\r\n";
    r += "Content-Type: audio/mpeg\r\n";
    r += "Ice-Public: " + std::string(c.publicListing ? "1" : "0") + "\r\n";
    r += "Ice-Name: " + oneLine(c.name) + "\r\n";
    if (!c.description.empty()) r += "Ice-Description: " + oneLine(c.description) + "\r\n";
    r += "Ice-Audio-Info: bitrate=" + std::to_string(c.kbps) + "\r\n";
    r += "\r\n";
    return r;
}

bool icecastAccepted(const std::string& statusLine, std::string& why) {
    // "HTTP/1.x 200 ..." (also the old SOURCE answer "HTTP/1.0 200 OK"); anything else is a refusal
    const size_t sp = statusLine.find(' ');
    const int code = sp == std::string::npos ? 0 : std::atoi(statusLine.c_str() + sp + 1);
    if (statusLine.rfind("HTTP/", 0) == 0 && code == 200) { why.clear(); return true; }
    if (code == 401) why = "the server refused the user name or password";
    else if (code == 403) why = "the server refused the stream (mount point in use, or not allowed)";
    else if (code > 0) why = "the server answered " + std::to_string(code);
    else why = "not an Icecast server";
    return false;
}

struct AudioStreamer::Impl {
    AudioStreamConfig cfg;
    std::vector<float> ring = std::vector<float>(kRingFrames * 2);
    std::atomic<uint64_t> w{0}, r{0};
    std::atomic<const void*> owner{nullptr};
    std::atomic<int64_t> lastMs{0};
    std::atomic<int> ringRate{0};
    std::atomic<uint64_t> dropped{0};
    std::thread th;
    std::atomic<bool> quit{false};
    mutable std::mutex mu;   // the stats below
    AudioStreamStats st;
};

AudioStreamer& AudioStreamer::instance() { static AudioStreamer s; return s; }
AudioStreamer::AudioStreamer() : p_(new Impl) {}
AudioStreamer::~AudioStreamer() { stop(); }

AudioStreamStats AudioStreamer::stats() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    AudioStreamStats s = p_->st;
    s.active = active();
    s.dropped = p_->dropped.load();
    return s;
}

void AudioStreamer::push(const void* src, const float* stereo, int frames, int rate) {
    Impl& s = *p_;
    const int64_t now = nowMs();
    const void* o = s.owner.load(std::memory_order_relaxed);
    if (o != src) {
        if (o && now - s.lastMs.load(std::memory_order_relaxed) < 2000) return;   // another output is the source
        if (!s.owner.compare_exchange_strong(o, src)) return;
    }
    s.lastMs.store(now, std::memory_order_relaxed);
    int rr = s.ringRate.load(std::memory_order_acquire);
    if (rr == 0 && s.ringRate.compare_exchange_strong(rr, rate)) rr = rate;   // the first audio sets the rate
    if (rate != rr) { s.ringRate.store(rate, std::memory_order_release); return; }   // the thread sees the change and starts a new stream
    const uint64_t w = s.w.load(std::memory_order_relaxed), r = s.r.load(std::memory_order_acquire);
    const size_t space = kRingFrames - (size_t)(w - r);
    const size_t n = std::min<size_t>((size_t)frames, space);
    if (n < (size_t)frames) s.dropped.fetch_add((uint64_t)frames - n, std::memory_order_relaxed);
    const size_t mask = kRingFrames - 1;
    for (size_t i = 0; i < n; i++) { const size_t k = (size_t)((w + i) & mask) * 2; s.ring[k] = stereo[2 * i]; s.ring[k + 1] = stereo[2 * i + 1]; }
    s.w.store(w + n, std::memory_order_release);
}

void AudioStreamer::stop() {
    if (!p_->th.joinable()) { active_ = false; return; }
    p_->quit = true;
    p_->th.join();
    active_ = false;
}

bool AudioStreamer::start(const AudioStreamConfig& c, std::string& err) {
    stop();
    if (!Mp3Enc::available()) { err = "streaming needs the LAME library (libmp3lame), which is not installed"; return false; }
    if (c.host.empty() || c.port <= 0 || c.port > 65535) { err = "the server address or port is missing"; return false; }
    if (c.kbps < 8 || c.kbps > 320) { err = "the bit rate must be 8 to 320 kbit/s"; return false; }
    Impl& s = *p_;
    s.cfg = c;
    s.quit = false;
    s.owner = nullptr; s.ringRate = 0; s.dropped = 0;
    s.r.store(s.w.load());
    { std::lock_guard<std::mutex> lk(s.mu); s.st = AudioStreamStats(); }
    active_ = true;
    s.th = std::thread([this] {
        setThreadName("audio-stream");
        Impl& s = *p_;
        sock_t sock = kBadSock;
        Mp3Enc enc;
        bool sendFailed = false;
        int encRate = 0;
        uint64_t frames = 0;
        int64_t retryAt = 0;
        std::vector<int16_t> pcm;
        const auto setErr = [&](const std::string& e, bool connected) { std::lock_guard<std::mutex> lk(s.mu); s.st.error = e; s.st.connected = connected; };
        const auto drop = [&](const std::string& why) {
            if (encRate) { enc.finish(); encRate = 0; }   // the encoder's last frames still go out when the connection is alive
            if (sock != kBadSock) { sockClose(sock); sock = kBadSock; }
            setErr(why, false);
            retryAt = nowMs() + kRetryMs;
        };
        while (!s.quit) {
            const int rate = s.ringRate.load(std::memory_order_acquire);
            if (sock != kBadSock && encRate && rate != encRate) drop("");   // a new sample rate: a new stream
            if (sock == kBadSock) {
                s.r.store(s.w.load(std::memory_order_acquire), std::memory_order_release);   // no stale audio after a reconnect
                if (rate == 0 || nowMs() < retryAt) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
                std::string err;
                sock = tcpConnect(s.cfg.host, s.cfg.port, 5000, err);
                if (sock == kBadSock) { drop(err); continue; }
                const std::string req = icecastRequest(s.cfg);
                std::string answer;
                sockRecvTimeout(sock, 5000);
                if (sockSendAll(sock, req.data(), req.size())) {
                    char b[512];
                    while (answer.find("\r\n") == std::string::npos && answer.size() < 4096) {
                        const long long k = sockRecv(sock, b, sizeof b);
                        if (k <= 0) break;
                        answer.append(b, (size_t)k);
                    }
                }
                std::string why;
                if (!icecastAccepted(answer.substr(0, answer.find("\r\n")), why)) { drop(answer.empty() ? "no answer from the server" : why); { std::lock_guard<std::mutex> lk(s.mu); s.st.reconnects++; } continue; }
                sendFailed = false;
                if (!enc.begin([&](const unsigned char* d, size_t n) {
                        if (sendFailed || !sockSendAll(sock, d, n)) { sendFailed = true; return; }
                        std::lock_guard<std::mutex> lk(s.mu); s.st.bytesSent += n;
                    }, (uint32_t)rate, s.cfg.kbps, 2)) { drop("the MP3 encoder could not start"); continue; }
                encRate = rate;
                setErr("", true);
            }
            const uint64_t w = s.w.load(std::memory_order_acquire), r = s.r.load(std::memory_order_relaxed);
            const size_t n = (size_t)std::min<uint64_t>(w - r, 4800);
            if (n == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
            pcm.resize(n * 2);
            const size_t mask = kRingFrames - 1;
            for (size_t i = 0; i < n; i++) {
                const size_t k = (size_t)((r + i) & mask) * 2;
                for (int ch = 0; ch < 2; ch++) pcm[2 * i + ch] = (int16_t)std::lrint(std::max(-1.f, std::min(1.f, s.ring[k + ch])) * 32767.f);
            }
            s.r.store(r + n, std::memory_order_release);
            enc.write(pcm.data(), n);
            if (sendFailed) { drop("the connection to the server broke"); std::lock_guard<std::mutex> lk(s.mu); s.st.reconnects++; continue; }
            frames += n;
            std::lock_guard<std::mutex> lk(s.mu); s.st.seconds = (double)frames / rate;
        }
        if (sock != kBadSock) { if (encRate) enc.finish(); sockClose(sock); }
        std::lock_guard<std::mutex> lk(s.mu); s.st.connected = false;
    });
    return true;
}

}  // namespace dect2
