// Cursor on Target in (cot_rx.h): one UDP socket, one thread, the contacts by uid until they are stale.
#include "dect2/net/cot_rx.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/cot.h"
#include "udp_listen.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace dect2 {

namespace {
constexpr int64_t kNoStaleMs = 5 * 60 * 1000;   // an event without a stale time
constexpr size_t kMaxContacts = 5000;           // a flood cannot grow the table without end

struct AbiCheck {
    AbiCheck() { rsAbiCheck("saurcom_cot", {saurcom_cot_struct_size()}, {sizeof(SaurcomCotEvent)}); }
} abiCheck;

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
    UdpListen udp;
};

CotListener& CotListener::get() { static CotListener l; return l; }
CotListener::CotListener() : impl_(new Impl) {}
CotListener::~CotListener() { stop(); }

bool CotListener::take(const uint8_t* d, size_t n, int64_t nowMs) {
    SaurcomCotEvent e{};
    if (!saurcom_cot_decode(d, n, &e)) return false;
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
    impl_->udp.stop();
    running_ = false;
}

bool CotListener::start(const std::string& group, int port, std::string& err) {
    stop();
    if (!impl_->udp.open(group, port, true, err)) return false;   // ATAK or another listener may share the port
    running_ = true;
    impl_->udp.run("cot-rx", 65536, [this](const uint8_t* d, size_t n, const std::string&) { take(d, n, wallMs()); });
    return true;
}

} // namespace dect2
