// Aaronia RTSA-Suite client: a Spectran V6 (or another radio RTSA-Suite drives) reached through the "HTTP Server" block of its graph.
// The radio itself stays with RTSA-Suite; this opens two connections to that block, one holding GET /stream?format=... open for the
// samples and one for the PUT /control requests that set the centre and span and start the graph's streaming blocks. The wire format and
// the requests are in aaronia_proto.h, which says which parts of Aaronia's document each piece comes from.
//
// The graph decides what is actually possible: the span RTSA-Suite is set to is what arrives, so the rate asked for here is a request to
// the graph, and the rate reported back is the one its packets carry. A graph serving spectra rather than IQ is refused with a message
// saying so, because there is nothing a receiver can do with it.
#include "../net/tcp_client.h"
#include "aaronia_proto.h"
#include "native_common.h"
#include "dect2/net/net_proto.h"
#include <map>
#include <mutex>

namespace dect2 {
namespace native {

namespace {

// The credentials of each server, kept aside so they never travel in a DeviceInfo (the same way the KiwiSDR password is handled).
std::mutex gAuthMu;
std::map<std::string, std::string> gAuth;

constexpr int kConnectMs = 6000;
constexpr int kReplyMs = 6000;
constexpr int kStallMs = 4000;        // no bytes for this long: the connection is taken as lost
constexpr double kMinRate = 1e3;
constexpr double kMaxRate = 92e6;     // a V6 streams up to about 92 Msps over its own link; the HTTP block is the real limit
constexpr double kMinFreq = 1e6, kMaxFreq = 8e9;   // the V6 range, for the note the picker shows

std::string hostOf(const std::string& addr) {
    std::string h;
    int p = 0;
    return netp::splitHostPort(addr, h, p) ? h : addr;
}

// "user:password" to the Basic header value; "" when there is none.
std::string basicAuth(const std::string& userPass) {
    if (userPass.empty()) return "";
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < userPass.size(); i += 3) {
        const unsigned a = (unsigned char)userPass[i];
        const unsigned b = i + 1 < userPass.size() ? (unsigned char)userPass[i + 1] : 0;
        const unsigned c = i + 2 < userPass.size() ? (unsigned char)userPass[i + 2] : 0;
        const unsigned v = (a << 16) | (b << 8) | c;
        o += k[(v >> 18) & 63];
        o += k[(v >> 12) & 63];
        o += i + 1 < userPass.size() ? k[(v >> 6) & 63] : '=';
        o += i + 2 < userPass.size() ? k[v & 63] : '=';
    }
    return "Basic " + o;
}

aaronia::Format formatOf(const std::string& s) {
    if (s == "int16") return aaronia::Format::Int16;
    if (s == "float16") return aaronia::Format::Float16;
    return aaronia::Format::Float32;
}

void setCaps(DeviceInfo& d) {
    d.maxRateHz = kMaxRate;
    d.minRateHz = kMinRate;
    d.minFreqHz = kMinFreq;
    d.maxFreqHz = kMaxFreq;
    d.gainMinDb = d.gainMaxDb = 0;     // the gain belongs to the RTSA-Suite graph, not to this client
    d.settings.clear();
    d.settings.push_back(choiceSetting("format", "Sample format",
                                       "What the HTTP block sends. 32-bit float is the plain one. 16-bit float halves the bytes on the network for the same span. "
                                       "16-bit integer halves them too and is scaled by the factor the server puts in each packet.",
                                       {"float32", "float16", "int16"}, {"32-bit float", "16-bit float", "16-bit integer"}, "float32"));
    d.settings.push_back(boolSetting("tune", "Tune the graph",
                                     "Send the frequency and span to RTSA-Suite when you tune here (PUT /control). Turn it off to leave the graph alone and just "
                                     "listen to whatever it is already set to.", true));
    d.settings.push_back(boolSetting("startgraph", "Start the graph",
                                     "Ask RTSA-Suite to start its streaming blocks when the receiver starts, and stop them when it stops.", true));
    // The HTTP block can offer more than one input; the main one is what a graph serving IQ uses. The client can read another (streamPath
    // takes it) but there is no free-text setting to type one into, so it is not offered yet.
}

} // namespace

class AaroniaSource : public NativeSource {
public:
    AaroniaSource(std::string addr, std::string auth) : addr_(std::move(addr)), auth_(basicAuth(auth)) {}
    ~AaroniaSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        std::string host;
        int port = 0;
        if (!netp::splitHostPort(addr_, host, port)) {
            err = "Aaronia: bad address " + addr_ + " (host:port)";
            openFailure_ = OpenFailure::Final;
            return false;
        }
        if (port <= 0) port = aaronia::kDefaultPort;
        host_ = host;
        port_ = port;
        hostHeader_ = addr_;
        fmt_ = formatOf(radioOption(s, "format", "float32"));
        input_ = radioOption(s, "input", "");
        tuneGraph_ = radioOption(s, "tune", "1") != "0";
        startGraph_ = radioOption(s, "startgraph", "1") != "0";
        reader_.reset();
        reader_.setFormat(fmt_);
        havePrev_ = false;
        rate_ = 0;
        reportedRate_ = false;
        std::string e;
        stream_ = tcpConnect(host_, port_, kConnectMs, e);
        if (stream_ == kBadSock) {
            err = "Aaronia: " + e + " (is RTSA-Suite running with an HTTP Server block on " + addr_ + "?)";
            openFailure_ = OpenFailure::Retry;
            return false;
        }
        const std::string path = aaronia::streamPath(fmt_, 0, input_);
        const std::string req = aaronia::httpRequest("GET", path, hostHeader_, "", auth_);
        sockRecvTimeout(stream_, kReplyMs);
        if (!sockSendAll(stream_, req.data(), req.size())) { err = "Aaronia: " + addr_ + " closed the stream request"; return false; }
        // the response headers, then the body arrives without end
        std::string head;
        int status = 0;
        size_t headerLen = 0;
        std::string body;
        while (head.find("\r\n\r\n") == std::string::npos) {
            char b[2048];
            const long long k = sockRecv(stream_, b, sizeof b);
            if (k <= 0) { err = "Aaronia: " + addr_ + " sent no answer to the stream request"; return false; }
            head.append(b, (size_t)k);
            if (head.size() > (1u << 20)) { err = "Aaronia: " + addr_ + " sent a huge header"; return false; }
        }
        if (!aaronia::httpResponse(head, status, body, headerLen, chunked_)) { err = "Aaronia: bad answer from " + addr_; return false; }
        if (status == 401 || status == 403) {
            err = "Aaronia: " + addr_ + " wants a user name and password (put \"user:password\" in the radio's Password field)";
            openFailure_ = OpenFailure::Final;
            return false;
        }
        if (status != 200) {
            err = "Aaronia: " + addr_ + " answered " + std::to_string(status) + " to " + path;
            openFailure_ = OpenFailure::Final;
            return false;
        }
        pending_ = body;
        chunkPos_ = 0;
        sockRecvTimeout(stream_, kStallMs);
        // the graph is told what to do only once the stream is open, so a server that cannot be reached is reported as that
        if (!configure(s, err, false)) return false;
        fprintf(stderr, "Aaronia: streaming from %s%s, %s\n", addr_.c_str(), path.c_str(), aaronia::formatName(fmt_));
        fflush(stderr);
        return true;
    }

    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        (void)live;
        requested_ = s.sampleRate;
        if (!tuneGraph_ && !startGraph_) return true;
        std::string e;
        if (startGraph_ && !control(aaronia::streamingBody(true), e)) {
            err = "Aaronia: could not start the graph on " + addr_ + " (" + e + ")";
            openFailure_ = e.find("password") != std::string::npos ? OpenFailure::Final : OpenFailure::Retry;
            return false;
        }
        if (tuneGraph_ && s.sampleRate > 0) {
            if (!control(aaronia::tuneBody(s.centerHz, s.sampleRate), e)) {
                err = "Aaronia: could not set the frequency on " + addr_ + " (" + e + ")";
                openFailure_ = e.find("password") != std::string::npos ? OpenFailure::Final : OpenFailure::Retry;
                return false;
            }
        }
        return true;
    }

    void closeDevice() override {
        if (startGraph_ && ctrlOk_) { std::string e; control(aaronia::streamingBody(false), e); }
        if (stream_ != kBadSock) { sockClose(stream_); stream_ = kBadSock; }
        pending_.clear();
        chunkPos_ = 0;
    }

    void streamLoop() override {
        std::vector<cf32> iq;
        aaronia::Header h;
        while (run_) {
            // whatever is already buffered first, so a read that stalls does not hold back complete packets
            while (run_ && reader_.next(h, iq)) deliver(h, iq);
            char b[1 << 16];
            const long long k = sockRecv(stream_, b, sizeof b);
            if (k <= 0) break;
            pending_.append(b, (size_t)k);
            if (chunked_) {
                std::string chunk;
                bool done = false;
                while (aaronia::chunkedNext(pending_, chunkPos_, chunk, done)) {
                    if (!chunk.empty()) reader_.feed((const uint8_t*)chunk.data(), chunk.size());
                    if (done) { run_ = false; break; }
                }
                if (chunkPos_ > (1u << 20)) { pending_.erase(0, chunkPos_); chunkPos_ = 0; }   // keep the held text from growing without end
            } else {
                reader_.feed((const uint8_t*)pending_.data(), pending_.size());
                pending_.clear();
            }
        }
        if (run_) {
            fprintf(stderr, "Aaronia: lost the stream from %s\n", addr_.c_str());
            fflush(stderr);
        }
    }

    void interrupt() override { if (stream_ != kBadSock) sockShutdown(stream_); }
    bool restartForRate() const override { return false; }   // the span is a control request; the stream keeps running

private:
    void deliver(const aaronia::Header& h, std::vector<cf32>& iq) {
        if (!h.iq()) {   // the graph is serving something else: say so once, there is nothing a receiver can do with it
            if (!warnedPayload_) {
                warnedPayload_ = true;
                fprintf(stderr, "Aaronia: %s is serving \"%s\" packets, not IQ: put an IQ demodulator block before the HTTP Server block\n",
                        addr_.c_str(), h.payload.c_str());
                fflush(stderr);
            }
            return;
        }
        const double r = h.rateHz();
        if (r > 0 && (rate_ <= 0 || std::fabs(r - rate_) > rate_ * 0.01)) {
            rate_ = r;
            if (!reportedRate_ || true) {
                fprintf(stderr, "Aaronia: %.0f Hz span at %.6f MHz%s\n", r, h.centreHz() / 1e6,
                        requested_ > 0 && std::fabs(r - requested_) > requested_ * 0.01 ? " (the graph is set to this, not to what was asked for)" : "");
                fflush(stderr);
                reportedRate_ = true;
            }
        }
        if (havePrev_) {
            const uint64_t lost = aaronia::gapSamples(prev_, h, rate_ > 0 ? rate_ : r);
            if (lost) {   // Aaronia's document: the block drops data past an 8 MB outbound buffer, and the timestamps are how to tell
                lost_ += lost;
                if (ring_) ring_->noteLost(lost);
            }
        }
        prev_ = h;
        havePrev_ = true;
        if (!iq.empty()) push(iq.data(), iq.size());
    }

    // One PUT /control on its own short-lived connection: the stream socket is busy and must not be disturbed.
    bool control(const std::string& body, std::string& err) {
        std::lock_guard<std::mutex> lk(ctrlMu_);
        sock_t k = tcpConnect(host_, port_, kConnectMs, err);
        if (k == kBadSock) { ctrlOk_ = false; return false; }
        const std::string req = aaronia::httpRequest("PUT", "/control", hostHeader_, body, auth_);
        sockRecvTimeout(k, kReplyMs);
        bool ok = sockSendAll(k, req.data(), req.size());
        if (ok) {
            std::string head;
            while (head.find("\r\n\r\n") == std::string::npos && head.size() < (1u << 16)) {
                char b[1024];
                const long long got = sockRecv(k, b, sizeof b);
                if (got <= 0) break;
                head.append(b, (size_t)got);
            }
            int status = 0;
            size_t hl = 0;
            bool ch = false;
            std::string body2;
            if (!aaronia::httpResponse(head, status, body2, hl, ch)) { ok = false; err = "no answer"; }
            else if (status == 401 || status == 403) { ok = false; err = "it wants a user name and password"; }
            else if (status >= 400) { ok = false; err = "it answered " + std::to_string(status); }
        } else err = "the connection closed";
        sockClose(k);
        ctrlOk_ = ok;
        return ok;
    }

public:
    uint64_t lostSamples() const { return lost_; }

private:
    std::string addr_, auth_, host_, hostHeader_, input_;
    int port_ = aaronia::kDefaultPort;
    sock_t stream_ = kBadSock;
    std::mutex ctrlMu_;
    aaronia::Format fmt_ = aaronia::Format::Float32;
    aaronia::Reader reader_;
    aaronia::Header prev_;
    bool havePrev_ = false, chunked_ = false, warnedPayload_ = false, reportedRate_ = false;
    bool tuneGraph_ = true, startGraph_ = true, ctrlOk_ = false;
    std::string pending_;
    size_t chunkPos_ = 0;
    double rate_ = 0, requested_ = 0;
    uint64_t lost_ = 0;
};

void aaroniaSetAuth(const std::string& hostPort, const std::string& userPass) {
    std::lock_guard<std::mutex> lk(gAuthMu);
    if (userPass.empty()) gAuth.erase(hostPort);
    else gAuth[hostPort] = userPass;
}

static std::string authFor(const std::string& hostPort) {
    std::lock_guard<std::mutex> lk(gAuthMu);
    auto it = gAuth.find(hostPort);
    return it == gAuth.end() ? std::string() : it->second;
}

DeviceInfo aaroniaDevice(const std::string& host, int port, const std::string& auth) {
    DeviceInfo d;
    d.kind = DeviceInfo::Native;
    d.board = "aaronia";
    d.nativeArgs = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port > 0 ? port : aaronia::kDefaultPort);
    d.serial = d.nativeArgs;
    d.name = "Aaronia RTSA-Suite " + d.nativeArgs + " (network, Spectran)";
    setCaps(d);
    aaroniaSetAuth(d.nativeArgs, auth);   // kept aside: it must not travel in the device entry
    return d;
}

void aaroniaCaps(DeviceInfo& d) { setCaps(d); }

std::unique_ptr<IqSource> makeAaronia(const DeviceInfo& d) {
    return std::make_unique<AaroniaSource>(d.nativeArgs, authFor(d.nativeArgs));
}

} // namespace native
} // namespace dect2
