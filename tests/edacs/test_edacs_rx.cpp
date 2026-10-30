// EDACS against DSD-FME's decoder (src/edacs-fme.c; ISC licence): the BCH(40,28) generator (our own textbook computation, with the
// parameters DSD-FME uses) equals the product of the two minimal polynomials, every one- and two-bit error is corrected; the vote; the messages: dsd-fme's two
// captured EA frames (F802180 0000000 a test call, FE00045 FE00045 a regroup) and frames written by hand from dsd-fme's field masks, for the
// standard / networked and the EA sets, with the ESK; the receiver on generated control channels (wide and narrowband, both polarities,
// noise), and a working channel (FM voice with the low-speed data, the end-of-transmission dotting, ProVoice).
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

struct Run { EdacsTelemetry t; std::vector<uint32_t> msgs; std::vector<float> audio; };   // msgs: m1, m2 of each frame

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
            for (const EdacsMsgRec& m : x.msgs) if (m.seq > lastMsg) { lastMsg = m.seq; R.msgs.push_back(m.m1); R.msgs.push_back(m.m2); }
            R.t = x;
        }
    }
    return R;
}

std::vector<uint8_t> lead(int n, unsigned seed) { std::vector<uint8_t> b; for (int i = 0; i < n; i++) { seed = seed * 1664525u + 1013904223u; b.push_back((uint8_t)((seed >> 20) & 1)); } return b; }

} // namespace

int main() {
    {   // frames this receiver decoded from real recordings (SigIDWiki "EDACS_96.mp3" and "EDACS96_Sound.mp3", 9600 bit/s control channels:
        // every message passed the BCH as received); the recordings are not redistributed, only these messages. In the standard set
        // they are the site ID messages (MT-A 7, MT-B 7, MT-D 8..B) of sites 1 and 2.
        const edacs::Message a = edacs::parse(0xFD04101, 0xFC40002, edacs::Mode::Standard);
        const edacs::Message b = edacs::parse(0xFD41102, 0xFC40003, edacs::Mode::Standard);
        CHECK(a.kind == edacs::Message::SiteId && a.site == 1 && a.ccLcn == 4, "real frame FD04101: kind %d site %d LCN %d", (int)a.kind, a.site, a.ccLcn);
        CHECK(b.kind == edacs::Message::SiteId && b.site == 2 && b.ccLcn == 1, "real frame FD41102: kind %d site %d LCN %d", (int)b.kind, b.site, b.ccLcn);
    }

    // ---------------------------------------------------------------- BCH(40,28)
    CHECK(edacs::bchGenerator() == gfMul(0x43, 0x57) && edacs::bchGenerator() == 0x1539, "the generator %X = m1 m3", edacs::bchGenerator());
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
        int wrong = 0;
        for (int k = 0; k < 2000; k++) {
            const uint64_t cw = edacs::bchEncode(rng() & 0xFFFFFFF);
            std::set<int> pos; while (pos.size() < 3) pos.insert((int)(rng() % 40));
            uint64_t w = cw; for (int p : pos) w ^= 1ull << p;
            if (edacs::bchDecode(w) && w == cw) wrong++;
        }
        CHECK(wrong == 0, "three errors gave back the sent word %d times", wrong);
    }
    // ---------------------------------------------------------------- vote
    {
        const auto f = edacs::encodeFrame(0xEE12345, 0xFC00000);
        CHECK((int)f.size() == edacs::kFrameBits, "frame bits %zu", f.size());
        uint64_t sync = 0; for (int i = 0; i < 48; i++) sync = sync << 1 | f[(size_t)i];
        CHECK(sync == edacs::kSync, "sync %012llX", (unsigned long long)sync);
        auto g = f;
        g[48 + 3] ^= 1; g[48 + 40 + 9] ^= 1; g[48 + 80 + 20] ^= 1;
        edacs::Decoded d = edacs::decodeMessage(g.data() + 48);
        CHECK(d.ok && d.bchOk && d.corrected == 0 && d.disagree == 3 && d.data == 0xEE12345, "vote: data %07X", d.data);
        g[48 + 40 + 3] ^= 1; g[48 + 40 + 20] ^= 1;
        d = edacs::decodeMessage(g.data() + 48);
        CHECK(d.ok && d.corrected == 2 && d.data == 0xEE12345, "vote + BCH: data %07X corrected %d", d.data, d.corrected);
        std::vector<uint8_t> h(120);   // copies that agree but a wrong check: refused, as dsd-fme does
        const uint64_t w = (uint64_t)0x1234567 << 12 | (edacs::bchCheck(0x1234567) ^ 0x7);
        for (int i = 0; i < 40; i++) { const int b = (int)(w >> (39 - i) & 1); h[(size_t)i] = (uint8_t)b; h[40 + (size_t)i] = (uint8_t)(b ^ 1); h[80 + (size_t)i] = (uint8_t)b; }
        d = edacs::decodeMessage(h.data());
        CHECK(!d.ok || d.data == 0x1234567, "a bad check word is refused or corrected to the data sent");
    }
    // ---------------------------------------------------------------- messages: dsd-fme's captures and its field masks
    {
        using M = edacs::Message;
        const edacs::Mode S = edacs::Mode::Standard, E = edacs::Mode::Extended;
        M m = edacs::parse(0xF802180, 0x0000000, E);   // dsd-fme edacs-fme.c: "MSG_1 [F802180] MSG_2 [0000000] (MT1: 1F; MT2: 0) Initiate Test Call", CC LCN 1
        CHECK(m.kind == M::TestCall && m.ccLcn == 1 && m.lcn == 3, "captured EA test call: kind %d CC LCN %d WC LCN %d", (int)m.kind, m.ccLcn, m.lcn);
        m = edacs::parse(0xFE00045, 0xFE00045, E);     // "MSG_1 [FE00045] MSG_2 [FE00045] (MT1: 1F; MT2: C) System Dynamic Regroup :: SP-WGID: 069; Target: 0000069"
        CHECK(m.kind == M::Patch && m.group == 69 && m.target == 69, "captured EA regroup: group %d target %d", m.group, m.target);
        m = edacs::parse(0x5547923, 0x0540000, S);     // standard: MT-A 2 (digital), LID high 0x55, LCN 7, transmission trunked, group 0x123; LID low 0x2A
        CHECK(m.kind == M::GroupVoice && m.digital && !m.emergency && m.lcn == 7 && m.group == 0x123 && m.source == 0x2AAA, "standard group call: LCN %d group %X LID %X", m.lcn, m.group, m.source);
        m = edacs::parse(0x319BEEF, 0x0312345, E);     // EA: MT1 6 (analog), LCN 12, update, group 0xBEEF; emergency, source 0x12345
        CHECK(m.kind == M::GroupVoice && !m.digital && m.update && m.emergency && m.lcn == 12 && m.group == 0xBEEF && m.source == 0x12345, "EA group call");
        m = edacs::parse(0x8254321, 0x090ABCD, E);     // EA: MT1 0x10 digital I-call to 0x54321 from 0xABCD on LCN 9
        CHECK(m.kind == M::IndivVoice && m.digital && m.lcn == 9 && m.target == 0x54321 && m.source == 0xABCD, "EA individual call");
        m = edacs::parse(0xF890123, 0, S);             // standard: MT-A 7 MT-B 6, drop, LCN 4
        CHECK(m.kind == M::UnkeyDrop && m.drop && m.lcn == 4, "standard console drop");
        m = edacs::parse(0xFD03011, 0, S);             // standard: MT-D 8 site ID 0x11, CC LCN 3
        CHECK(m.kind == M::SiteId && m.site == 0x11 && m.ccLcn == 3, "standard site ID %d LCN %d", m.site, m.ccLcn);
        m = edacs::parse(0xFC01A2B, 0x0000005, E);     // EA: MT1 1F MT2 8 system 0x1A2B, CC LCN 5
        CHECK(m.kind == M::SystemInfo && m.sysId == 0x1A2B && m.ccLcn == 5, "EA system information");
        m = edacs::parse(0x5547923 ^ (edacs::kEsk << 20), 0x0540000 ^ (edacs::kEsk << 20), S, edacs::kEsk);
        CHECK(m.kind == M::GroupVoice && m.group == 0x123 && m.lcn == 7, "the ESK is taken off");
        // build / parse round trips (the builders the generated signals use)
        int bad = 0;
        M a; a.kind = M::GroupVoice; a.lcn = 7; a.group = 0x2A5; a.emergency = true; a.source = 0x1ABC;
        M b; b.kind = M::IndivVoice; b.lcn = 3; b.target = 12345; b.source = 4321; b.individual = true;
        M c; c.kind = M::SiteId; c.site = 9; c.ccLcn = 1;
        M d2; d2.kind = M::UnkeyDrop; d2.drop = true; d2.lcn = 5; d2.source = 77;
        M e; e.kind = M::GroupVoice; e.lcn = 12; e.group = 40000; e.source = 0x54321; e.digital = true;
        M f; f.kind = M::SystemInfo; f.sysId = 0xBEEF; f.ccLcn = 2;
        for (const auto& [x, md] : std::vector<std::pair<M, edacs::Mode>>{{a, S}, {b, S}, {c, S}, {d2, S}, {e, E}, {b, E}, {f, E}}) {
            uint32_t m1, m2;
            edacs::build(x, md, m1, m2);
            const M p = edacs::parse(m1, m2, md);
            if (p.kind != x.kind || p.lcn != x.lcn || p.group != x.group || p.target != x.target || (x.source >= 0 && p.source != x.source) ||
                p.digital != x.digital || p.emergency != x.emergency || p.site != x.site || p.sysId != x.sysId) {
                bad++;
                printf("  %s -> %s\n", edacs::describe(x, md).c_str(), edacs::describe(p, md).c_str());
            }
        }
        CHECK(bad == 0, "%d messages did not survive build / parse", bad);
        CHECK(edacs::afsText(0x2A5) == "05-045", "AFS text %s", edacs::afsText(0x2A5).c_str());
        CHECK(std::string(edacs::lcnStatus(30)) == "system busy" && *edacs::lcnStatus(5) == 0, "LCN status values");
    }

    // ---------------------------------------------------------------- the receiver on a control channel
    using M = edacs::Message;
    auto controlBits = [&](int frames, std::vector<uint32_t>& sent) {
        std::vector<uint8_t> b = lead(400, 77);
        for (int k = 0; k < frames; k++) {
            M x;
            if (k % 3 == 0) { x.kind = M::SiteId; x.site = 3; x.ccLcn = 1; }
            else if (k % 3 == 1) { x.kind = M::GroupVoice; x.lcn = 1 + k % 20; x.group = 0x100 + k; x.source = 0x1000 + k; }
            else { x.kind = M::IndivVoice; x.lcn = 4; x.target = 1000 + k; x.source = 2000 + k; x.individual = true; }
            uint32_t wa, wb;
            edacs::build(x, edacs::Mode::Standard, wa, wb);
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
        printf("  control channel%s: %d of %zu messages, %zu received, BCH good %llu, corrected %llu, refused %llu, polarity %s\n",
               inv ? " (inverted)" : "", n, sent.size(), R.msgs.size(), (unsigned long long)R.t.bchOk, (unsigned long long)R.t.bchFixed, (unsigned long long)R.t.msgBad,
               R.t.inverted ? "inverted" : "normal");
        CHECK(n == (int)sent.size() && R.msgs.size() <= sent.size() + 2, "control channel%s: %d of %zu messages", inv ? " (inverted)" : "", n, sent.size());
        CHECK(R.t.locked && R.t.inverted == (inv == 1), "lock and the polarity found by the sync and the BCH");
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
        printf("  noisy: %d of %zu messages, %d wrong (good %llu, corrected %llu, refused %llu)\n", n, sent.size(), stray, (unsigned long long)R.t.bchOk, (unsigned long long)R.t.bchFixed, (unsigned long long)R.t.msgBad);
        CHECK(n >= (int)sent.size() * 7 / 10 && stray == 0, "noisy: %d of %zu, %d wrong", n, sent.size(), stray);
    }
    {   // noise only: nothing
        const Run R = run(std::vector<cf32>((size_t)(fs * 2)), fs, off, 0.3);
        CHECK(R.t.blocksOk == 0 && R.msgs.empty() && !R.t.voice.carrier, "noise only: %llu frames, carrier %d", (unsigned long long)R.t.blocksOk, R.t.voice.carrier);
    }

    // ---------------------------------------------------------------- a working channel
    {
        const auto sig = edacs::workingChannel(fs, off, 2.0, 1000.0, 96, 0.5f);
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
                if (t.timeSec > 0.5 && t.timeSec < 1.9) { carrier |= t.voice.carrier; lsd |= t.lsd; provoice |= t.provoice; framesDuringVoice = t.eot; }
            }
        }
        for (int k = 0; k < 30; k++) { std::vector<cf32> z(4096); for (cf32& v : z) v = cf32(0.3f * gauss(), 0.3f * gauss()); rx.feed(z.data(), z.size()); if (rx.telemetry(t, seq)) seq = t.seq; }
        // the tone in the sound: correlate with 1 kHz
        double c = 0, s = 0, e = 0;
        const size_t a0 = audio.size() / 4, a1 = audio.size() * 3 / 4;
        for (size_t i = a0; i < a1; i++) { const double ph = 2 * 3.14159265358979323846 * 1000.0 * (double)i / 48000.0; c += audio[i] * std::cos(ph); s += audio[i] * std::sin(ph); e += audio[i] * audio[i]; }
        const double frac = e > 0 ? 2 * (c * c + s * s) / ((double)(a1 - a0) * e) : 0;
        printf("  working channel: carrier %d, low-speed data %d, ProVoice %d, %zu audio samples (%.0f %% of the power at 1 kHz), end of transmission %llu (during the voice %llu), carrier after %d\n",
               carrier, lsd, provoice, audio.size(), 100 * frac, (unsigned long long)t.eot, (unsigned long long)framesDuringVoice, t.voice.carrier);
        CHECK(carrier && lsd && !provoice, "working channel: carrier %d, low-speed data %d, ProVoice %d", carrier, lsd, provoice);
        CHECK(audio.size() > 48000 && frac > 0.8, "the voice tone plays (%.0f %% at 1 kHz), the low-speed data is filtered out", 100 * frac);
        CHECK(framesDuringVoice == 0 && t.eot >= 1, "the end-of-transmission dotting is heard (%llu, %llu during the voice)", (unsigned long long)t.eot, (unsigned long long)framesDuringVoice);
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
