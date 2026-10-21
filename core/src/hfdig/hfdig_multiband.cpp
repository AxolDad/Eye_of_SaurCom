// Multi-band monitor (see hfdig_multiband.h).
#include "dect2/hfdig/hfdig_multiband.h"
#include "dect2/hfdig/hfdig_wide.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

namespace dect2 {

namespace {
const MbBand kBands[kMbBands] = {
    {"160 m", 1810000, 2000000},   {"80 m", 3500000, 3800000},    {"60 m", 5351500, 5366500},  {"40 m", 7000000, 7200000},
    {"30 m", 10100000, 10150000},  {"20 m", 14000000, 14350000},  {"17 m", 18068000, 18168000}, {"15 m", 21000000, 21450000},
    {"12 m", 24890000, 24990000},  {"10 m", 28000000, 29700000},  {"6 m", 50000000, 54000000},
};
// WSJT-X default dial frequencies (data/frequencies/hf.tsv); WSPR: the centre of the 200 Hz sub-band minus 1500 Hz
const double kDial[kMbBands][kMbModes] = {
    {1840000, 0, 0, 1836600},        {3573000, 3575000, 0, 3568600},   {5357000, 0, 0, 5364700},
    {7074000, 7047500, 0, 7038600},  {10136000, 10140000, 0, 10138700}, {14074000, 14080000, 14084000, 14095600},
    {18100000, 18104000, 0, 18104600}, {21074000, 21140000, 0, 21094600}, {24915000, 24919000, 0, 24924600},
    {28074000, 28180000, 0, 28124600}, {50313000, 50318000, 0, 50293000},
};
double halfUsable(double rate) { return rate * kMbUsableFrac / 2; }
}

const MbBand& mbBand(int b) { return kBands[std::max(0, std::min(kMbBands - 1, b))]; }
double mbDialHz(int band, int mode) {
    if (band < 0 || band >= kMbBands || mode < 0 || mode >= kMbModes) return 0;
    return kDial[band][mode];
}
int mbBandOf(double hz) {
    for (int b = 0; b < kMbBands; b++) if (hz >= kBands[b].loHz - 3000 && hz <= kBands[b].hiHz) return b;
    return -1;
}
uint64_t mbMask(const std::vector<MbSel>& s) {
    uint64_t m = 0;
    for (const MbSel& x : s) if (x.band >= 0 && x.band < kMbBands && x.mode >= 0 && x.mode < kMbModes) m |= 1ull << (x.band * kMbModes + x.mode);
    return m;
}
std::vector<MbSel> mbFromMask(uint64_t m) {
    std::vector<MbSel> r;
    for (int b = 0; b < kMbBands; b++) for (int mo = 0; mo < kMbModes; mo++) if ((m >> (b * kMbModes + mo)) & 1) r.push_back({b, mo});
    return r;
}

std::vector<MbCover> mbCoverage(double centre, double rate, const std::vector<MbSel>& sels) {
    std::vector<MbCover> r;
    const double half = halfUsable(rate);
    for (const MbSel& s : sels) {
        MbCover c; c.sel = s; c.dialHz = mbDialHz(s.band, s.mode);
        if (c.dialHz > 0) {
            c.offsetHz = c.dialHz - centre;
            c.covered = rate > 0 && std::fabs(c.offsetHz) + kMbAudioHz <= half;
            if (!c.covered) c.neededSpanHz = 2 * (std::fabs(c.offsetHz) + kMbAudioHz) / kMbUsableFrac;
        }
        r.push_back(c);
    }
    return r;
}

double mbBestCentre(double rate, const std::vector<MbSel>& sels, int* covered) {
    std::vector<double> d;
    for (const MbSel& s : sels) if (const double f = mbDialHz(s.band, s.mode); f > 0) d.push_back(f);
    if (d.empty()) { if (covered) *covered = 0; return 0; }
    std::sort(d.begin(), d.end());
    const double w = 2 * (halfUsable(rate) - kMbAudioHz);      // the dials of one centre lie inside a window this wide
    int best = 0; double bc = d[0];
    if (w >= 0) {
        for (size_t i = 0; i < d.size(); i++) {
            size_t j = i;
            while (j + 1 < d.size() && d[j + 1] - d[i] <= w) j++;
            const int n = (int)(j - i + 1);
            if (n > best) { best = n; bc = 0.5 * (d[i] + d[j]); }
        }
    }
    if (covered) *covered = best;
    return bc;
}

double mbNeededRate(const std::vector<MbSel>& sels) {
    double lo = 1e18, hi = 0;
    for (const MbSel& s : sels) if (const double f = mbDialHz(s.band, s.mode); f > 0) { lo = std::min(lo, f); hi = std::max(hi, f); }
    if (hi <= 0) return 0;
    return 2 * (0.5 * (hi - lo) + kMbAudioHz) / kMbUsableFrac;
}

bool MbCpuGuard::update(double busy, double block) {
    if (block <= 0) return false;
    const double x = busy / block;
    load_ = load_ == 0 ? x : 0.8 * load_ + 0.2 * x;
    if (load_ > limit_) over_++; else { over_ = 0; slow_ = false; }
    if (over_ > 0) slow_ = true;
    if (over_ >= n_) { over_ = 0; return true; }
    return false;
}

struct HfdigMultiBand::Impl {
    HfdigFtx& main;
    double rate = 0, centre = 0, reportDial = 0;
    std::vector<MbSel> wanted, active, notCovered, droppedSel;
    std::vector<std::unique_ptr<HfdigWide>> bands;
    std::vector<int> bandIds;
    MbCpuGuard guard;
    double (*clock)() = nullptr;
    explicit Impl(HfdigFtx& m) : main(m) {}

    static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    void build() {
        bands.clear(); bandIds.clear(); active.clear(); notCovered.clear();
        std::map<int, std::vector<MbSel>> perBand;
        for (const MbCover& c : mbCoverage(centre, rate, wanted)) {
            if (std::find(droppedSel.begin(), droppedSel.end(), c.sel) != droppedSel.end()) continue;
            if (c.covered) { active.push_back(c.sel); perBand[c.sel.band].push_back(c.sel); }
            else notCovered.push_back(c.sel);
        }
        for (auto& kv : perBand) {
            std::vector<FtxSlot> slots;
            double lo = 1e18, hi = 0;
            for (const MbSel& s : kv.second) { const double f = mbDialHz(s.band, s.mode); slots.push_back({f, s.mode}); lo = std::min(lo, f); hi = std::max(hi, f); }
            const double ref = 0.5 * (lo + hi);
            auto w = std::make_unique<HfdigWide>(main);
            w->configureSlots(rate, (hi - lo) + 2 * kMbAudioHz + 2000, ref, ref - centre, slots, reportDial > 0 ? reportDial : centre);
            bands.push_back(std::move(w));
            bandIds.push_back(kv.first);
        }
    }

    // drop the band that has produced the fewest decodes (ties: the highest band number)
    void dropOne() {
        if (bandIds.size() <= 1) return;
        HfdigFtxTelemetry t;
        main.telemetry(t);
        std::map<int, int> n;
        for (int b : bandIds) n[b] = 0;
        for (const FtxDecode& d : t.decodes)
            for (int b : bandIds) if (d.dialHz >= mbBand(b).loHz && d.dialHz <= mbBand(b).hiHz) n[b]++;
        int worst = bandIds.back();
        for (int b : bandIds) if (n[b] < n[worst] || (n[b] == n[worst] && b > worst)) worst = b;
        for (const MbSel& s : active) if (s.band == worst) droppedSel.push_back(s);
        build();
        guard.reset();
    }

    void step(double busy, double block) { if (guard.update(busy, block)) dropOne(); }
};

HfdigMultiBand::HfdigMultiBand(HfdigFtx& m) : p_(std::make_unique<Impl>(m)) {}
HfdigMultiBand::~HfdigMultiBand() = default;

void HfdigMultiBand::configure(double rate, double centre, const std::vector<MbSel>& sels, double reportDial) {
    Impl& m = *p_;
    m.reportDial = reportDial;
    m.rate = rate; m.centre = centre; m.wanted = sels; m.droppedSel.clear();
    m.guard.reset();
    m.build();
}

void HfdigMultiBand::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.bands.empty() || m.rate <= 0 || n == 0) return;
    const double t0 = m.clock ? m.clock() : Impl::now();
    for (auto& b : m.bands) b->feed(x, n);
    const double t1 = m.clock ? m.clock() : Impl::now();
    m.step(t1 - t0, (double)n / m.rate);
}

void HfdigMultiBand::setLoadClock(double (*clock)()) { p_->clock = clock; }
void HfdigMultiBand::testForceLoad(double busy, double block) { p_->step(busy, block); }

MbStatus HfdigMultiBand::status() const {
    const Impl& m = *p_;
    MbStatus s;
    s.centreHz = m.centre; s.rateHz = m.rate;
    s.bands = (int)m.bands.size();
    for (const auto& b : m.bands) s.channels += b->ftxChannels();
    s.dropped = (int)m.droppedSel.size();
    s.tooSlow = m.guard.tooSlow() || !m.droppedSel.empty();
    s.load = m.guard.load();
    s.active = m.active; s.notCovered = m.notCovered; s.droppedSel = m.droppedSel;
    return s;
}

} // namespace dect2
