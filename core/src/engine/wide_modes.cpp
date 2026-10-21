// Whole-span decoding for the digital voice modes: see wide_modes.h.
#include "dect2/engine/wide_modes.h"
#include "dect2/dmr/dmr_rx.h"
#include "dect2/dpmr/dpmr_rx.h"
#include "dect2/dstar/dstar_rx.h"
#include "dect2/m17/m17_rx.h"
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/pager/pager_tel.h"
#include "dect2/packet/packet_rx.h"
#include "dect2/packet/packet_tel.h"
#include "dect2/dpmr/dpmr_tel.h"
#include "dect2/dstar/dstar_tel.h"
#include "dect2/m17/m17_tel.h"
#include "dect2/nxdn/nxdn_tel.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/t2/t2rx.h"
#include "dect2/engine/wide_bank.h"
#include <algorithm>
#include <cstdio>

namespace dect2 {

namespace {

constexpr size_t kLogCap = 200;

// newest first lists: stamp, append, order by start time, cap
template <class T> void appendNewestFirst(std::vector<T>& d, const std::vector<T>& s, double hz) {
    for (T c : s) { c.carrierId = 1; c.carrierHz = hz; d.push_back(std::move(c)); }
    std::stable_sort(d.begin(), d.end(), [](const T& a, const T& b) { return a.wallTime > b.wallTime; });
    if (d.size() > kLogCap) d.resize(kLogCap);
}

std::string fmt(const char* f, int v) { char b[48]; snprintf(b, sizeof b, f, v); return b; }

template <class Rx> void commonSetup(Rx& r, const std::function<void(const std::string&)>& log) {
    r.setSilent(true);
    if (log) r.setLogCallback([log](const std::string& line) { if (!WideLogCapture::sink(line)) log(line); });   // inside a parallel feed the line is held and logged in carrier order
}

template <class Rx> void audioSwitch(Rx& r, bool play) { r.setSilent(!play); r.setMuted(!play); }

} // namespace

struct WideModes::Impl {
    std::function<void(const std::string&)> log;
    std::unique_ptr<WideBank<DmrReceiver, DmrTelemetry>> dmr;
    std::unique_ptr<WideBank<M17Receiver, M17Telemetry>> m17;
    std::unique_ptr<WideBank<DstarReceiver, DstarTelemetry>> dstar;
    std::unique_ptr<WideBank<NxdnReceiver, NxdnTelemetry>> nxdn;
    std::unique_ptr<WideBank<DpmrReceiver, DpmrTelemetry>> dpmr;
    std::unique_ptr<WideBank<P25Receiver, P25Telemetry>> p25;
    std::unique_ptr<WideBank<PagerReceiver, PagerTelemetry>> pager;
    std::unique_ptr<WideBank<PacketReceiver, PacketTelemetry>> packet;
    bool pagerNarrow = false, packetNarrow = false;   // the single receivers' channel width, copied to every carrier receiver (setNarrowChannels)
    int pagerProtos = 3, pagerBauds = 7;   // the single receiver's settings, copied to every carrier's receiver (setPagerOptions)
    bool muted = false;
    float volume = 1.f;
    int sel = 0;

    template <class Rx, class Tel> WideOps<Rx, Tel> baseOps(double outRate) {
        // every single-channel receiver does its own channel filtering and decimation to 48 kHz, and some refuse an input below 1 Msps (DMR) or 250 ksps:
        // the carrier is handed over at that low rate, mixed to 0 Hz, with only the aliases of the narrowing removed here
        WideOps<Rx, Tel> o;
        o.outRate = outRate; o.occupiedHz = 60000;
        o.log = [this](const std::string& line) { if (log) log(line); };
        o.setup = [this](Rx& r) { commonSetup(r, log); r.setVolume(volume); };
        o.audio = [](Rx& r, bool play) { audioSwitch(r, play); };
        o.clear = [](Rx& r) { r.clearResults(); };
        return o;
    }
    Impl() {
        auto d = baseOps<DmrReceiver, DmrTelemetry>(1.2e6);
        d.clearLists = [](DmrTelemetry& t) { t.callLog.clear(); t.messages.clear(); };
        d.append = [](DmrTelemetry& t, const DmrTelemetry& s, double hz) {
            for (DmrCall c : s.callLog) { c.carrierId = 1; c.carrierHz = hz; t.callLog.push_back(c); }
            for (DmrMessage m : s.messages) { m.carrierId = 1; m.carrierHz = hz; t.messages.push_back(m); }
            if (t.callLog.size() > kLogCap) t.callLog.erase(t.callLog.begin(), t.callLog.end() - (long)kLogCap);
            if (t.messages.size() > kLogCap) t.messages.erase(t.messages.begin(), t.messages.end() - (long)kLogCap);
        };
        d.info = [](const DmrTelemetry& t) { return t.cc >= 0 ? fmt("CC %d", t.cc) + (t.link.empty() ? "" : " " + t.link) : t.link; };
        dmr.reset(new WideBank<DmrReceiver, DmrTelemetry>(d));

        auto m = baseOps<M17Receiver, M17Telemetry>(300000);
        m.clearLists = [](M17Telemetry& t) { t.log.clear(); };
        m.append = [](M17Telemetry& t, const M17Telemetry& s, double hz) { appendNewestFirst(t.log, s.log, hz); };
        m.info = [](const M17Telemetry& t) { return t.log.empty() ? std::string() : t.log.front().src; };
        m17.reset(new WideBank<M17Receiver, M17Telemetry>(m));

        auto s = baseOps<DstarReceiver, DstarTelemetry>(300000);
        s.clearLists = [](DstarTelemetry& t) { t.log.clear(); };
        s.append = [](DstarTelemetry& t, const DstarTelemetry& x, double hz) { appendNewestFirst(t.log, x.log, hz); };
        s.info = [](const DstarTelemetry& t) { return t.log.empty() ? std::string() : t.log.front().my; };
        dstar.reset(new WideBank<DstarReceiver, DstarTelemetry>(s));

        auto n = baseOps<NxdnReceiver, NxdnTelemetry>(300000);
        n.clearLists = [](NxdnTelemetry& t) { t.log.clear(); };
        n.append = [](NxdnTelemetry& t, const NxdnTelemetry& s2, double hz) { appendNewestFirst(t.log, s2.log, hz); };
        n.info = [](const NxdnTelemetry& t) { return t.ran >= 0 ? fmt("RAN %d", t.ran) : std::string(); };
        nxdn.reset(new WideBank<NxdnReceiver, NxdnTelemetry>(n));

        auto p = baseOps<DpmrReceiver, DpmrTelemetry>(300000);
        p.clearLists = [](DpmrTelemetry& t) { t.log.clear(); };
        p.append = [](DpmrTelemetry& t, const DpmrTelemetry& s2, double hz) { appendNewestFirst(t.log, s2.log, hz); };
        p.info = [](const DpmrTelemetry& t) { return t.cc >= 0 ? fmt("CC %d", t.cc) : std::string(); };
        dpmr.reset(new WideBank<DpmrReceiver, DpmrTelemetry>(p));

        auto q = baseOps<P25Receiver, P25Telemetry>(300000);
        q.clearLists = [](P25Telemetry& t) { t.log.clear(); t.tsbks.clear(); };
        q.append = [](P25Telemetry& t, const P25Telemetry& s2, double hz) {
            appendNewestFirst(t.log, s2.log, hz);
            // the control channel's messages of every carrier, oldest first by their process-wide number
            t.tsbks.insert(t.tsbks.end(), s2.tsbks.begin(), s2.tsbks.end());
            std::sort(t.tsbks.begin(), t.tsbks.end(), [](const P25TsbkRec& a, const P25TsbkRec& b) { return a.seq < b.seq; });
            t.tsbks.erase(std::unique(t.tsbks.begin(), t.tsbks.end(), [](const P25TsbkRec& a, const P25TsbkRec& b) { return a.seq == b.seq; }), t.tsbks.end());
            if (t.tsbks.size() > kP25TsbkMax) t.tsbks.erase(t.tsbks.begin(), t.tsbks.end() - (long)kP25TsbkMax);
        };
        q.info = [](const P25Telemetry& t) { return t.nac >= 0 ? fmt("NAC %03X", t.nac) : std::string(); };
        p25.reset(new WideBank<P25Receiver, P25Telemetry>(q));

        // pagers: one 25 kHz channel each; the carrier is brought to 192 ksps (the receiver's own front end takes it from there). Pages come in bursts,
        // so a carrier found late gets the half second before it was found, and a carrier stays for half a minute after its last burst.
        WideOps<PagerReceiver, PagerTelemetry> g;
        g.outRate = 192000; g.occupiedHz = 24000; g.preRollSec = 0.5;
        g.setup = [this](PagerReceiver& r) { r.setWideInput(true); r.setProtocols(pagerProtos); r.setPocsagBauds(pagerBauds); r.setNarrowChannel(pagerNarrow); };
        g.clear = [](PagerReceiver& r) { r.clearResults(); };
        g.clearLists = [](PagerTelemetry& t) {
            t.messages.reset(); t.messagesTotal = 0; t.blocksOk = t.blocksBad = 0; t.signal = t.sync = false;
            for (auto& st : t.speeds) st = PagerSpeedStat();
        };
        g.hasLog = [](const PagerTelemetry& t) { return t.messages && !t.messages->empty(); };
        g.append = [](PagerTelemetry& t, const PagerTelemetry& x, double hz) {
            t.messagesTotal += x.messagesTotal; t.blocksOk += x.blocksOk; t.blocksBad += x.blocksBad;
            t.signal = t.signal || x.signal; t.sync = t.sync || x.sync;
            for (int i = 0; i < kPagerSpeeds; i++) {
                t.speeds[i].ok += x.speeds[i].ok; t.speeds[i].fixed += x.speeds[i].fixed; t.speeds[i].failed += x.speeds[i].failed;
                t.speeds[i].messages += x.speeds[i].messages; t.speeds[i].transmissions += x.speeds[i].transmissions;
            }
            if (!x.messages || x.messages->empty()) return;
            auto v = std::make_shared<std::vector<PagerMessage>>(t.messages ? *t.messages : std::vector<PagerMessage>());
            for (PagerMessage m : *x.messages) { m.carrierId = 1; m.carrierHz = hz; v->push_back(std::move(m)); }
            std::stable_sort(v->begin(), v->end(), [](const PagerMessage& a, const PagerMessage& b) { return a.wallTime > b.wallTime; });
            if (v->size() > 2000) v->resize(2000);
            t.messages = std::move(v);
        };
        g.freqCorrection = [](const PagerTelemetry& t) { return t.cfoHz; };   // the receiver's carrier offset (its AFC measures it on every transmission)
        g.rowState = [](const PagerTelemetry& t) { return t.state == 1 ? 2 : 0; };
        g.info = [](const PagerTelemetry& t) {
            if (t.lastSpeed < 0 && !t.messagesTotal) return std::string();
            char b[80]; snprintf(b, sizeof b, "%s, %llu pages", t.lastSpeed >= 0 ? pagerSpeedName(t.lastSpeed) : "no sync", (unsigned long long)t.messagesTotal);
            return std::string(b);
        };
        pager.reset(new WideBank<PagerReceiver, PagerTelemetry>(g));

        // APRS / packet: one 25 kHz FM channel each (144.800, 144.390 and the neighbours inside one capture); no sound from the carrier receivers
        WideOps<PacketReceiver, PacketTelemetry> k;
        k.outRate = 192000; k.occupiedHz = 24000; k.preRollSec = 0.5;
        k.setup = [this](PacketReceiver& r) { r.setWideInput(true); r.setSilent(true); r.setNarrowChannel(packetNarrow); };
        k.clear = [](PacketReceiver& r) { r.clearResults(); };
        k.clearLists = [](PacketTelemetry& t) {
            t.frames.clear(); t.stations.clear(); t.blocksOk = t.blocksBad = 0; t.ok1200 = t.bad1200 = t.ok9600 = t.bad9600 = t.aprsFrames = 0; t.carrier = false;
        };
        k.hasLog = [](const PacketTelemetry& t) { return !t.frames.empty() || !t.stations.empty(); };
        k.append = [](PacketTelemetry& t, const PacketTelemetry& x, double hz) {
            const double shift = t.timeSec - x.timeSec;   // the carriers' signal clocks started at different moments: bring x onto the primary's
            t.blocksOk += x.blocksOk; t.blocksBad += x.blocksBad; t.ok1200 += x.ok1200; t.bad1200 += x.bad1200; t.ok9600 += x.ok9600; t.bad9600 += x.bad9600;
            t.aprsFrames += x.aprsFrames; t.carrier = t.carrier || x.carrier;
            t.last1200Sec = std::max(t.last1200Sec, x.last1200Sec + shift); t.last9600Sec = std::max(t.last9600Sec, x.last9600Sec + shift); t.lastAprsSec = std::max(t.lastAprsSec, x.lastAprsSec + shift);
            for (PacketFrameInfo f : x.frames) { f.timeSec += shift; f.carrierId = 1; f.carrierHz = hz; t.frames.push_back(std::move(f)); }
            std::stable_sort(t.frames.begin(), t.frames.end(), [](const PacketFrameInfo& a, const PacketFrameInfo& b) { return a.timeSec < b.timeSec; });
            if (t.frames.size() > 200) t.frames.erase(t.frames.begin(), t.frames.end() - 200);
            for (PacketStation s : x.stations) {   // one row per callsign: the newest hearing, the frames of all
                s.lastHeardSec += shift; s.carrierId = 1; s.carrierHz = hz;
                auto it = std::find_if(t.stations.begin(), t.stations.end(), [&](const PacketStation& q) { return q.call == s.call; });
                if (it == t.stations.end()) t.stations.push_back(std::move(s));
                else { const uint32_t n = it->count + s.count; if (s.lastHeardSec > it->lastHeardSec) *it = std::move(s); it->count = n; }
            }
            std::stable_sort(t.stations.begin(), t.stations.end(), [](const PacketStation& a, const PacketStation& b) { return a.lastHeardSec > b.lastHeardSec; });
            if (t.stations.size() > 300) t.stations.resize(300);
        };
        k.info = [](const PacketTelemetry& t) {
            if (!t.blocksOk) return std::string();
            char b[80]; snprintf(b, sizeof b, "%s%s, %llu frames", t.ok1200 ? "1200" : "", t.ok9600 ? (t.ok1200 ? " + 9600" : "9600") : "", (unsigned long long)t.blocksOk);
            return std::string(b);
        };
        packet.reset(new WideBank<PacketReceiver, PacketTelemetry>(k));
    }
};

WideModes::WideModes() : p_(new Impl()) {}
WideModes::~WideModes() = default;

bool WideModes::supports(int m) { return m == 10 || m == 32 || m == 34 || m == 39 || m == 46 || m == 51 || m == 24 || m == 25; }

CarrierCfg WideModes::cfgFor(int m) {
    CarrierCfg c;
    c.stepHz = m == 46 ? 6250.0 : m == 24 || m == 25 ? 25000.0 : 12500.0;
    c.maxCarriers = 8;
    if (m == 24 || m == 25) { c.debounceSec = 0.12; c.holdSec = 30; }   // pages come in short bursts
    return c;
}

void WideModes::configure(int m, double rate, double origin) {
    CarrierCfg c = cfgFor(m); c.gridOriginHz = origin;
    switch (m) {
    case 10: p_->dmr->configure(rate, c); break;
    case 32: p_->m17->configure(rate, c); break;
    case 34: p_->dstar->configure(rate, c); break;
    case 39: p_->nxdn->configure(rate, c); break;
    case 46: p_->dpmr->configure(rate, c); break;
    case 51: p_->p25->configure(rate, c); break;
    case 24: p_->pager->configure(rate, c); break;
    case 25: p_->packet->configure(rate, c); break;
    }
}

void WideModes::reset() { p_->dmr->reset(); p_->m17->reset(); p_->dstar->reset(); p_->nxdn->reset(); p_->dpmr->reset(); p_->p25->reset(); p_->pager->reset(); p_->packet->reset(); }
void WideModes::clearResults() { p_->dmr->clearResults(); p_->m17->clearResults(); p_->dstar->clearResults(); p_->nxdn->clearResults(); p_->dpmr->clearResults(); p_->p25->clearResults(); p_->pager->clearResults(); p_->packet->clearResults(); }

void WideModes::feed(int m, const cf32* x, size_t n) {
    switch (m) {
    case 10: p_->dmr->feed(x, n); break;
    case 32: p_->m17->feed(x, n); break;
    case 34: p_->dstar->feed(x, n); break;
    case 39: p_->nxdn->feed(x, n); break;
    case 46: p_->dpmr->feed(x, n); break;
    case 51: p_->p25->feed(x, n); break;
    case 24: p_->pager->feed(x, n); break;
    case 25: p_->packet->feed(x, n); break;
    }
}

size_t WideModes::liveCount(int m) const {
    switch (m) {
    case 10: return p_->dmr->liveCount();
    case 32: return p_->m17->liveCount();
    case 34: return p_->dstar->liveCount();
    case 39: return p_->nxdn->liveCount();
    case 46: return p_->dpmr->liveCount();
    case 51: return p_->p25->liveCount();
    case 24: return p_->pager->liveCount();
    case 25: return p_->packet->liveCount();
    }
    return 0;
}

std::vector<double> WideModes::liveHz(int m) const {
    switch (m) {
    case 10: return p_->dmr->liveHz();
    case 32: return p_->m17->liveHz();
    case 34: return p_->dstar->liveHz();
    case 39: return p_->nxdn->liveHz();
    case 46: return p_->dpmr->liveHz();
    case 51: return p_->p25->liveHz();
    case 24: return p_->pager->liveHz();
    case 25: return p_->packet->liveHz();
    }
    return {};
}

bool WideModes::ready(int m) const {
    switch (m) {
    case 10: return p_->dmr->ready();
    case 32: return p_->m17->ready();
    case 34: return p_->dstar->ready();
    case 39: return p_->nxdn->ready();
    case 46: return p_->dpmr->ready();
    case 51: return p_->p25->ready();
    case 24: return p_->pager->ready();
    case 25: return p_->packet->ready();
    }
    return false;
}

void WideModes::setSelected(int id) {
    p_->sel = id;
    p_->dmr->setSelected(id); p_->m17->setSelected(id); p_->dstar->setSelected(id); p_->nxdn->setSelected(id); p_->dpmr->setSelected(id); p_->p25->setSelected(id); p_->pager->setSelected(id); p_->packet->setSelected(id);
}

void WideModes::setMuted(bool m) {
    p_->muted = m;
    p_->dmr->setMuted(m); p_->m17->setMuted(m); p_->dstar->setMuted(m); p_->nxdn->setMuted(m); p_->dpmr->setMuted(m); p_->p25->setMuted(m); p_->pager->setMuted(m); p_->packet->setMuted(m);
}

void WideModes::setVolume(float v) {
    p_->volume = v;
    const std::function<void(DmrReceiver&)> fd = [v](DmrReceiver& r) { r.setVolume(v); };
    p_->dmr->forEach(fd);
    p_->m17->forEach([v](M17Receiver& r) { r.setVolume(v); });
    p_->dstar->forEach([v](DstarReceiver& r) { r.setVolume(v); });
    p_->nxdn->forEach([v](NxdnReceiver& r) { r.setVolume(v); });
    p_->dpmr->forEach([v](DpmrReceiver& r) { r.setVolume(v); });
    p_->p25->forEach([v](P25Receiver& r) { r.setVolume(v); });
}

void WideModes::setCenterHz(double hz) {
    p_->dmr->setCenterHz(hz); p_->m17->setCenterHz(hz); p_->dstar->setCenterHz(hz); p_->nxdn->setCenterHz(hz); p_->dpmr->setCenterHz(hz); p_->p25->setCenterHz(hz); p_->pager->setCenterHz(hz); p_->packet->setCenterHz(hz);
}

void WideModes::setPagerOptions(int protocols, int bauds) {
    if (protocols == p_->pagerProtos && bauds == p_->pagerBauds) return;
    p_->pagerProtos = protocols; p_->pagerBauds = bauds;
    p_->pager->forEach([&](PagerReceiver& r) { r.setProtocols(protocols); r.setPocsagBauds(bauds); });
}

// The carriers stay 25 kHz apart in the bank (WideBank steps by stepHz); the width only narrows the filter inside each carrier's receiver
void WideModes::setNarrowChannels(bool pager, bool packet) {
    if (pager != p_->pagerNarrow) { p_->pagerNarrow = pager; p_->pager->forEach([&](PagerReceiver& r) { r.setNarrowChannel(pager); }); }
    if (packet != p_->packetNarrow) { p_->packetNarrow = packet; p_->packet->forEach([&](PacketReceiver& r) { r.setNarrowChannel(packet); }); }
}

void WideModes::setLogCallback(std::function<void(const std::string&)> cb) { p_->log = std::move(cb); }

namespace {
template <class Bank, class Tel> bool fillTel(Bank& b, uint64_t& lastSeq, RxTelemetry& t, int stdm, Tel& slot, bool ready) {
    Tel mt; uint64_t seq = 0;
    if (!b.snapshot(mt, t.wide, lastSeq, &seq)) return false;
    lastSeq = seq;
    t.standard = stdm; t.seq = seq; t.state = mt.state; t.cfoHz = mt.cfoHz; t.dataValid = mt.dataValid; t.dataSnrDb = mt.snrDb;
    t.blocksOk = mt.blocksOk; t.blocksBad = mt.blocksBad; t.rateOk = ready;
    slot = std::move(mt);
    return true;
}
} // namespace

bool WideModes::telemetry(int m, RxTelemetry& t, uint64_t& lastSeq) {
    switch (m) {
    case 10: return fillTel(*p_->dmr, lastSeq, t, m, t.dmr, p_->dmr->ready());
    case 32: return fillTel(*p_->m17, lastSeq, t, m, t.m17, p_->m17->ready());
    case 34: return fillTel(*p_->dstar, lastSeq, t, m, t.dstar, p_->dstar->ready());
    case 39: return fillTel(*p_->nxdn, lastSeq, t, m, t.nxdn, p_->nxdn->ready());
    case 46: return fillTel(*p_->dpmr, lastSeq, t, m, t.dpmr, p_->dpmr->ready());
    case 51: return fillTel(*p_->p25, lastSeq, t, m, t.p25, p_->p25->ready());
    case 24: return fillTel(*p_->pager, lastSeq, t, m, t.pager, p_->pager->ready());
    case 25: return fillTel(*p_->packet, lastSeq, t, m, t.packet, p_->packet->ready());
    }
    return false;
}

} // namespace dect2
