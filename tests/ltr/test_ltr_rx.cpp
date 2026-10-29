// LTR and Passport: the checksums against a port of SDRTrunk's own BitSet code (CRCLTR.getCalculatedChecksum / getTransmittedChecksum,
// CRCPassport.check, GPL-3, Dennis Sheirer) and a hand-worked vector, message types after LTRStandardMessageProcessor and
// PassportMessage.getMessageType, then the full chain (generated FM signal with voice and noise -> receiver). No real recording was
// available: the signals are generated.
#include "dect2/ltr/ltr_proto.h"
#include "dect2/ltr/ltr_rx.h"
#include "dect2/ltr/ltr_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { std::printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

using namespace dect2;

static unsigned rngState = 12345;
static float gauss() { float s = 0; for (int i = 0; i < 6; i++) { rngState = rngState * 1664525u + 1013904223u; s += ((rngState >> 8) & 0xFFFF) / 65536.f - 0.5f; } return s * 2.0f * 0.7071f; }   // about unit variance

static std::vector<ltr::Word> sentWords() {
    std::vector<ltr::Word> w;
    auto add = [&](int area, int gt, int home, int id, int fr) { ltr::Word x; x.area = area; x.gotoCh = gt; x.home = home; x.id = id; x.freeCh = fr; x.chk = ltr::checksum(x); w.push_back(x); };
    add(0, 3, 3, 17, 5);
    add(0, 5, 1, 101, 7);
    add(1, 12, 12, 254, 2);
    add(0, 9, 8, 255, 9);
    add(0, 7, 2, 33, 20);
    add(0, 19, 19, 200, 1);
    return w;
}

// ---- SDRTrunk's checksum code, ported line for line onto a bool array indexed like its BitSet (bit 0 = the first sync bit)
static const int kSdrLtr[24] = {0x38, 0x1C, 0x0E, 0x46, 0x23, 0x51, 0x68, 0x75, 0x7A, 0x3D, 0x1F, 0x4F, 0x26, 0x52, 0x29, 0x15, 0x0B, 0x45, 0x62, 0x31, 0x19, 0x0D, 0x07, 0x43};
static int sdrLtrCalculated(const std::vector<uint8_t>& msg) { int c = 0; for (int i = 9; i < 33; i++) if (msg[i]) c ^= kSdrLtr[i - 9]; return c; }
static int sdrLtrTransmitted(const std::vector<uint8_t>& msg) { int r = 0; for (int x = 0; x < 7; x++) if (msg[x + 33]) r += 1 << (6 - x); return r; }
static const int kSdrPass[51] = {0x6E, 0xBF, 0xD6, 0xE3, 0xF8, 0x7C, 0x3E, 0x97, 0xC2, 0xE9, 0x75, 0x3B, 0x94, 0x4A, 0xAD, 0x57, 0xA2, 0xD9, 0x6D,
                                 0x37, 0x92, 0xC1, 0x61, 0x31, 0x19, 0x0D, 0x07, 0x8A, 0xCD, 0x67, 0xBA, 0xD5, 0x6B, 0xBC, 0x5E, 0xA7, 0xDA, 0xE5,
                                 0x73, 0xB0, 0x58, 0x2C, 0x16, 0x83, 0xC8, 0x64, 0x32, 0x91, 0x49, 0x25, 0x13};
static int sdrPassResidual(const std::vector<uint8_t>& msg) {
    int c = 0;
    for (int i = 9; i <= 59; i++) if (msg[i]) c ^= kSdrPass[i - 9];
    int t = 0;
    for (int x = 0; x < 8; x++) if (msg[x + 60]) t ^= 1 << (7 - x);
    return (c ^ t) & 0xFF;
}
static int sdrGetInt(const std::vector<uint8_t>& m, int from, int to) { int v = 0; for (int i = from; i <= to; i++) v = (v << 1) | m[i]; return v; }

static std::vector<uint8_t> stream(const std::vector<ltr::Word>& w, int repeats) {
    std::vector<uint8_t> b;
    for (int i = 0; i < 40; i++) b.push_back((uint8_t)(i & 1));   // lead-in so the slicer thresholds settle
    for (int r = 0; r < repeats; r++)
        for (const auto& x : w) {
            const auto e = ltr::encodeWord(x);
            b.insert(b.end(), e.begin(), e.end());
            for (int i = 0; i < 12; i++) b.push_back((uint8_t)((i * 7 + r) & 1));
        }
    return b;
}

static LtrTelemetry run(const std::vector<cf32>& sig, double fs, double off, double noiseSigma, bool noiseOnly = false) {
    LtrReceiver rx;
    rx.configure(fs);
    rx.setSignalOffset(off);
    LtrTelemetry t;
    uint64_t seq = 0;
    std::vector<cf32> blk(4096);
    const size_t total = noiseOnly ? (size_t)(fs * 40) : sig.size() + (size_t)(fs * 0.3);
    for (size_t i = 0; i < total; i += blk.size()) {
        const size_t n = std::min(blk.size(), total - i);
        for (size_t k = 0; k < n; k++) {
            cf32 v = (noiseOnly || i + k >= sig.size()) ? cf32(0, 0) : sig[i + k];
            if (noiseSigma > 0) v += cf32((float)noiseSigma * gauss(), (float)noiseSigma * gauss());
            blk[k] = v;
        }
        rx.feed(blk.data(), n);
        LtrTelemetry x;
        if (rx.telemetry(x, seq)) { seq = x.seq; t = x; }
    }
    return t;
}

static int found(const LtrTelemetry& t, const std::vector<ltr::Word>& sent, int& stray) {
    std::set<std::string> want;
    for (const auto& w : sent) want.insert(ltr::talkgroup(w));
    std::set<std::string> got;
    stray = 0;
    for (const auto& m : t.messages) { got.insert(m.talkgroup); if (!want.count(m.talkgroup)) stray++; }
    int n = 0;
    for (const auto& s : want) n += got.count(s) ? 1 : 0;
    return n;
}

int main() {
    const auto sent = sentWords();
    {   // a hand-worked vector: area 0, go-to 1, home 1, group 255, free 1 (the idle word of repeater 1). Set bits and CRCLTR values:
        // go-to bit 0 -> 0x51, home bit 0 -> 0x1F, group 7..0 -> 0x4F 0x26 0x52 0x29 0x15 0x0B 0x45 0x62, free bit 0 -> 0x43; XOR = 0x26
        ltr::Word w; w.area = 0; w.gotoCh = 1; w.home = 1; w.id = 255; w.freeCh = 1;
        CHECK(ltr::checksum(w) == 0x26, "hand vector: checksum %02X, want 26", ltr::checksum(w));
        const auto b = ltr::encodeWord(w);
        std::string bs;
        for (auto x : b) bs += (char)('0' + x);
        CHECK(bs == "101011000" "0" "00001" "00001" "11111111" "00001" "0100110", "hand vector bits %s", bs.c_str());
        ltr::Word r;
        CHECK(ltr::parseWord(b.data(), r) && ltr::checkWord(r) == 1, "hand vector does not check");
    }
    {   // our checksum against SDRTrunk's code on every combination of a spread of field values; the inbound (inverted) word too
        unsigned st = 99;
        int n = 0, bad = 0;
        for (int k = 0; k < 4000; k++) {
            st = st * 1103515245u + 12345u;
            ltr::Word w; w.area = (int)(st >> 30) & 1; w.gotoCh = (int)(st >> 8) & 31; w.home = (int)(st >> 13) & 31; w.id = (int)(st >> 18) & 255; w.freeCh = (int)(st >> 3) & 31;
            const auto b = ltr::encodeWord(w);
            if (sdrLtrCalculated(b) != sdrLtrTransmitted(b)) bad++;   // CRCLTR.check for an OSW: PASSED
            // the ISW: LTRStandardMessageProcessor flips bits 0 - 39, then CRCLTR accepts calculated == transmitted or calculated ^ 127
            auto i = ltr::encodeWord(w, true);
            for (auto& x : i) x ^= 1;
            if (sdrLtrCalculated(i) != sdrLtrTransmitted(i)) bad++;
            ltr::Word r;
            if (!ltr::parseWord(i.data(), r) || r.area != w.area || r.gotoCh != w.gotoCh || r.home != w.home || r.id != w.id || r.freeCh != w.freeCh || ltr::checkWord(r) != 1) bad++;
            // an inbound word whose checksum is inverted: SDRTrunk PASSED_INV, ours 2
            std::vector<uint8_t> inv = b;
            for (int x = 33; x < 40; x++) inv[x] ^= 1;
            if ((sdrLtrCalculated(inv) ^ 127) != sdrLtrTransmitted(inv) || !ltr::parseWord(inv.data(), r) || ltr::checkWord(r) != 2) bad++;
            n++;
        }
        CHECK(bad == 0, "LTR checksum differs from SDRTrunk's in %d of %d words", bad, n);
        // every single bit error in bits 9 - 39 is caught
        ltr::Word w; w.gotoCh = 4; w.home = 6; w.id = 77; w.freeCh = 3;
        const auto b = ltr::encodeWord(w);
        int missed = 0;
        for (int i = 9; i < 40; i++) {
            auto e = b; e[i] ^= 1;
            ltr::Word r;
            if (ltr::parseWord(e.data(), r) && ltr::checkWord(r) != 0) missed++;
        }
        CHECK(missed == 0, "%d single bit errors pass the LTR checksum", missed);
        auto e = b; e[3] ^= 1;
        ltr::Word r;
        CHECK(!ltr::parseWord(e.data(), r), "a word with a bad sync was accepted");
        CHECK(ltr::kindOf(255) == "idle" && ltr::kindOf(254) == "callsign ID" && ltr::kindOf(17).empty(), "kinds");
    }
    {   // Passport: our encoder and fields against SDRTrunk's CRCPassport and PassportMessage field positions
        unsigned st = 7;
        int bad = 0;
        for (int k = 0; k < 4000; k++) {
            st = st * 1103515245u + 12345u; const unsigned a = st;
            st = st * 1103515245u + 12345u; const unsigned c = st;
            ltr::PassportWord p; p.dcc = (int)(a >> 30); p.lcn = (int)(a >> 4) & 0x7FF; p.site = (int)(a >> 16) & 0x7F; p.group = (int)(c >> 8) & 0xFFFF; p.type = (int)(c >> 26) & 15; p.freeCh = (int)(a >> 13) & 0x7FF;
            const auto b = ltr::encodePassport(p);
            if (b.size() != 68 || sdrPassResidual(b) != 0) { bad++; continue; }
            if (sdrGetInt(b, 0, 8) != 0x158 || sdrGetInt(b, 9, 10) != p.dcc || sdrGetInt(b, 11, 21) != p.lcn || sdrGetInt(b, 22, 28) != p.site ||
                sdrGetInt(b, 29, 44) != p.group || sdrGetInt(b, 22, 44) != p.radioId() || sdrGetInt(b, 33, 36) != p.neighborBand() ||
                sdrGetInt(b, 41, 44) != p.siteBand() || sdrGetInt(b, 45, 48) != p.type || sdrGetInt(b, 49, 59) != p.freeCh) bad++;
            ltr::PassportWord r;
            if (!ltr::parsePassport(b.data(), r) || !ltr::passportOk(r) || r.group != p.group || r.lcn != p.lcn || r.freeCh != p.freeCh) bad++;
        }
        CHECK(bad == 0, "Passport differs from SDRTrunk in %d words", bad);
        ltr::PassportWord p; p.lcn = 345; p.site = 12; p.group = 4321; p.type = 0; p.freeCh = 346;
        const auto b = ltr::encodePassport(p);
        int missed = 0;
        for (int i = 9; i < 66; i++) {   // the last two check bits are the ones SDRTrunk corrects
            auto e = b; e[i] ^= 1;
            ltr::PassportWord r;
            if (ltr::parsePassport(e.data(), r) && ltr::passportOk(r)) missed++;
        }
        CHECK(missed == 0, "%d single bit errors pass the Passport checksum", missed);
        auto e = b; e[67] ^= 1;
        ltr::PassportWord r;
        CHECK(ltr::parsePassport(e.data(), r) && ltr::passportOk(r), "an error in check bit 67 is corrected by SDRTrunk, refused here");
        // message types (PassportMessage.getMessageType)
        auto kind = [](int type, int lcn, int fr) { ltr::PassportWord w; w.type = type; w.lcn = lcn; w.freeCh = fr; return ltr::passportKind(w); };
        CHECK(kind(0, 100, 5) == ltr::PassportKind::CallStart && kind(2, 100, 5) == ltr::PassportKind::CallStart && kind(1, 100, 5) == ltr::PassportKind::CallStart, "call start");
        CHECK(kind(1, 100, 2042) == ltr::PassportKind::TgAssign && kind(1, 1792, 5) == ltr::PassportKind::Idle && kind(1, 1793, 5) == ltr::PassportKind::Idle, "assign / idle");
        CHECK(kind(1, 2047, 5) == ltr::PassportKind::CallEnd && kind(9, 3, 5) == ltr::PassportKind::Data && kind(5, 3, 5) == ltr::PassportKind::Page && kind(6, 3, 5) == ltr::PassportKind::RadioId && kind(11, 3, 5) == ltr::PassportKind::Register && kind(1, 1900, 5) == ltr::PassportKind::Unknown, "other types");
        // PassportBand: base + channel x 12.5 kHz; 460 is band 6, 800 band 0, 216 band 13, CAN has no base
        CHECK(std::fabs(ltr::passportFreqHz(6, 100) - 461.25e6) < 1 && std::fabs(ltr::passportFreqHz(0, 1) - 800.0125e6) < 1 && std::fabs(ltr::passportFreqHz(13, 8) - 216.1e6) < 1 && ltr::passportFreqHz(10, 5) == 0, "band plan");
    }
    const double fs = 480000, off = -50000;
    const auto bits = stream(sent, 3);
    {   // full chain, clean, with a voice tone on top
        const auto sig = ltr::modulate(bits, fs, off, 600.0, 0.5f, 1000.0, 1500.0);
        const LtrTelemetry t = run(sig, fs, off, 0);
        int stray = 0;
        const int n = found(t, sent, stray);
        CHECK(n == (int)sent.size(), "clean: %d of %zu words found", n, sent.size());
        CHECK(stray == 0, "clean: %d words that were not sent", stray);
        CHECK(t.blocksOk >= sent.size() * 2, "clean: only %llu good words", (unsigned long long)t.blocksOk);
        CHECK(t.locked, "clean: not locked");
        CHECK(t.repeaters.size() >= 8, "clean: %zu repeaters", t.repeaters.size());
        std::printf("clean: found %d of %zu, good %llu bad %llu, repeaters %zu\n", n, sent.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.repeaters.size());
    }
    {   // inverted polarity and a carrier offset
        const auto sig = ltr::modulate(bits, fs, off + 700, 600.0, 0.5f, 0, 0, true);
        const LtrTelemetry t = run(sig, fs, off, 0);
        int stray = 0;
        const int n = found(t, sent, stray);
        CHECK(n == (int)sent.size() && stray == 0, "inverted + offset: found %d, stray %d", n, stray);
    }
    {   // noise
        const auto sig = ltr::modulate(bits, fs, off, 600.0, 0.5f, 1000.0, 1500.0);
        const LtrTelemetry t = run(sig, fs, off, 0.15);
        int stray = 0;
        const int n = found(t, sent, stray);
        CHECK(n >= (int)sent.size() - 1 && stray == 0, "noise: found %d of %zu, stray %d", n, sent.size(), stray);
        std::printf("noise: found %d of %zu, stray %d, good %llu bad %llu\n", n, sent.size(), stray, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    {   // noise only: no word is accepted as good
        const LtrTelemetry t = run({}, fs, off, 0.1, true);
        CHECK(t.blocksOk == 0, "noise only: %llu good words", (unsigned long long)t.blocksOk);
        std::printf("noise only: good %llu bad %llu\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    {   // Passport words through the receiver: idle (site 9, band 460), a call on LCN 77 for group 1234, its end; both polarities
        std::vector<ltr::PassportWord> pw(3);
        pw[0].lcn = 1792; pw[0].site = 9; pw[0].group = 6; pw[0].type = 1; pw[0].freeCh = 81; pw[0].dcc = 2;
        pw[1].lcn = 77; pw[1].site = 9; pw[1].group = 1234; pw[1].type = 0; pw[1].freeCh = 81; pw[1].dcc = 2;
        pw[2].lcn = 2047; pw[2].site = 9; pw[2].group = 1234; pw[2].type = 1; pw[2].freeCh = 81; pw[2].dcc = 2;
        std::vector<uint8_t> pb;
        for (int i = 0; i < 40; i++) pb.push_back((uint8_t)(i & 1));
        for (int r = 0; r < 3; r++) for (const auto& w : pw) { const auto e = ltr::encodePassport(w); pb.insert(pb.end(), e.begin(), e.end()); }
        for (int i = 0; i < 24; i++) pb.push_back((uint8_t)(i & 1));   // the transmitter keeps the data going after the last word
        for (bool inv : {false, true}) {
            const auto sig = ltr::modulate(pb, fs, off, 600.0, 0.5f, 1000.0, 1500.0, inv);
            const LtrTelemetry t = run(sig, fs, off, 0);
            int calls = 0, idles = 0, ends = 0, ltrWords = 0;
            for (const auto& m : t.messages) {
                if (!m.passport) { ltrWords++; continue; }
                if (m.kind == "call" && m.lcn == 77 && m.talkgroup == "1234") calls++;
                if (m.kind == "idle") idles++;
                if (m.kind == "call end" && m.group == 1234) ends++;
            }
            CHECK(calls == 3 && idles == 3 && ends == 3 && ltrWords == 0, "Passport%s: calls %d idles %d ends %d, LTR words %d", inv ? " inverted" : "", calls, idles, ends, ltrWords);
            CHECK(t.passport && t.passportSite == 9 && t.passportDcc == 2 && t.passportBand == 6 && t.turnOffGroup == 1234, "Passport telemetry: site %d dcc %d band %d", t.passportSite, t.passportDcc, t.passportBand);
            std::printf("passport%s: %zu words, good %llu bad %llu\n", inv ? " inverted" : "", t.messages.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        }
    }
    std::printf("%s: %d failure(s)\n", fails ? "test_ltr_rx FAILED" : "test_ltr_rx OK", fails);
    return fails ? 1 : 0;
}
