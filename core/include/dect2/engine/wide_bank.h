// "Whole span" decoding for the narrowband modes: one single-channel receiver per carrier the finder (carrierfind.h) found in the captured span.
// Every receiver is the mode's ordinary class, fed the carrier's own baseband stream by a Channelizer; no state is shared between them.
//
//   feed(span) -> CarrierFinder -> per carrier: Channelizer -> Rx::feed
//   snapshot() -> the selected carrier's telemetry, with the log of every carrier (also the ones that have gone) merged in, and the carriers table
//
// Threads: the analysis thread calls feed(); the per-carrier work (Channelizer + Rx::feed, the expensive part) of the live carriers then runs in
// parallel on the shared pool (ThreadPool::shared().parallelFor, the analysis thread helps and waits). Each slot touches only its own receiver, so
// the output is the same as a sequential loop; the lines the receivers log meanwhile are caught per carrier (WideLogCapture) and passed on in
// carrier order after the join, so the log is deterministic too. DECT2_THREADS=0 runs the plain loop.
//
// Template over the receiver and its telemetry so every mode uses the same machinery; the mode supplies a few hooks (WideOps).
#pragma once
#include "dect2/dsp/carrierfind.h"
#include "dect2/common/threadpool.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

// One row of the carriers table (shared by every mode)
struct WideCarrier {
    int id = 0;
    double hz = 0;               // frequency relative to the centre of the span
    double levelDb = -200;       // dBFS-like level of the cell
    double snrDb = 0;            // above the noise floor
    bool active = false;         // above the threshold now (else only held)
    bool selected = false;       // the one that is listened to and shown in detail
    int state = 0;               // 0 searching, 1 partly locked, 2 locked (the decoder's own state)
    std::string info;            // what is known: colour code / RAN / NAC / system / talkgroup ...
};

// While a pool job feeds one carrier's receiver, the lines that receiver logs go to this buffer (thread-local) instead of the engine's log;
// WideBank::feed replays them in carrier order. A mode's log callback must route through WideLogCapture::sink() first.
// Process-wide switch (default on) for the parallel per-carrier feed: tests and benchmarks turn it off to compare with the sequential loop.
struct WideParallel {
    static std::atomic<bool>& enabled() { static std::atomic<bool> e{true}; return e; }
};

struct WideLogCapture {
    static std::vector<std::string>*& current() { static thread_local std::vector<std::string>* c = nullptr; return c; }
    // true when the line was captured (the caller must not log it itself)
    static bool sink(const std::string& line) { auto* c = current(); if (!c) return false; c->push_back(line); return true; }
};

template <class Rx, class Tel>
struct WideOps {
    double outRate = 48000;                                  // the rate the receiver wants
    double occupiedHz = 9000;                                // bandwidth the channel filter keeps
    std::function<void(const std::string&)> log;             // optional: where the captured log lines go after a parallel feed (the same sink the receivers' callback uses)
    std::function<void(Rx&)> setup;                          // after configure: silent, no offset, log callback
    std::function<void(Rx&, bool play)> audio;               // play = this is the listened-to carrier
    std::function<void(Rx&)> clear;                          // clearResults
    std::function<void(Tel& t)> clearLists;                  // empty the call / message lists of t
    std::function<void(Tel& dst, const Tel& src, double hz)> append;   // append src's list entries (stamped with the carrier frequency hz) to dst
    std::function<std::string(const Tel&)> info;             // short text for the table
    double preRollSec = 0;                                   // > 0: a carrier found late gets this much of the span from before it was found (bursty signals: the start of the transmission)
    std::function<double(const Tel&)> freqCorrection;        // optional: Hz to add to the finder's frequency of the carrier (the decoder's own carrier-offset measurement)
    std::function<bool(const Tel&)> hasLog;                  // optional: false = this telemetry has no list entries, a carrier that goes is not kept in the log
    std::function<int(const Tel&)> rowState;                 // optional: the table's state (0 searching, 1 partly locked, 2 locked) when the telemetry's own state is not that scale
};

template <class Rx, class Tel>
class WideBank {
public:
    WideBank(WideOps<Rx, Tel> ops) : ops_(std::move(ops)) {}

    // spanRate: the sample rate of what feed() gets; the raster cfg is the mode's
    void configure(double spanRate, const CarrierCfg& cfg) {
        std::lock_guard<std::mutex> g(m_);
        spanRate_ = spanRate; cfg_ = cfg;
        finder_.configure(spanRate, cfg);
        hist_.clear();
        live_.clear(); retired_.clear(); sel_ = 0; seq_ = 1;
    }
    bool ready() const { return spanRate_ >= ops_.outRate; }
    void reset() { std::lock_guard<std::mutex> g(m_); finder_.reset(); hist_.clear(); retireAll(); sel_ = 0; seq_++; }   // after a retune: the carriers are new, the logs stay
    // The radio's centre frequency (any thread; takes effect with the next feed, on the analysis thread). A carrier that has gone keeps its place
    // in the log by its absolute frequency, so a retune does not move the old rows.
    void setCenterHz(double hz) { pendCenter_.store(hz); }
    // runs f on every live receiver (a setting that changes while carriers are tracked)
    void forEach(const std::function<void(Rx&)>& f) { std::lock_guard<std::mutex> g(m_); for (auto& s : live_) f(*s->rx); }
    void clearResults() {
        std::lock_guard<std::mutex> g(m_);
        retired_.clear();
        for (auto& s : live_) { if (ops_.clear) ops_.clear(*s->rx); s->tel = Tel(); s->telSeq = 0; }
        seq_++;
    }
    void setSelected(int id) { std::lock_guard<std::mutex> g(m_); sel_ = id; applyAudio(); seq_++; }
    void setMuted(bool m) { std::lock_guard<std::mutex> g(m_); muted_ = m; applyAudio(); }
    int selected() const { std::lock_guard<std::mutex> g(m_); return sel_; }
    size_t liveCount() const { std::lock_guard<std::mutex> g(m_); return live_.size(); }

    void feed(const cf32* x, size_t n) {
        std::lock_guard<std::mutex> g(m_);
        if (spanRate_ <= 0) return;
        const double pc = pendCenter_.load();
        if (pc != center_) { retireAll(); finder_.reset(); hist_.clear(); sel_ = 0; center_ = pc; seq_++; }   // retuned without a reset: the old carriers are gone
        finder_.feed(x, n);
        for (int id : finder_.takeRemoved()) retire(id);
        for (const Carrier& c : finder_.carriers()) {
            Slot* s = find(c.id);
            if (!s) s = add(c);
            if (s) { s->car = c; }
        }
        feedCarriers(x, n);
        if (ops_.preRollSec > 0) {   // keep the last preRollSec of the span (before this block) for a carrier found later
            const size_t cap = (size_t)(ops_.preRollSec * spanRate_);
            hist_.insert(hist_.end(), x, x + n);
            if (hist_.size() > cap) hist_.erase(hist_.begin(), hist_.end() - (long)cap);
        }
    }

    // The selected carrier's telemetry (the strongest locked one when none is chosen), the logs of all merged in, and the table.
    // Returns false when nothing changed since lastSeq.
    bool snapshot(Tel& primary, std::vector<WideCarrier>& rows, uint64_t lastSeq, uint64_t* seqOut) {
        std::lock_guard<std::mutex> g(m_);
        bool changed = false;
        for (auto& s : live_) { Tel t; if (s->rx->telemetry(t, s->telSeq)) { s->telSeq = t.seq; s->tel = std::move(t); changed = true; } }
        if (changed) seq_++;
        if (seq_ == lastSeq) return false;
        *seqOut = seq_;
        rows.clear();
        Slot* best = nullptr;
        for (auto& s : live_) {
            WideCarrier r; r.id = s->car.id; r.hz = hzOf(*s); r.levelDb = s->car.levelDb; r.snrDb = s->car.snrDb; r.active = s->car.active;
            r.state = ops_.rowState ? ops_.rowState(s->tel) : s->tel.state; r.info = ops_.info ? ops_.info(s->tel) : std::string();
            rows.push_back(r);
            if (s->car.id == sel_) best = s.get();
        }
        if (!best) {   // none chosen: the best locked one, else the strongest
            for (auto& s : live_) {
                auto rank = [](const Slot& q) { return q.tel.state * 1000.0 + q.car.levelDb; };
                if (!best || rank(*s) > rank(*best)) best = s.get();
            }
        }
        if (sel_ == 0 && ops_.audio) {   // none chosen: the sound follows the best locked carrier too (it used to stay silent until a row was clicked); it stays on its carrier while that is as good as the best
            Slot* cur = nullptr;
            for (auto& s : live_) if (s->car.id == autoPlay_) cur = s.get();
            const int want = (cur && best && cur->tel.state >= best->tel.state) ? autoPlay_ : (best ? best->car.id : 0);
            if (want != autoPlay_) { autoPlay_ = want; applyAudio(); }
        } else if (autoPlay_ != 0) autoPlay_ = 0;
        Tel p = best ? best->tel : Tel();
        // the logs: carriers that have gone first, then the live ones (each list keeps its own order)
        if (ops_.append) {
            if (ops_.clearLists) ops_.clearLists(p);
            for (auto& r : retired_) ops_.append(p, r.tel, r.hz - center_);
            for (auto& s : live_) ops_.append(p, s->tel, hzOf(*s));
        }
        p.seq = seq_;
        primary = std::move(p);
        for (auto& r : rows) r.selected = best && r.id == best->car.id;
        return true;
    }

    const CarrierFinder& finder() const { return finder_; }
    // test hook: the receivers' frequencies in the span
    std::vector<double> liveHz() const { std::lock_guard<std::mutex> g(m_); std::vector<double> v; for (auto& s : live_) v.push_back(s->car.hz); return v; }

private:
    struct Slot { Carrier car; std::unique_ptr<Rx> rx; Channelizer ch; Tel tel; uint64_t telSeq = 0; std::vector<cf32> scratch; std::vector<std::string> log; };
    struct Retired { int id; double hz; Tel tel; };   // hz: relative to the span centre plus the centre of that time (absolute)
    // Channelizer + receiver of every live carrier. Two or more carriers: in parallel on the shared pool, each slot with its own scratch and log buffer.
    void feedCarriers(const cf32* x, size_t n) {
        ThreadPool& pool = ThreadPool::shared();
        if (live_.size() < 2 || pool.workers() == 0 || n < 4096 || !WideParallel::enabled().load(std::memory_order_relaxed)) {
            std::vector<cf32> out;
            for (auto& s : live_) {
                out.clear();
                s->ch.process(x, n, out);
                if (!out.empty()) s->rx->feed(out.data(), out.size());
            }
            return;
        }
        pool.parallelFor(live_.size(), [&](size_t i) {
            Slot& s = *live_[i];
            s.log.clear();
            WideLogCapture::current() = &s.log;
            s.scratch.clear();
            s.ch.process(x, n, s.scratch);
            if (!s.scratch.empty()) s.rx->feed(s.scratch.data(), s.scratch.size());
            WideLogCapture::current() = nullptr;
        });
        if (ops_.log) for (auto& s : live_) for (const std::string& l : s->log) ops_.log(l);
    }
    Slot* find(int id) { for (auto& s : live_) if (s->car.id == id) return s.get(); return nullptr; }
    Slot* add(const Carrier& c) {
        auto s = std::make_unique<Slot>();
        s->car = c;
        s->rx.reset(new Rx());
        s->rx->configure(ops_.outRate);
        if (ops_.setup) ops_.setup(*s->rx);
        if (!s->ch.configure(spanRate_, ops_.outRate, c.hz, ops_.occupiedHz)) return nullptr;
        if (ops_.audio) ops_.audio(*s->rx, !muted_ && c.id == effSel());
        if (!hist_.empty()) {   // the start of the transmission that made the finder notice the carrier
            std::vector<cf32> out;
            s->ch.process(hist_.data(), hist_.size(), out);
            if (!out.empty()) s->rx->feed(out.data(), out.size());
        }
        live_.push_back(std::move(s));
        seq_++;
        return live_.back().get();
    }
    void retire(int id) {
        for (size_t i = 0; i < live_.size(); i++) if (live_[i]->car.id == id) {
            Tel t; if (live_[i]->rx->telemetry(t, 0)) live_[i]->tel = std::move(t);
            if (!ops_.hasLog || ops_.hasLog(live_[i]->tel)) retired_.push_back(Retired{id, hzOf(*live_[i]) + center_, std::move(live_[i]->tel)});
            if (retired_.size() > 64) retired_.pop_front();
            if (sel_ == id) sel_ = 0;
            live_.erase(live_.begin() + (long)i); seq_++; return;
        }
    }
    double hzOf(const Slot& s) const {   // the finder's frequency, refined by what the decoder measured on the carrier
        const double c = ops_.freqCorrection ? ops_.freqCorrection(s.tel) : 0.0;
        return s.car.hz + (std::fabs(c) < 0.3 * cfg_.stepHz ? c : 0.0);
    }
    void retireAll() { while (!live_.empty()) retire(live_.back()->car.id); }
    void applyAudio() { if (!ops_.audio) return; for (auto& s : live_) ops_.audio(*s->rx, !muted_ && s->car.id == effSel()); }
    int effSel() const { return sel_ ? sel_ : autoPlay_; }   // the carrier whose sound plays: the chosen one, else the best locked one
    WideOps<Rx, Tel> ops_;
    mutable std::mutex m_;
    double spanRate_ = 0;
    CarrierCfg cfg_;
    CarrierFinder finder_;
    std::vector<std::unique_ptr<Slot>> live_;
    std::deque<Retired> retired_;
    int sel_ = 0;
    int autoPlay_ = 0;   // the carrier that plays while none is chosen (sel_ == 0)
    bool muted_ = false;
    uint64_t seq_ = 1;
    std::vector<cf32> hist_;
    double center_ = 0;
    std::atomic<double> pendCenter_{0};
};

} // namespace dect2
