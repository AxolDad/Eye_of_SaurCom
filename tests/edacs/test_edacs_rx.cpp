// EDACS: the BCH(40,28) code (its generator is the product of the two minimal polynomials, every one- and two-bit error corrected), the
// three-copy vote, the messages (parse / build round trip), the receiver on generated control channels (wide and narrowband, both data
// polarities found by the sync, noise), and a working channel (FM voice with the low-speed data, the drop burst at the end, ProVoice).
#include "dect2/edacs/edacs_proto.h"
#include "dect2/edacs/edacs_rx.h"
#include "dect2/edacs/edacs_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <set>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

namespace {

std::mt19937 rng(17);
float gauss() { static std::normal_distribution<float> nd(0.f, 1.f); return nd(rng); }

uint32_t gfMul(uint32_t a, uint32_t b) { uint32_t r = 0; for (int i = 0; i < 16; i++) if (b >> i & 1) r ^= a << i; return r; }

struct Run { EdacsTelemetry t; std::vector<uint32_t> msgs; std::vector<float> audio; };

Run run(const std::vector<cf32>& sig, double fs, double off, double sigma, bool narrow = false, bool voice = false, double extraSec = 0.3) {
    EdacsReceiver rx;
    rx.configure(fs);
    rx.setSignalOffset(off);
    rx.setNarrow(narrow);
    rx.setSilent(true);
    rx.setVoice(voice);
    Run R;
    rx.setAudioTap([&](const float* x, size_t n) { R.audio.insert(R.audio.end(), x, x + n); });
    uint64_t seq = 0, lastMsg = 0;
    std::vector<cf32> blk(4096);
    const size_t total = sig.size() + (size_t)(fs * extraSec);
    for (size_t i = 0; i < total; i += blk.size()) {
        const size_t n = std::min(blk.size(), total - i);
        for (size_t k = 0; k < n; k++) {
            cf32 v = i + k < sig.size() ? sig[i + k] : cf32(0, 0);
            if (sigma > 0) v += cf32((float)sigma * gauss(), (float)sigma * gauss());
            blk[k] = v;
        }
        rx.feed(blk.data(), n);
        EdacsTelemetry x;
        if (rx.telemetry(x, seq)) {
            seq = x.seq;
            for (const EdacsMsgRec& m : x.msgs) if (m.seq > lastMsg) { lastMsg = m.seq; R.msgs.push_back(m.data); }
            R.t = x;
        }
    }
    return R;
}

std::vector<uint8_t> lead(int n, unsigned seed) { std::vector<uint8_t> b; for (int i = 0; i < n; i++) { seed = seed * 1664525u + 1013904223u; b.push_back((uint8_t)((seed >> 20) & 1)); } return b; }

} // namespace

int main() {
    // ---------------------------------------------------------------- BCH(40,28)
    CHECK(gfMul(0x43, 0x57) == edacs::kBchGen, "the generator is m1(x) m3(x) = %X", gfMul(0x43, 0x57));
    {
        int bad = 0, n = 0;
        for (uint32_t d : {0x0000000u, 0xFFFFFFFu, 0x1234567u, 0xEE12345u, 0xABCDEF0u, 0x5555555u}) {
            const uint64_t cw = edacs::bchEncode(d);
            uint64_t w = cw;
            if (!edacs::bchDecode(w) || w != cw) bad++;
            for (int i = 0; i < 40; i++) for (int j = i; j < 40; j++) {
                w = cw ^ (1ull << i) ^ (i == j ? 0 : 1ull << j);
                int c = 0;
                n++;
                if (!edacs::bchDecode(w, &c) || w != cw || c != (i == j ? 1 : 2)) bad++;
            }
        }
        CHECK(bad == 0, "BCH: %d of %d one- and two-bit error patterns not corrected", bad, n);
        // three errors are never "corrected" back to the sent word
        int wrong = 0;
        for (int k = 0; k < 2000; k++) {
            const uint64_t cw = edacs::bchEncode(rng() & 0xFFFFFFF);
            std::set<int> pos; while (pos.size() < 3) pos.insert((int)(rng() % 40));
            uint64_t w = cw; for (int p : pos) w ^= 1ull << p;
            if (edacs::bchDecode(w) && w == cw) wrong++;
        }
        CHECK(wrong == 0, "three errors gave back the sent word %d times", wrong);
    }
    // ---------------------------------------------------------------- vote and messages
    {
        const auto f = edacs::encodeFrame(0xEE12345, 0xFC00000);
        CHECK((int)f.size() == edacs::kFrameBits, "frame bits %zu", f.size());
        uint64_t sync = 0; for (int i = 0; i < 48; i++) sync = sync << 1 | f[(size_t)i];
        CHECK(sync == edacs::kSync, "sync %012llX", (unsigned long long)sync);
        auto g = f;
        g[48 + 3] ^= 1; g[48 + 40 + 9] ^= 1; g[48 + 80 + 20] ^= 1;   // one bad bit in each copy, different places: outvoted
        edacs::Decoded d = edacs::decodeMessage(g.data() + 48);
        CHECK(d.ok && d.bchOk && d.corrected == 0 && d.disagree == 3 && d.data == 0xEE12345, "vote: data %07X bch %d", d.data, d.bchOk);
        g[48 + 40 + 3] ^= 1; g[48 + 40 + 20] ^= 1;   // the same two bits wrong in two copies: the vote is wrong there, the BCH corrects it
        d = edacs::decodeMessage(g.data() + 48);
        CHECK(d.ok && d.bchOk && d.corrected == 2 && d.data == 0xEE12345, "vote + BCH: data %07X corrected %d", d.data, d.corrected);
        // a word whose check bits are not the generator's but whose copies agree: used (copies only), not BCH good
        std::vector<uint8_t> h(120);
        const uint64_t w = (uint64_t)0x1234567 << 12 | 0xABC;
        for (int i = 0; i < 40; i++) { const int b = (int)(w >> (39 - i) & 1); h[(size_t)i] = (uint8_t)b; h[40 + (size_t)i] = (uint8_t)(b ^ 1); h[80 + (size_t)i] = (uint8_t)b; }
        d = edacs::decodeMessage(h.data());
        CHECK(d.ok && !d.bchOk && d.data == 0x1234567, "copies only: ok %d bch %d", d.ok, d.bchOk);
    }
    {
        using M = edacs::Message;
        M a; a.kind = M::GroupVoice; a.lcn = 7; a.group = 0x2A5; a.emergency = true;
        M b; b.kind = M::GroupVoice; b.update = true; b.lcn = 12; b.group = 100; b.digital = true;
        M c; c.kind = M::IndivVoice; c.lcn = 3; c.lid = 12345;
        M d; d.kind = M::Caller; d.lid = 4321;
        M e; e.kind = M::SystemId; e.sysId = 0xBEEF; e.site = 9;
        M f; f.kind = M::Idle;
        int bad = 0;
        for (const M& m : {a, b, c, d, e, f}) {
            const M p = edacs::parse(edacs::build(m));
            if (p.kind != m.kind || p.lcn != (m.kind == M::GroupVoice || m.kind == M::IndivVoice ? m.lcn : -1) || p.group != m.group || p.lid != m.lid ||
                p.digital != m.digital || p.emergency != m.emergency || p.update != m.update || p.sysId != m.sysId || p.site != m.site) {
                bad++;
                printf("  %s -> %s\n", edacs::describe(m).c_str(), edacs::describe(p).c_str());
            }
        }
        CHECK(bad == 0, "%d messages did not survive build / parse", bad);
        CHECK(edacs::afsText(0x2A5) == "05-045" && edacs::afsText(100) == "00-124", "AFS text %s %s", edacs::afsText(0x2A5).c_str(), edacs::afsText(100).c_str());
        CHECK(edacs::parse(0x1234567).kind == M::Unknown, "an unknown command stays unknown");
    }

    // ---------------------------------------------------------------- the receiver on a control channel
    using M = edacs::Message;
    auto controlBits = [&](int frames, std::vector<uint32_t>& sent) {
        std::vector<uint8_t> b = lead(400, 77);
        for (int k = 0; k < frames; k++) {
            M x; M y;
            if (k % 3 == 0) { x.kind = M::SystemId; x.sysId = 0x1A2B; x.site = 3; y.kind = M::Idle; }
            else if (k % 3 == 1) { x.kind = M::GroupVoice; x.lcn = 1 + k % 20; x.group = 0x100 + k; y.kind = M::Idle; }
            else { x.kind = M::IndivVoice; x.lcn = 4; x.lid = 1000 + k; y.kind = M::Caller; y.lid = 2000 + k; }
            const uint32_t wa = edacs::build(x), wb = edacs::build(y);
            if (k > 0) { sent.push_back(wa); sent.push_back(wb); }
            const auto f = edacs::encodeFrame(wa, wb);
            b.insert(b.end(), f.begin(), f.end());
        }
        auto t = lead(80, 91);
        b.insert(b.end(), t.begin(), t.end());
        return b;
    };
    auto got = [](const std::vector<uint32_t>& sent, const std::vector<uint32_t>& rx) {
        std::multiset<uint32_t> r(rx.begin(), rx.end());
        int n = 0;
        for (uint32_t w : sent) { auto it = r.find(w); if (it != r.end()) { n++; r.erase(it); } }
        return n;
    };
    const double fs = 480000, off = -100000;
    for (int inv = 0; inv < 2; inv++) {
        std::vector<uint32_t> sent;
        const auto bits = controlBits(12, sent);
        const Run R = run(edacs::modulate(bits, fs, off, 2400.0, 0.5f, inv), fs, off, 0.03);
        const int n = got(sent, R.msgs);
        printf("  control channel%s: %d of %zu messages, %zu received, BCH good %llu, copies only %llu, refused %llu, system %04X site %d, polarity %s\n",
               inv ? " (inverted)" : "", n, sent.size(), R.msgs.size(), (unsigned long long)R.t.bchOk, (unsigned long long)R.t.copiesOnly, (unsigned long long)R.t.msgBad,
               R.t.sysId, R.t.site, R.t.inverted ? "inverted" : "normal");
        CHECK(n == (int)sent.size() && R.msgs.size() <= sent.size() + 2, "control channel%s: %d of %zu messages", inv ? " (inverted)" : "", n, sent.size());
        CHECK(R.t.sysId == 0x1A2B && R.t.site == 3 && R.t.locked && R.t.inverted == (inv == 1), "system ID, lock and the polarity found by the sync");
        CHECK(R.t.copiesOnly == 0, "every message checked by its BCH");
    }
    {   // narrowband: 4800 bit/s
        std::vector<uint32_t> sent;
        const auto bits = controlBits(10, sent);
        const Run R = run(edacs::modulate(bits, fs, off, 1600.0, 0.5f, false, edacs::kBaudNarrow), fs, off, 0.03, true);
        const int n = got(sent, R.msgs);
        printf("  narrowband: %d of %zu messages\n", n, sent.size());
        CHECK(n == (int)sent.size(), "narrowband: %d of %zu messages", n, sent.size());
    }
    {   // noisy: what decodes is right
        std::vector<uint32_t> sent;
        const auto bits = controlBits(30, sent);
        const Run R = run(edacs::modulate(bits, fs, off, 2400.0, 0.5f), fs, off, 0.35);
        const int n = got(sent, R.msgs);
        std::set<uint32_t> ok(sent.begin(), sent.end());
        int stray = 0;
        for (uint32_t w : R.msgs) stray += !ok.count(w);
        printf("  noisy: %d of %zu messages, %d wrong, BCH corrections in use (good %llu, refused %llu)\n", n, sent.size(), stray, (unsigned long long)R.t.bchOk, (unsigned long long)R.t.msgBad);
        CHECK(n >= (int)sent.size() * 7 / 10 && stray == 0, "noisy: %d of %zu, %d wrong", n, sent.size(), stray);
    }
    {   // noise only: nothing
        const Run R = run(std::vector<cf32>((size_t)(fs * 2)), fs, off, 0.3);
        CHECK(R.t.blocksOk == 0 && R.msgs.empty() && !R.t.voice.carrier, "noise only: %llu frames, carrier %d", (unsigned long long)R.t.blocksOk, R.t.voice.carrier);
    }

    // ---------------------------------------------------------------- a working channel
    {
        const auto sig = edacs::workingChannel(fs, off, 2.0, 1000.0, 4, 0.5f);
        // the state while the voice is on: telemetry part way through
        EdacsReceiver rx;
        rx.configure(fs); rx.setSignalOffset(off); rx.setSilent(true); rx.setVoice(true);
        std::vector<float> audio;
        rx.setAudioTap([&](const float* x, size_t n) { audio.insert(audio.end(), x, x + n); });
        uint64_t seq = 0;
        bool carrier = false, lsd = false, provoice = false;
        uint64_t framesDuringVoice = 0;
        EdacsTelemetry t;
        for (size_t i = 0; i < sig.size(); i += 4096) {
            const size_t n = std::min<size_t>(4096, sig.size() - i);
            std::vector<cf32> b(sig.begin() + (long)i, sig.begin() + (long)(i + n));
            for (cf32& v : b) v += cf32(0.02f * gauss(), 0.02f * gauss());
            rx.feed(b.data(), n);
            if (rx.telemetry(t, seq)) {
                seq = t.seq;
                if (t.timeSec > 0.5 && t.timeSec < 1.9) { carrier |= t.voice.carrier; lsd |= t.lsd; provoice |= t.provoice; framesDuringVoice = t.blocksOk; }
            }
        }
        for (int k = 0; k < 30; k++) { std::vector<cf32> z(4096); for (cf32& v : z) v = cf32(0.3f * gauss(), 0.3f * gauss()); rx.feed(z.data(), z.size()); if (rx.telemetry(t, seq)) seq = t.seq; }
        // the tone in the sound: correlate with 1 kHz
        double c = 0, s = 0, e = 0;
        const size_t a0 = audio.size() / 4, a1 = audio.size() * 3 / 4;
        for (size_t i = a0; i < a1; i++) { const double ph = 2 * 3.14159265358979323846 * 1000.0 * (double)i / 48000.0; c += audio[i] * std::cos(ph); s += audio[i] * std::sin(ph); e += audio[i] * audio[i]; }
        const double frac = e > 0 ? 2 * (c * c + s * s) / ((double)(a1 - a0) * e) : 0;
        printf("  working channel: carrier %d, low-speed data %d (%.0f Hz), ProVoice %d, %zu audio samples (%.0f %% of the power at 1 kHz), drop frames %llu, carrier after %d\n",
               carrier, lsd, t.lsdHz, provoice, audio.size(), 100 * frac, (unsigned long long)t.blocksOk, t.voice.carrier);
        CHECK(carrier && lsd && !provoice, "working channel: carrier %d, low-speed data %d, ProVoice %d", carrier, lsd, provoice);
        CHECK(audio.size() > 48000 && frac > 0.8, "the voice tone plays (%.0f %% at 1 kHz), the low-speed data is filtered out", 100 * frac);
        CHECK(framesDuringVoice == 0 && t.blocksOk >= 2, "the drop burst at the end decodes (%llu frames, %llu during the voice)", (unsigned long long)t.blocksOk, (unsigned long long)framesDuringVoice);
        CHECK(!t.voice.carrier, "the carrier's end closes the squelch");
    }
    {
        const auto sig = edacs::workingChannel(fs, off, 2.0, 1000.0, 0, 0.5f, true);
        EdacsReceiver rx;
        rx.configure(fs); rx.setSignalOffset(off); rx.setSilent(true); rx.setVoice(true);
        uint64_t seq = 0;
        int pv = 0, reports = 0;
        for (size_t i = 0; i < sig.size(); i += 4096) {
            const size_t n = std::min<size_t>(4096, sig.size() - i);
            rx.feed(sig.data() + i, n);
            EdacsTelemetry t;
            if (rx.telemetry(t, seq)) { seq = t.seq; if (t.timeSec > 1.2) { reports++; pv += t.provoice; } }
        }
        CHECK(reports > 0 && pv == reports, "ProVoice detected in %d of %d reports", pv, reports);
    }
    return testSummary("edacs rx");
}
