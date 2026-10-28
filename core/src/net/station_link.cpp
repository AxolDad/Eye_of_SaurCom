#include "dect2/net/station_link.h"
#include "dect2/automation/engine.h"
#include "dect2/automation/json_mini.h"
#include "dect2/common/thread_name.h"
#include "dect2/net/event_ring.h"
#include "tcp_client.h"
#include "ws_client.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace dect2 {

namespace aj = automation::json;

namespace {

constexpr int kConnectMs = 3000, kReadMs = 1000, kMaxBackoffSec = 60, kHealthySec = 30;
constexpr size_t kMaxMessage = 1u << 20;   // json::parse takes at most 1 MiB

bool sameConfig(const SiteConfig& a, const SiteConfig& b) {
    return a.name == b.name && a.host == b.host && a.port == b.port && a.token == b.token && a.on == b.on;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c;
        else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
    }
    return r;
}

struct Link {
    SiteConfig cfg;
    SiteStatus st;
    mutable std::mutex mu;            // st
    std::mutex waitMu;                // for the backoff wait
    std::condition_variable cv;
    std::atomic<bool> stopFlag{false};
    std::mutex sockMu;                // sock: the owner closes under it, wake() shuts down under it, so a reused descriptor is never hit
    sock_t sock = kBadSock;
    std::atomic<bool> done{false};    // the thread has finished (set last)
    std::thread th;

    void setSock(sock_t s) { std::lock_guard<std::mutex> lk(sockMu); sock = s; }
    void closeSock(sock_t s) { std::lock_guard<std::mutex> lk(sockMu); sock = kBadSock; sockClose(s); }

    void setState(int s, const std::string& err = {}, bool keepErr = false) {
        std::lock_guard<std::mutex> lk(mu);
        st.state = s;
        if (!keepErr) st.lastError = err;
    }
    // Closes the socket under way so that a blocked read returns now
    void wake() {
        { std::lock_guard<std::mutex> lk(sockMu); if (sock != kBadSock) sockShutdown(sock); }
        std::lock_guard<std::mutex> lk(waitMu);
        cv.notify_all();
    }
    bool sleepFor(int sec) {   // false: stop was asked
        std::unique_lock<std::mutex> lk(waitMu);
        return !cv.wait_for(lk, std::chrono::seconds(sec), [this] { return stopFlag.load(); });
    }

    // GET /api/v1/status on its own connection: the site's position, when it tells. Best effort.
    void readPosition() {
        std::string err;
        sock_t s = tcpConnect(cfg.host, cfg.port, kConnectMs, err);
        if (s == kBadSock) return;
        setSock(s);   // so that stop() can cut it short
        if (stopFlag) { closeSock(s); return; }
        sockRecvTimeout(s, kReadMs * 2);
        const std::string hp = (cfg.host.find(':') != std::string::npos ? "[" + cfg.host + "]" : cfg.host) + ":" + std::to_string(cfg.port);
        std::string path = "/api/v1/status";
        if (!cfg.token.empty()) path += "?token=" + urlEncode(cfg.token);
        const std::string rq = "GET " + path + " HTTP/1.1\r\nHost: " + hp + "\r\nConnection: close\r\n\r\n";
        std::string resp;
        if (sockSendAll(s, rq.data(), rq.size())) {
            char b[2048];
            while (resp.size() < 65536) {
                const long long k = sockRecv(s, b, sizeof b);
                if (k <= 0) break;
                resp.append(b, (size_t)k);
            }
        }
        closeSock(s);
        const size_t e = resp.find("\r\n\r\n");
        if (resp.compare(0, 9, "HTTP/1.1 ") != 0 || resp.compare(9, 3, "200") != 0 || e == std::string::npos) return;
        aj::Value v; std::string perr;
        if (!aj::parse(resp.substr(e + 4), v, perr)) return;
        const aj::Value* la = v.get("lat");
        const aj::Value* lo = v.get("lon");
        std::lock_guard<std::mutex> lk(mu);
        st.lat = la && la->type == aj::Value::Num && std::isfinite(la->n) ? la->n : NAN;
        st.lon = lo && lo->type == aj::Value::Num && std::isfinite(lo->n) ? lo->n : NAN;
    }

    void onEvent(const std::string& line) {
        automation::Event ev; std::string err;
        if (!eventFromJson(line, ev, err)) return;
        ev.f["site"] = automation::Value::string(cfg.name);
        const double now = automation::wallNow();
        {
            std::lock_guard<std::mutex> lk(mu);
            st.events++;
            st.lastEventAt = now;
            if (ev.time != 0) {
                const double d = now - ev.time;
                st.clockSkewSec = st.events == 1 || st.clockSkewSec == 0 ? d : st.clockSkewSec + 0.1 * (d - st.clockSkewSec);
            }
        }
        automation::emit(std::move(ev));
    }

    // One connection from the handshake to its end. Returns the reason it ended; `live` says whether it got as far as the subscription.
    std::string runOnce(uint64_t& lastSeq, bool& haveSeq, bool& live) {
        std::string err;
        sock_t s = tcpConnect(cfg.host, cfg.port, kConnectMs, err);
        if (s == kBadSock) return err;
        setSock(s);
        struct Closer { Link& l; sock_t s; ~Closer() { l.closeSock(s); } } closer{*this, s};
        if (stopFlag) return "stopped";
        sockRecvTimeout(s, kReadMs);
        WsClient ws;
        ws.attach(s);
        std::string path = "/api/v1/ws";
        if (!cfg.token.empty()) path += "?token=" + urlEncode(cfg.token);
        if (!ws.handshake(cfg.host, cfg.port, path, err)) return err;
        std::string sub = "{\"events\":true";
        if (haveSeq) sub += ",\"since\":" + std::to_string(lastSeq);
        sub += "}";
        if (!ws.sendText(sub)) return "the connection dropped while subscribing";
        { std::lock_guard<std::mutex> lk(mu); st.lat = NAN; st.lon = NAN; }
        setState(2);
        live = true;
        readPosition();
        setSock(s);   // readPosition cleared it
        std::vector<unsigned char> msg;
        for (;;) {
            if (stopFlag) return "stopped";
            const WsClient::Kind k = ws.readMessage(msg, kMaxMessage);
            if (k == WsClient::Kind::Timeout) continue;
            if (k == WsClient::Kind::Closed) return stopFlag ? "stopped" : "the station closed the connection";
            if (k == WsClient::Kind::Bad) return "the station sent something that is not a valid WebSocket message";
            if (k != WsClient::Kind::Text) continue;
            aj::Value v; std::string perr;
            if (!aj::parse(std::string(msg.begin(), msg.end()), v, perr)) continue;
            const std::string type = v.str("type");
            if (type == "event") {
                const aj::Value* e = v.get("ev");
                const double seq = v.num("seq", -1);
                if (!e || e->type != aj::Value::Obj) continue;
                if (seq >= 0 && seq < 1.8e19) { lastSeq = (uint64_t)seq; haveSeq = true; }
                onEvent(aj::dump(*e));
            } else if (type == "gap") {
                const double m = v.num("missed", 0);
                if (m > 0 && m < 1.8e19) { std::lock_guard<std::mutex> lk(mu); st.missed += (uint64_t)m; }
            } else if (type == "reset") {
                const double h = v.num("head", 0);
                lastSeq = h >= 0 && h < 1.8e19 ? (uint64_t)h : 0;
                haveSeq = true;
            }
        }
    }

    void run() {
        uint64_t lastSeq = 0;
        bool haveSeq = false;
        int backoff = 2;
        while (!stopFlag) {
            setState(1, {}, true);
            bool live = false;
            const auto t0 = std::chrono::steady_clock::now();
            const std::string why = runOnce(lastSeq, haveSeq, live);
            if (stopFlag) break;
            const auto up = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
            if (live && up >= kHealthySec) backoff = 2;
            setState(3, why);
            if (!sleepFor(backoff)) break;
            backoff = std::min(backoff * 2, kMaxBackoffSec);
        }
    }
    void runThread() { run(); done = true; }   // nothing of the owner is touched after this
};

}  // namespace

struct StationLinks::Impl {
    std::mutex cfgMu;        // one configure() / stop() at a time
    mutable std::mutex mu;   // links
    std::vector<std::shared_ptr<Link>> links;
    std::vector<std::shared_ptr<Link>> retiring;   // stopped links whose threads may still be in connect / name lookup; joined in stop()
    void reap() {   // joins the finished ones (instant), keeps the rest; never blocks
        for (auto it = retiring.begin(); it != retiring.end();) {
            if ((*it)->done) { if ((*it)->th.joinable()) (*it)->th.join(); it = retiring.erase(it); }
            else ++it;
        }
    }
};

StationLinks::StationLinks() : p_(new Impl) {}
StationLinks::~StationLinks() { stop(); }

// Stops all of them: every socket is closed first so the threads wake together, then they are joined
static void haltAll(std::vector<std::shared_ptr<Link>>& v) {
    for (auto& l : v) { l->stopFlag = true; l->wake(); }
    for (auto& l : v) if (l->th.joinable()) l->th.join();
}

void StationLinks::configure(const std::vector<SiteConfig>& sites) {
    std::lock_guard<std::mutex> cl(p_->cfgMu);
    std::vector<std::shared_ptr<Link>> old;
    { std::lock_guard<std::mutex> lk(p_->mu); old = p_->links; }
    std::vector<std::shared_ptr<Link>> next, used;
    for (const SiteConfig& sc : sites) {
        if (sc.name.empty() || sc.host.empty()) continue;
        bool dup = false;
        for (auto& n : next) if (n->cfg.name == sc.name) dup = true;
        if (dup) continue;
        std::shared_ptr<Link> keep;
        for (auto& o : old) if (sameConfig(o->cfg, sc)) { keep = o; break; }
        if (keep) { next.push_back(keep); used.push_back(keep); continue; }
        auto l = std::make_shared<Link>();
        l->cfg = sc;
        if (sc.on) {
            Link* raw = l.get();
            l->st.state = 1;
            l->th = namedThread("saurcom-link", [raw] { raw->runThread(); });
        }
        next.push_back(l);
    }
    std::vector<std::shared_ptr<Link>> drop;
    for (auto& o : old) {
        bool u = false;
        for (auto& k : used) if (k == o) u = true;
        if (!u) drop.push_back(o);
    }
    { std::lock_guard<std::mutex> lk(p_->mu); p_->links = std::move(next); }
    // never join here: a thread inside connect() or getaddrinfo() cannot be woken. Signal, park it, reap what has finished.
    for (auto& d : drop) { d->stopFlag = true; d->wake(); p_->retiring.push_back(d); }
    p_->reap();
}

std::vector<std::pair<SiteConfig, SiteStatus>> StationLinks::status() const {
    std::vector<std::pair<SiteConfig, SiteStatus>> r;
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& l : p_->links) {
        std::lock_guard<std::mutex> lk2(l->mu);
        r.emplace_back(l->cfg, l->st);
    }
    return r;
}

void StationLinks::stop() {
    std::lock_guard<std::mutex> cl(p_->cfgMu);
    std::vector<std::shared_ptr<Link>> old;
    { std::lock_guard<std::mutex> lk(p_->mu); old.swap(p_->links); }
    old.insert(old.end(), p_->retiring.begin(), p_->retiring.end());
    p_->retiring.clear();
    haltAll(old);
}

StationLinks& globalLinks() {
    static StationLinks g;
    return g;
}

}  // namespace dect2
