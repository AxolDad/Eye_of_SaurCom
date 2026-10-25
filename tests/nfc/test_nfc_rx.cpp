// NFC receiver on the test signal: the selection exchange of the test card comes out frame by frame as the generator put it in (nfc-laboratory
// decodes, this side names), the card is listed with its UID, ATQA, SAK and type, at 10 Msps and at the lowest rate the mode accepts; the
// 7-byte card takes two cascade levels; noise alone gives nothing; the frames and cards survive a reset and go on Clear.
#include "dect2/nfc/nfc_gen.h"
#include "dect2/nfc/nfc_proto.h"
#include "dect2/nfc/nfc_rx.h"
#include "dect2/nfc/nfc_tel.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static void run(NfcReceiver& rx, const NfcGenConfig& c, double seconds) {
    NfcGenerator gen(c);
    std::vector<cf32> buf(65536);
    const size_t total = (size_t)(seconds * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) {
        gen.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
    }
}

// the decoded frames oldest first, without the carrier markers
static std::vector<NfcFrame> oldestFirst(const NfcTelemetry& t) { return std::vector<NfcFrame>(t.frames.rbegin(), t.frames.rend()); }

static int checkExchange(const NfcTelemetry& t, int scene, const char* label) {
    const auto want = nfcTestExchange(scene);
    const auto got = oldestFirst(t);
    // the first exchange: find the first REQA / WUPA and compare from there
    size_t start = 0;
    while (start < got.size() && !(got[start].poll && got[start].bytes.size() == 1 && (got[start].bytes[0] == 0x26 || got[start].bytes[0] == 0x52))) start++;
    CHECK(start < got.size(), "%s: no REQA / WUPA among %zu frames", label, got.size());
    if (start >= got.size()) return 0;
    int matched = 0;
    for (size_t i = 0; i < want.size() && start + i < got.size(); i++) {
        const NfcFrame& g = got[start + i];
        const bool same = g.poll == want[i].poll && g.bytes == want[i].bytes;
        if (!same) {
            printf("    %s: frame %zu: want %s %s, got %s %s (%s)\n", label, i, want[i].poll ? "poll" : "listen", nfc::hex(want[i].bytes.data(), want[i].bytes.size()).c_str(),
                   g.poll ? "poll" : "listen", nfc::hex(g.bytes.data(), g.bytes.size()).c_str(), g.name.c_str());
            break;
        }
        CHECK(!g.parityError && !g.crcError && !g.truncated, "%s: frame %zu flagged (%s)", label, i, g.name.c_str());
        CHECK(g.tech == nfc::TechA && g.rateKbps == 106, "%s: frame %zu tech %d rate %d", label, i, (int)g.tech, g.rateKbps);
        matched++;
    }
    CHECK(matched == (int)want.size(), "%s: %d of %zu frames of the exchange matched", label, matched, want.size());
    return matched;
}

int main() {
    // the generator's own facts: the exchange, the CRCs it writes, the envelope's shape
    {
        const auto ex = nfcTestExchange(0);
        CHECK(ex.size() == 7 && ex[0].poll && ex[0].bits == 7 && ex[0].bytes[0] == 0x26, "scene 0 exchange");
        CHECK(nfc::crcAOk(ex[4].bytes.data(), ex[4].bytes.size()) && nfc::crcAOk(ex[5].bytes.data(), ex[5].bytes.size()), "scene 0 CRCs");
        const auto ex1 = nfcTestExchange(1);
        CHECK(ex1.size() == 13 && ex1[3].bytes[0] == 0x88 && nfcTestUid(1).size() == 7, "scene 1 exchange %zu", ex1.size());
        NfcGenConfig c; c.snrDb = 200;
        NfcGenerator g(c);
        std::vector<cf32> v(200000);
        g.generate(v.data(), v.size());   // the carrier alone (20 ms lead): a steady magnitude of amp
        double mn = 1e9, mx = 0;
        for (const auto& s : v) { const double m = std::abs(s); mn = std::min(mn, m); mx = std::max(mx, m); }
        CHECK(std::fabs(mn - 0.5) < 1e-3 && std::fabs(mx - 0.5) < 1e-3, "lead magnitude %.4f..%.4f", mn, mx);
        g.generate(v.data(), v.size());   // the exchange: pauses to zero and the card's subcarrier
        mn = 1e9; mx = 0;
        for (const auto& s : v) { const double m = std::abs(s); mn = std::min(mn, m); mx = std::max(mx, m); }
        CHECK(mn < 1e-3 && mx > 0.5 * (1 + 0.1), "exchange magnitude %.4f..%.4f", mn, mx);
    }
    // 10 Msps, a MIFARE Classic 1K
    {
        NfcReceiver rx;
        CHECK(!rx.ready(), "ready before configure");
        rx.configure(10e6);
        CHECK(rx.ready(), "not ready at 10 Msps");
        rx.configure(2e6);
        CHECK(!rx.ready(), "ready at 2 Msps");
        rx.configure(10e6);
        std::vector<std::string> logs;
        rx.setLogCallback([&](const std::string& s) { logs.push_back(s); });
        NfcGenConfig c; c.rate = 10e6; c.snrDb = 40;
        run(rx, c, 0.45);   // five exchanges: the lead, then one every 0.1 s (0.02, 0.12, ... 0.42)
        NfcTelemetry t;
        CHECK(rx.telemetry(t, 0), "no telemetry");
        printf("  10 Msps: %s; %zu frames, field %d, level %.1f dBFS\n", nfcSummary(t).c_str(), t.frames.size(), t.fieldOn, t.levelDb);
        CHECK(t.fieldOn && t.fieldOnCount >= 1, "field not detected");
        CHECK(t.state == 2 && t.dataValid, "state %d", t.state);
        CHECK(t.frames.size() == 7 * 5, "%zu frames for 5 exchanges", t.frames.size());
        checkExchange(t, 0, "10 Msps");
        CHECK(t.blocksBad == 0, "%llu damaged frames", (unsigned long long)t.blocksBad);
        CHECK(t.pollFrames == 4 * 5 && t.listenFrames == 3 * 5, "poll %llu listen %llu", (unsigned long long)t.pollFrames, (unsigned long long)t.listenFrames);
        CHECK(t.cards.size() == 1, "%zu cards", t.cards.size());
        if (!t.cards.empty()) {
            const NfcCard& k = t.cards[0];
            CHECK(k.uid == nfcTestUid(0) && k.atqa[0] == 0x04 && k.atqa[1] == 0x00 && k.sak == 0x08, "card %s atqa %02X %02X sak %02X", nfc::hex(k.uid.data(), k.uid.size()).c_str(), k.atqa[0], k.atqa[1], k.sak);
            CHECK(k.type.find("MIFARE Classic 1K") != std::string::npos && k.manufacturer == "NXP Semiconductors" && !k.iso14443_4 && k.selections == 5, "type '%s' maker '%s' sel %llu", k.type.c_str(), k.manufacturer.c_str(), (unsigned long long)k.selections);
            printf("  card: %s, %s, %s\n", nfc::hex(k.uid.data(), k.uid.size()).c_str(), k.type.c_str(), k.manufacturer.c_str());
        }
        bool logged = false;
        for (const auto& l : logs) if (l.find("UID 04 7B C2 1A") != std::string::npos) logged = true;
        CHECK(logged, "new card not logged (%zu lines)", logs.size());
        // the names
        const auto f = oldestFirst(t);
        bool reqa = false, atqa = false, sel = false, uid = false, sak = false, hlta = false;
        for (const auto& x : f) {
            if (x.name == "REQA") reqa = true;
            if (x.name.rfind("ATQA 04 00", 0) == 0) atqa = true;
            if (x.name == "SELECT CL1 UID 04 7B C2 1A") sel = true;
            if (x.name == "UID CL1 04 7B C2 1A") uid = true;
            if (x.name == "SAK 08") sak = true;
            if (x.name == "HLTA") hlta = true;
        }
        CHECK(reqa && atqa && sel && uid && sak && hlta, "names: REQA %d ATQA %d SEL %d UID %d SAK %d HLTA %d", reqa, atqa, sel, uid, sak, hlta);
        // the frame times rise and the first exchange sits after the lead
        CHECK(f.size() > 1 && f[0].sec > 0.019 && f[0].sec < 0.03 && f[1].sec > f[0].sec, "times %.4f %.4f", f.size() ? f[0].sec : -1, f.size() > 1 ? f[1].sec : -1);
        // reset keeps the lists, the decoder goes on; Clear empties them
        rx.reset();
        run(rx, c, 0.15);
        NfcTelemetry t2;
        rx.telemetry(t2, 0);
        CHECK(t2.frames.size() > t.frames.size() && t2.cards.size() == 1, "after reset: %zu frames, %zu cards", t2.frames.size(), t2.cards.size());
        rx.clearResults();
        NfcTelemetry t3;
        rx.telemetry(t3, 0);
        CHECK(t3.frames.empty() && t3.cards.empty() && t3.blocksOk == 0, "after clear: %zu frames", t3.frames.size());
    }
    // the 7-byte card: two cascade levels and a READ; a carrier offset does not matter (the magnitude is decoded)
    {
        NfcReceiver rx;
        rx.configure(10e6);
        NfcGenConfig c; c.rate = 10e6; c.snrDb = 35; c.scene = 1; c.cfoHz = 150e3;
        run(rx, c, 0.35);
        NfcTelemetry t;
        rx.telemetry(t, 0);
        printf("  7-byte UID: %s; %zu frames\n", nfcSummary(t).c_str(), t.frames.size());
        checkExchange(t, 1, "scene 1");
        CHECK(t.cards.size() == 1, "%zu cards", t.cards.size());
        if (!t.cards.empty()) {
            const NfcCard& k = t.cards[0];
            CHECK(k.uid == nfcTestUid(1) && k.atqa[0] == 0x44 && k.sak == 0x00 && k.type.find("NTAG") != std::string::npos, "card %s sak %02X type '%s'", nfc::hex(k.uid.data(), k.uid.size()).c_str(), k.sak, k.type.c_str());
        }
        bool rd = false, data = false;
        for (const auto& x : t.frames) { if (x.name == "READ block 4") rd = true; if (x.name == "16 data bytes") data = true; }
        CHECK(rd && data, "READ %d, data %d", rd, data);
    }
    // the lowest rate the mode accepts, with a weaker, noisier signal
    {
        NfcReceiver rx;
        rx.configure(2.5e6);
        CHECK(rx.ready(), "not ready at 2.5 Msps");
        NfcGenConfig c; c.rate = 2.5e6; c.snrDb = 30; c.amp = 0.2;
        run(rx, c, 0.45);
        NfcTelemetry t;
        rx.telemetry(t, 0);
        printf("  2.5 Msps: %s; %zu frames\n", nfcSummary(t).c_str(), t.frames.size());
        CHECK(t.fieldOn, "field not detected at 2.5 Msps");
        CHECK(t.cards.size() == 1 && t.cards[0].uid == nfcTestUid(0), "%zu cards at 2.5 Msps", t.cards.size());
        CHECK(t.frames.size() >= 7, "%zu frames at 2.5 Msps", t.frames.size());
    }
    // noise alone at a quiet receiver's level (a magnitude under the field threshold): no field, no frames, no cards
    {
        NfcReceiver rx;
        rx.configure(10e6);
        NfcGenConfig c; c.rate = 10e6; c.noiseOnly = true; c.snrDb = 0; c.amp = 0.005;   // sigma 0.0035 per component, magnitude about 0.0044
        run(rx, c, 0.3);
        NfcTelemetry t;
        rx.telemetry(t, 0);
        CHECK(!t.fieldOn && t.frames.empty() && t.cards.empty(), "noise: field %d, %zu frames, %zu cards", t.fieldOn, t.frames.size(), t.cards.size());
        CHECK(t.state == 1 && !t.dataValid, "noise state %d", t.state);
    }
    // loud noise with no field: the decoder takes it for a field (nothing distinguishes an unmodulated carrier from a steady level) and
    // sorts stray pulses into frames; none of them makes a card, and the user raises the threshold (fieldView) when this happens
    {
        NfcReceiver rx;
        rx.configure(10e6);
        NfcGenConfig c; c.rate = 10e6; c.noiseOnly = true; c.snrDb = 0; c.amp = 0.5;
        run(rx, c, 0.3);
        NfcTelemetry t;
        rx.telemetry(t, 0);
        CHECK(t.cards.empty(), "%zu cards from noise", t.cards.size());
        printf("  loud noise: field %d, %zu stray frames, %llu of them damaged\n", t.fieldOn, t.frames.size(), (unsigned long long)t.blocksBad);
        rx.setPowerThreshold(0.5f);   // the level is about 0.44: over the threshold nothing is a field
        rx.clearResults();
        run(rx, c, 0.3);
        rx.telemetry(t, 0);
        CHECK(!t.fieldOn && t.frames.empty(), "raised threshold: field %d, %zu frames", t.fieldOn, t.frames.size());
    }
    // the technology mask and the threshold reach the decoder: with NFC-A off nothing is decoded; a threshold above the carrier: no field
    {
        NfcReceiver rx;
        rx.configure(10e6);
        rx.setTechMask(2 | 4 | 8);
        NfcGenConfig c; c.rate = 10e6;
        run(rx, c, 0.25);
        NfcTelemetry t;
        rx.telemetry(t, 0);
        CHECK(t.techMask == 14 && t.frames.empty(), "NFC-A off: %zu frames", t.frames.size());
        rx.setTechMask(15);
        rx.setPowerThreshold(0.45f);   // above the carrier's 0.5 * (1 - depth)... the pauses pull the average under it
        rx.setPowerThreshold(0.9f);
        rx.clearResults();
        run(rx, c, 0.25);
        rx.telemetry(t, 0);
        CHECK(std::fabs(t.powerThreshold - 0.5f) < 1e-6 && !t.fieldOn, "threshold %.3f field %d", t.powerThreshold, t.fieldOn);
    }
    const ModeTuning mt = nfcTuning();
    CHECK(mt.stdMode == 150 && std::string(mt.id) == "nfc" && mt.carrierAtCentre && mt.notCircular && mt.tuneOffsetHz == 0 && mt.minSampleRate == 2.5e6, "tuning");
    return testSummary("nfc_rx");
}
