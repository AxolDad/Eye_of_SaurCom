#include "dect2/common/thread_name.h"
#include "dect2/engine/multitv.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <thread>

namespace dect2 {

namespace {
constexpr int kNone = INT_MIN / 2;
constexpr double kQueueSec = 0.2;         // signal time that may wait for a receiver (a count of blocks was 9 ms at 60 Msps: any hiccup of a receiver lost samples)
constexpr size_t kQueueMin = 1u << 19;    // samples, at least
constexpr int kAbsentEvals = 8;           // evaluations (a quarter second each) without a signal before an idle receiver is dropped
inline uint64_t nowNs() { return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

using Block = std::shared_ptr<const std::vector<cf32>>;

struct MultiTv::Slot {
    ChannelSlotPos pos;
    MultiTv* owner = nullptr;
    ChannelFilter filter;
    std::unique_ptr<T2Receiver> t2;
    std::unique_ptr<DvbtReceiver> t;
    bool dvbt = false;
    // transport stream
    std::mutex tsMu;
    BbUnpacker unpack;
    TsDemux demux;
    TsSnapshot snap;
    int lastT2Frame = -1;
    std::vector<uint8_t> frameBuf;
    // the receiver's thread
    std::mutex qMu;
    std::condition_variable qCv;
    std::deque<Block> q;
    size_t qN = 0;   // samples in q
    bool stop = false, busy = false;
    std::thread th;
    std::atomic<uint64_t> busyNs{0}, dropped{0}, sigSamples{0};
    // polling (the owner's thread)
    uint64_t telSeq = 0;
    RxTelemetry tel;
    bool haveTel = false;
    uint64_t cpuBusy0 = 0, cpuSig0 = 0;
    float cpuPct = 0;
    int absent = 0;
    bool occupied = true;

    ~Slot() { t2.reset(); t.reset(); }   // the receivers' threads call back into this slot: they go first
    void deliver(const uint8_t* pk, size_t n, double secs) {
        TsSnapshot s;
        {
            std::lock_guard<std::mutex> lk(tsMu);
            for (size_t i = 0; i < n; i++) demux.feed(pk + i * 188);
            demux.advance(secs);
            snap = demux.snapshot();
            s = snap;
        }
        owner->mtvDeliver(pos.index, pk, n, secs, s);
    }
    void onPlp(const PlpResult& r) {
        std::unique_lock<std::mutex> lk(tsMu);
        frameBuf.clear();
        if (lastT2Frame >= 0) {
            const int mod = r.numT2Frames > 0 ? r.numT2Frames : 256;
            if (r.t2Frame != (lastT2Frame + std::max(1, r.frameInterval)) % mod) { unpack.lost(); demux.markLoss(); }
        }
        lastT2Frame = r.t2Frame;
        for (const auto& f : r.frames) {
            if (f.bits.empty()) { unpack.lost(); demux.markLoss(); continue; }
            unpack.push(f.bits, f.header);
        }
        demux.advance(r.frameSec);
        snap = demux.snapshot();
        TsSnapshot s = snap;
        std::vector<uint8_t> out;
        out.swap(frameBuf);
        lk.unlock();
        owner->mtvDeliver(pos.index, out.data(), out.size() / 188, r.frameSec, s);
    }
    void run() {
        std::vector<cf32> o;
        for (;;) {
            Block b;
            {
                std::unique_lock<std::mutex> lk(qMu);
                qCv.wait(lk, [&] { return stop || !q.empty(); });
                if (stop) return;
                b = std::move(q.front()); q.pop_front(); qN -= b->size();
                busy = true;
            }
            qCv.notify_all();
            const uint64_t t0 = nowNs();
            o.clear();
            filter.process(b->data(), b->size(), o);
            if (!o.empty()) { if (dvbt) t->feed(o.data(), o.size()); else t2->feed(o.data(), o.size()); }
            busyNs += nowNs() - t0;
            { std::lock_guard<std::mutex> lk(qMu); busy = false; }
            qCv.notify_all();
        }
    }
};

MultiTv::MultiTv() = default;
MultiTv::~MultiTv() { reset(); }

void MultiTv::setSink(Sink s) { std::lock_guard<std::mutex> lk(sinkMu_); sink_ = std::move(s); }

void MultiTv::mtvDeliver(int slot, const uint8_t* pk, size_t n, double secs, const TsSnapshot& snap) {
    if (slot != sel_.load() || !n) return;
    Sink f;
    { std::lock_guard<std::mutex> lk(sinkMu_); f = sink_; }
    if (f) f(slot, pk, n, secs, snap);
}

bool MultiTv::configure(const MultiTvOptions& o) {
    reset();
    o_ = o;
    gridOff_ = o.gridOffsetHz; gridFixed_ = !o.autoGrid;
    slots_ = spanSlots(o.spanRate, o.widthMhz * 1e6, gridOff_);
    scan_.configure(o.spanRate);
    queueLimit_ = std::max(kQueueMin, (size_t)(o.spanRate * kQueueSec));
    sel_ = kNone;
    return !slots_.empty() || o.autoGrid;
}

void MultiTv::stopSlot(size_t i) {
    Slot& s = *live_[i];
    { std::lock_guard<std::mutex> lk(s.qMu); s.stop = true; }
    s.qCv.notify_all();
    if (s.th.joinable()) s.th.join();
    live_.erase(live_.begin() + (long)i);
}

void MultiTv::reset() {
    std::vector<std::unique_ptr<Slot>> dead;
    {
        std::lock_guard<std::mutex> lk(mu_);
        dead.swap(live_);
    }
    for (auto& s : dead) {
        { std::lock_guard<std::mutex> lk(s->qMu); s->stop = true; }
        s->qCv.notify_all();
        if (s->th.joinable()) s->th.join();
    }
    dead.clear();   // receivers go (their own threads are joined in their destructors)
    scan_.reset();
    measured_.clear();
    sinceEval_ = 0;
    sel_ = kNone;
    lastSelPolled_ = kNone;
}

MultiTv::Slot* MultiTv::find(int slot) {
    for (auto& s : live_) if (s->pos.index == slot) return s.get();
    return nullptr;
}

void MultiTv::startSlot(const SlotPower& p) {
    auto s = std::make_unique<Slot>();
    s->pos = p.pos; s->owner = this; s->dvbt = o_.dvbt;
    const double outRate = tvChannelRate(o_.widthMhz, o_.spanRate);
    if (!s->filter.configure(o_.spanRate, p.pos.offsetHz, outRate)) return;
    Slot* raw = s.get();
    s->unpack.setSink([raw](const uint8_t* pkt) { raw->demux.feed(pkt); raw->frameBuf.insert(raw->frameBuf.end(), pkt, pkt + 188); });   // under tsMu (onPlp)
    if (o_.dvbt) {
        s->t = std::make_unique<DvbtReceiver>();
        s->t->configure(outRate, o_.widthMhz);
        s->t->setPacketCallback([raw](const uint8_t* pk, size_t n, double secs) { raw->deliver(pk, n, secs); });
    } else {
        s->t2 = std::make_unique<T2Receiver>();
        s->t2->configure(outRate, o_.widthMhz);
        s->t2->setPlpCallback([raw](const PlpResult& r) { raw->onPlp(r); });
    }
    s->th = dect2::namedThread("zorcom-mtv", [raw] { raw->run(); });
    std::lock_guard<std::mutex> lk(mu_);
    live_.push_back(std::move(s));
    if (sel_.load() == kNone) sel_ = p.pos.index;
}

void MultiTv::evaluate() {
    if (scan_.frames() < 12) return;
    if (!gridFixed_) {   // the capture is centred on a channel or between two: whichever fits more signals
        const double w = o_.widthMhz * 1e6;
        double best = -1, bestOff = 0;
        for (double off : {0.0, 0.5 * w}) {
            const auto sl = spanSlots(o_.spanRate, w, off);
            const auto m = scan_.measure(sl, w, o_.thresholdDb);
            double score = 0;
            for (const auto& p : m) if (p.occupied) score += 1000.0 + std::min(p.aboveDb, 60.0);
            if (score > best) { best = score; bestOff = off; }
        }
        gridOff_ = bestOff; gridFixed_ = true;
        slots_ = spanSlots(o_.spanRate, w, gridOff_);
    }
    auto m = scan_.measure(slots_, o_.widthMhz * 1e6, o_.thresholdDb);
    {
        std::lock_guard<std::mutex> lk(mu_);
        measured_ = m;
        for (auto& s : live_) {
            bool occ = false;
            for (const auto& p : m) if (p.pos.index == s->pos.index) occ = p.occupied;
            s->occupied = occ;
            s->absent = occ ? 0 : s->absent + 1;
        }
    }
    // drop receivers whose channel has gone and that never locked
    for (;;) {
        size_t victim = SIZE_MAX;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (size_t i = 0; i < live_.size(); i++)
                if (live_[i]->absent >= kAbsentEvals && !(live_[i]->haveTel && live_[i]->tel.state >= 2)) { victim = i; break; }
        }
        if (victim == SIZE_MAX) break;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const int idx = live_[victim]->pos.index;
            stopSlot(victim);
            if (sel_.load() == idx) sel_ = live_.empty() ? kNone : live_.front()->pos.index;
        }
    }
    // new receivers for the occupied slots, the strongest first, up to the cap
    std::vector<SlotPower> want;
    for (const auto& p : m) if (p.occupied) want.push_back(p);
    std::sort(want.begin(), want.end(), [](const SlotPower& a, const SlotPower& b) { return a.aboveDb > b.aboveDb; });
    for (const auto& p : want) {
        size_t cnt;
        bool have;
        { std::lock_guard<std::mutex> lk(mu_); cnt = live_.size(); have = find(p.pos.index) != nullptr; }
        if (have) continue;
        if ((int)cnt >= std::max(1, o_.maxChannels)) break;
        startSlot(p);
    }
}

void MultiTv::feed(const cf32* x, size_t n) {
    if (!n || o_.spanRate <= 0) return;
    scan_.feed(x, n);
    sinceEval_ += n;
    if (sinceEval_ >= (size_t)(o_.spanRate * 0.25)) { sinceEval_ = 0; evaluate(); }
    std::vector<Slot*> v;
    { std::lock_guard<std::mutex> lk(mu_); for (auto& s : live_) v.push_back(s.get()); }
    if (v.empty()) return;
    auto blk = std::make_shared<const std::vector<cf32>>(x, x + n);
    for (Slot* s : v) {
        s->sigSamples += n;
        std::unique_lock<std::mutex> lk(s->qMu);
        if (o_.blocking) s->qCv.wait(lk, [&] { return s->qN < queueLimit_ || s->stop; });
        else if (s->qN >= queueLimit_) { s->dropped += n; continue; }
        s->q.push_back(blk);
        s->qN += blk->size();
        lk.unlock();
        s->qCv.notify_all();
    }
}

void MultiTv::wait() {
    std::vector<Slot*> v;
    { std::lock_guard<std::mutex> lk(mu_); for (auto& s : live_) v.push_back(s.get()); }
    for (Slot* s : v) {
        std::unique_lock<std::mutex> lk(s->qMu);
        s->qCv.wait(lk, [&] { return (s->q.empty() && !s->busy) || s->stop; });
    }
}

void MultiTv::setSelected(int slot) {
    std::lock_guard<std::mutex> lk(mu_);
    if (find(slot)) sel_ = slot;
}

bool MultiTv::poll(RxTelemetry& out) {
    std::lock_guard<std::mutex> lk(mu_);
    bool selNew = false;
    const int sel = sel_.load();
    for (auto& sp : live_) {
        Slot& s = *sp;
        RxTelemetry t;
        if (s.dvbt ? s.t->telemetry(t, s.telSeq) : s.t2->telemetry(t, s.telSeq)) {
            s.telSeq = t.seq; s.tel = std::move(t); s.haveTel = true;
            if (s.pos.index == sel) selNew = true;
        }
        const uint64_t sig = s.sigSamples.load();
        if (sig - s.cpuSig0 >= (uint64_t)o_.spanRate) {   // about a second of signal
            const uint64_t busy = s.busyNs.load();
            const double secs = (double)(sig - s.cpuSig0) / o_.spanRate;
            s.cpuPct = (float)(100.0 * 1e-9 * (double)(busy - s.cpuBusy0) / std::max(secs, 1e-3));
            s.cpuBusy0 = busy; s.cpuSig0 = sig;
        }
    }
    if (sel != lastSelPolled_) { lastSelPolled_ = sel; selNew = true; }
    if (!selNew) return false;
    Slot* s = find(sel);
    if (!s || !s->haveTel) return false;
    out = s->tel;
    return true;
}

std::vector<TvChannelRow> MultiTv::channels() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<TvChannelRow> v;
    const int sel = sel_.load();
    for (const SlotPower& p : measured_) {
        TvChannelRow r;
        r.slot = p.pos.index; r.offsetHz = p.pos.offsetHz; r.levelDb = p.levelDb; r.aboveDb = p.aboveDb; r.occupied = p.occupied;
        if (Slot* s = find(p.pos.index)) {
            r.running = true; r.selected = s->pos.index == sel;
            r.cpuPct = s->cpuPct; r.dropped = s->dropped.load();
            if (s->haveTel) {
                r.state = s->tel.state; r.snrDb = s->tel.dataSnrDb;
                r.locked = s->tel.state >= 2 || s->tel.dvbt.fecSync || s->tel.dataValid;
            }
            std::lock_guard<std::mutex> tk(s->tsMu);
            r.services = (int)s->snap.services.size();
            r.network = s->snap.networkName;
        }
        v.push_back(r);
    }
    return v;
}

std::vector<MultiService> MultiTv::services() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<MultiService> v;
    std::vector<Slot*> order;
    for (auto& s : live_) order.push_back(s.get());
    std::sort(order.begin(), order.end(), [](Slot* a, Slot* b) { return a->pos.offsetHz < b->pos.offsetHz; });
    for (Slot* s : order) {
        std::lock_guard<std::mutex> tk(s->tsMu);
        for (const TsService& sv : s->snap.services) v.push_back(MultiService{s->pos.index, s->pos.offsetHz, sv});
    }
    return v;
}

TsSnapshot MultiTv::snapshot(int slot) {
    std::lock_guard<std::mutex> lk(mu_);
    Slot* s = find(slot);
    if (!s) return TsSnapshot();
    std::lock_guard<std::mutex> tk(s->tsMu);
    return s->snap;
}

std::map<int, std::vector<EpgEvent>> MultiTv::epg(int slot) {
    std::lock_guard<std::mutex> lk(mu_);
    Slot* s = find(slot);
    if (!s) return {};
    std::lock_guard<std::mutex> tk(s->tsMu);
    return s->demux.epg();
}

BbStats MultiTv::bbStats(int slot) {
    std::lock_guard<std::mutex> lk(mu_);
    Slot* s = find(slot);
    if (!s) return BbStats();
    std::lock_guard<std::mutex> tk(s->tsMu);
    return s->unpack.stats();
}

std::vector<int> MultiTv::runningSlots() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<int> v;
    for (auto& s : live_) v.push_back(s->pos.index);
    return v;
}

std::vector<SlotPower> MultiTv::lastMeasure() { std::lock_guard<std::mutex> lk(mu_); return measured_; }

} // namespace dect2
