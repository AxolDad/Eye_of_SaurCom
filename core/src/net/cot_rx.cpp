// Cursor on Target in (cot_rx.h): one UDP socket, one thread, the contacts by uid until they are stale.
#include "dect2/net/cot_rx.h"
#include "dect2/common/thread_name.h"
#include "dect2/rs/cot.h"
#include "netcompat.h"
#include "tcp_client.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

namespace dect2 {

namespace {
constexpr int64_t kNoStaleMs = 5 * 60 * 1000;   // an event without a stale time
constexpr size_t kMaxContacts = 5000;           // a flood cannot grow the table without end

int64_t wallMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

struct Contact {
    std::string type, callsign;
    double lat = 0, lon = 0, hae = NAN;
    int64_t heardMs = 0, staleMs = 0;
};
} // namespace

struct CotListener::Impl {
    mutable std::mutex mu;
    std::map<std::string, Contact> contacts;
    sock_t s = kBadSock;
    std::thread th;
    std::atomic<bool> quit{false};
};

CotListener& CotListener::get() { static CotListener l; return l; }
CotListener::CotListener() : impl_(new Impl) {}
CotListener::~CotListener() { stop(); }

bool CotListener::take(const uint8_t* d, size_t n, int64_t nowMs) {
    OnairCotEvent e{};
    if (!onair_cot_decode(d, n, &e)) return false;
    Contact c;
    c.type = (const char*)e.cot_type;
    c.callsign = (const char*)e.callsign;
    c.lat = e.lat; c.lon = e.lon; c.hae = e.hae;
    c.heardMs = nowMs;
    c.staleMs = e.stale_ms > 0 ? e.stale_ms : nowMs + kNoStaleMs;
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto& m = impl_->contacts;
    const std::string uid = (const char*)e.uid;
    if (m.size() >= kMaxContacts && !m.count(uid)) {   // full: make room by dropping what is already stale, else ignore the newcomer
        for (auto it = m.begin(); it != m.end();) it = it->second.staleMs <= nowMs ? m.erase(it) : std::next(it);
        if (m.size() >= kMaxContacts) return true;
    }
    m[uid] = std::move(c);
    received_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::vector<MapMarker> CotListener::markers(int64_t nowMs) const {
    std::vector<MapMarker> v;
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (const auto& [uid, c] : impl_->contacts) {
        if (c.staleMs <= nowMs) continue;
        MapMarker m;
        m.layer = LayerCot;
        m.id = uid;
        m.label = c.callsign.empty() ? uid : c.callsign;
        m.lat = c.lat; m.lon = c.lon;
        if (std::isfinite(c.hae)) { m.hasAlt = true; m.altM = c.hae; }
        m.ageSec = (double)(nowMs - c.heardMs) / 1000.0;
        m.detail = c.type;
        v.push_back(std::move(m));
    }
    return v;
}

void CotListener::stop() {
    impl_->quit = true;
    if (impl_->th.joinable()) impl_->th.join();
    if (impl_->s != kBadSock) { sockClose(impl_->s); impl_->s = kBadSock; }
    running_ = false;
}

bool CotListener::start(const std::string& group, int port, std::string& err) {
    stop();
    netInit();
    if (port <= 0 || port > 65535) { err = "port out of range"; return false; }
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBadSock) { err = "no UDP socket"; return false; }
    const int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);   // ATAK or another listener may share the port
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (const sockaddr*)&a, sizeof a) != 0) { sockClose(s); err = "cannot listen on UDP port " + std::to_string(port); return false; }
    in_addr g{};
    if (!group.empty() && inet_pton(AF_INET, group.c_str(), &g) == 1 && (ntohl(g.s_addr) >> 28) == 0xE) {   // 224.0.0.0/4: join it
        ip_mreq mr{};
        mr.imr_multiaddr = g; mr.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mr, sizeof mr) != 0) { sockClose(s); err = "cannot join " + group; return false; }
    }
    sockRecvTimeout(s, 300);   // so stop() is noticed
    impl_->s = s;
    impl_->quit = false;
    running_ = true;
    impl_->th = std::thread([this] {
        setThreadName("cot-rx");
        std::vector<uint8_t> buf(65536);
        while (!impl_->quit) {
            const long long n = sockRecv(impl_->s, buf.data(), buf.size());
            if (n > 0) take(buf.data(), (size_t)n, wallMs());
        }
    });
    return true;
}

} // namespace dect2
