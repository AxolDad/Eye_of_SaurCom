// P25 Phase 2 TDMA outbound (core/src/p25/p25p2_*.cpp), with test vectors this test builds itself: the DUID and I-ISCH codes, CRC-12, the
// MAC PDUs through FACCH and SACCH (Reed-Solomon corrects symbol errors, the CRC refuses what it cannot), the encryption sync, the frame
// layer on a synthetic two-channel voice carrier (superframe and timeslot sync, each slot's AMBE+2 frames in order, MAC_PTT / MAC_END_PTT
// into the call tracker, an encrypted call), and the H-DQPSK modem (48 kHz exactly, and 500 kHz with a carrier offset and noise).
#include "dect2/p25/p25p2.h"
#include <cmath>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;
using namespace dect2::p25p2;
#include "../common/test_util.h"

namespace {

struct Seen {
    std::vector<std::vector<uint8_t>> frames[2];   // the voice frames each channel handed on, in order
    std::vector<MacPdu> macs[2];
    int locks = 0, lost = 0;
    int ptt[2] = {0, 0}, end[2] = {0, 0}, encStarts[2] = {0, 0};
    Call startCall[2], lastCall[2];
};

void run(Framer& f, CallTracker& ct, const std::vector<uint8_t>& dib, Seen& s) {
    for (uint8_t d : dib) {
        f.push(d);
        for (const Event& e : f.take()) {
            const bool was = ct.call(e.chan).active;
            ct.onEvent(e);
            const Call& c = ct.call(e.chan);
            if (e.kind == Event::Lock) s.locks++;
            if (e.kind == Event::Lost) s.lost++;
            if (e.kind == Event::Voice) for (int i = 0; i < e.nVoice; i++) s.frames[e.chan].emplace_back(e.voice[i], e.voice[i] + 72);
            if (e.kind == Event::Mac) {
                s.macs[e.chan].push_back(e.mac);
                if (e.mac.opcode == kMacPtt) { s.ptt[e.chan]++; s.startCall[e.chan] = c; if (c.encrypted) s.encStarts[e.chan]++; }
                if (e.mac.opcode == kMacEndPtt) s.end[e.chan]++;
            }
            if (was && !c.active) s.lastCall[e.chan] = c;
        }
    }
}

// how many of the channel's frames match the generator's, in order from the first one the channel handed on
int matching(const std::vector<std::vector<uint8_t>>& got, int chan, int* firstIdx) {
    if (got.empty()) return 0;
    // the generator numbers each channel's frames from 0; find where the receiver started
    int start = -1;
    for (int n = 0; n < 4000 && start < 0; n++) {
        uint8_t ref[72];
        Generator::voiceFrame(chan, (uint32_t)n, ref);
        if (!memcmp(ref, got[0].data(), 72)) start = n;
    }
    if (firstIdx) *firstIdx = start;
    if (start < 0) return 0;
    int ok = 0;
    for (size_t i = 0; i < got.size(); i++) {
        uint8_t ref[72];
        Generator::voiceFrame(chan, (uint32_t)(start + (int)i), ref);
        ok += !memcmp(ref, got[i].data(), 72);
    }
    return ok;
}

} // namespace

int main() {
    // ---------------------------------------------------------------- codes
    {
        int bad = 0;
        for (int d = 0; d < 16; d++) {
            const uint8_t c = duidEncode(d);
            if (duidDecode(c) != d) bad++;
            for (int b = 0; b < 8; b++) if (duidDecode((uint8_t)(c ^ (1 << b))) != d) bad++;
        }
        CHECK(bad == 0, "DUID (8,4) code: %d bad decodes (all values, every single bit error)", bad);
        int minD = 99;
        for (int a = 0; a < 16; a++) for (int b = a + 1; b < 16; b++) minD = std::min(minD, __builtin_popcount((unsigned)(duidEncode(a) ^ duidEncode(b))));
        CHECK(minD == 4, "DUID code distance %d", minD);
    }
    {
        std::mt19937 rng(5);
        int bad = 0;
        for (int i = 0; i < 512; i++) {
            uint64_t cw = ischEncode(i);
            for (int k = 0; k < 3; k++) cw ^= 1ull << (rng() % 40);
            if (ischDecode(cw) != i) bad++;
        }
        CHECK(bad == 0, "I-ISCH: %d of 512 not decoded with 3 bit errors", bad);
        int dist = 0;
        CHECK(ischDecode(kSisch, &dist) < 0 || dist > 4, "the S-ISCH is not mistaken for an I-ISCH (distance %d)", dist);
    }
    {
        uint8_t bits[168];
        for (int i = 0; i < 168; i++) bits[i] = (uint8_t)((i * 7 + 3) % 5 == 0);
        const uint16_t c = crc12(bits, 168);
        int undetected = 0;
        for (int i = 0; i < 168; i++) { bits[i] ^= 1; undetected += crc12(bits, 168) == c; bits[i] ^= 1; }
        CHECK(undetected == 0, "CRC-12 misses %d single bit errors", undetected);
    }
    {   // the scrambler: depends on the seed, balanced
        Scrambler a, b;
        a.seed(0xBEE00, 0x3A1, 0x293); b.seed(0xBEE00, 0x3A1, 0x294);
        int diff = 0, ones = 0;
        for (int k = 0; k < kSlotsPerSuperframe; k++)
            for (int i = 0; i < kPayloadBits; i++) { diff += a.bits(k)[i] != b.bits(k)[i]; ones += a.bits(k)[i]; }
        const int total = kSlotsPerSuperframe * kPayloadBits;
        CHECK(diff > total / 3 && ones > total * 2 / 5 && ones < total * 3 / 5, "scrambler: %d of %d differ with another NAC, %d ones", diff, total, ones);
    }

    // ---------------------------------------------------------------- MAC PDUs through the bursts
    {
        Scrambler scr; scr.seed(0xBEE00, 0x3A1, 0x293);
        uint8_t mi[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        struct Case { int duid; std::vector<uint8_t> mac; int op; };
        const Case cases[] = {
            {kFacch, macPtt(1234567, 1201, 0x80, 0, mi), kMacPtt},
            {kFacch, macEndPtt(1234567, 1201, 0x293), kMacEndPtt},
            {kSacch, macWithUser(kMacActive, 1201, 1234567, 0x80), kMacActive},
            {kSacchClear, macWithUser(kMacIdle, 77, 42, 0), kMacIdle},
            {kFacchClear, macPtt(99, 4321, 0x84, 0x1234, mi), kMacPtt},
        };
        for (const Case& c : cases) {
            uint8_t dib[kBurstDibits];
            burstBuild(c.duid, nullptr, nullptr, c.mac, scr.bits(3), dib);
            Burst b;
            burstParse(dib, scr.bits(3), b);
            CHECK(b.duid == c.duid && b.macPresent && b.macOk && b.mac.opcode == c.op, "%s %s: duid %d mac %d op %d", duidName(c.duid), macName(c.op), b.duid, b.macOk, b.mac.opcode);
            // symbol errors: 4 damaged hexbits are corrected (RS(45,26) and RS(52,30) correct up to 9 and 11)
            uint8_t d2[kBurstDibits];
            memcpy(d2, dib, sizeof d2);
            for (int k : {5, 30, 77, 120}) d2[k] ^= 3;
            burstParse(d2, scr.bits(3), b);
            CHECK(b.macOk && b.mac.opcode == c.op, "%s %s with 4 damaged dibits", duidName(c.duid), macName(c.op));
            // the wrong timeslot's scrambling: refused (scrambled DUIDs only)
            if (duidScrambled(c.duid)) { burstParse(dib, scr.bits(4), b); CHECK(!b.macOk, "%s refused with the wrong timeslot's scrambling", duidName(c.duid)); }
        }
        uint8_t dib[kBurstDibits];
        burstBuild(kFacch, nullptr, nullptr, macPtt(1234567, 1201, 0x84, 0x55AA, mi), scr.bits(0), dib);
        Burst b;
        burstParse(dib, scr.bits(0), b);
        CHECK(b.macOk && b.mac.src == 1234567 && b.mac.tg == 1201 && b.mac.algId == 0x84 && b.mac.keyId == 0x55AA && !memcmp(b.mac.mi, mi, 9),
              "MAC_PTT fields: src %d tg %d alg %02X key %04X", b.mac.src, b.mac.tg, b.mac.algId, b.mac.keyId);
        burstBuild(kSacch, nullptr, nullptr, macWithUser(kMacActive, 3003, 5005, 0xC0), scr.bits(10), dib);
        burstParse(dib, scr.bits(10), b);
        CHECK(b.macOk && b.mac.msgOpcode == 1 && b.mac.tg == 3003 && b.mac.src == 5005 && b.mac.svcOpts == 0xC0, "MAC_ACTIVE group voice channel user");
        burstBuild(kSacch, nullptr, nullptr, macWithUser(kMacActive, 700001, 5005, 0, false), scr.bits(11), dib);
        burstParse(dib, scr.bits(11), b);
        CHECK(b.macOk && b.mac.msgOpcode == 2 && b.mac.tg == 700001 && !b.mac.group, "MAC_ACTIVE unit to unit voice channel user");
    }
    {   // the encryption sync
        uint8_t essB[4][24], essA[168], mi[9] = {9, 8, 7, 6, 5, 4, 3, 2, 1}, mo[9];
        essBuild(0xAA, 0x0102, mi, essB, essA);
        essA[3] ^= 1; essA[100] ^= 1; essB[1][5] ^= 1;   // three bit errors
        int alg = 0, key = 0;
        CHECK(essParse(essB, essA, alg, key, mo) && alg == 0xAA && key == 0x0102 && !memcmp(mi, mo, 9), "ESS round trip with errors: alg %02X key %04X", alg, key);
    }
    {   // voice bursts: the frames come back in order
        Scrambler scr; scr.seed(1, 2, 3);
        uint8_t v[4][72];
        for (int f = 0; f < 4; f++) Generator::voiceFrame(1, (uint32_t)f, v[f]);
        uint8_t dib[kBurstDibits];
        burstBuild(k4V, v, nullptr, {}, scr.bits(7), dib);
        Burst b;
        burstParse(dib, scr.bits(7), b);
        bool same = b.duid == k4V && b.nVoice == 4;
        for (int f = 0; f < 4; f++) same = same && !memcmp(b.voice[f], v[f], 72);
        CHECK(same, "4V burst round trip");
        burstBuild(k2V, v, nullptr, {}, scr.bits(9), dib);
        burstParse(dib, scr.bits(9), b);
        CHECK(b.duid == k2V && b.nVoice == 2 && !memcmp(b.voice[0], v[0], 72) && !memcmp(b.voice[1], v[1], 72), "2V burst round trip");
    }

    // ---------------------------------------------------------------- the frame layer on a synthetic voice channel
    const int wacn = 0xBEE00, sys = 0x3A1, nac = 0x293;
    auto makeGen = [&]() {
        Generator g(wacn, sys, nac);
        GenCall a; a.chan = 0; a.startSf = 2; a.superframes = 4; a.tg = 1201; a.src = 1234567;
        GenCall b; b.chan = 1; b.startSf = 3; b.superframes = 5; b.tg = 2402; b.src = 7654321;
        GenCall c; c.chan = 0; c.startSf = 9; c.superframes = 2; c.tg = 3303; c.src = 42; c.algId = 0x84; c.keyId = 7;
        g.add(a); g.add(b); g.add(c);
        return g;
    };
    {
        Generator g = makeGen();
        std::vector<uint8_t> dib;
        for (int i = 0; i < 37; i++) dib.push_back((uint8_t)(i * 3 % 4));   // a little junk before the first superframe
        for (int s = 0; s < 14; s++) { auto v = g.superframe(s); dib.insert(dib.end(), v.begin(), v.end()); }
        Framer f; f.setSeed(wacn, sys, nac);
        CallTracker ct;
        Seen s;
        run(f, ct, dib, s);
        printf("  frame layer: %llu superframes, %llu bursts, MAC %llu good %llu bad; channel 0: %zu frames, channel 1: %zu frames\n",
               (unsigned long long)f.superframes(), (unsigned long long)f.bursts(), (unsigned long long)f.macOk(), (unsigned long long)f.macBad(), s.frames[0].size(), s.frames[1].size());
        CHECK(s.locks == 1 && s.lost == 0 && f.macBad() == 0, "locked once (%d), never lost (%d), no bad MAC PDU (%llu)", s.locks, s.lost, (unsigned long long)f.macBad());
        int first0 = -1, first1 = -1;
        const int ok0 = matching(s.frames[0], 0, &first0), ok1 = matching(s.frames[1], 1, &first1);
        // channel 0: two calls (4 + 2 superframes of 18 frames); channel 1: one call of 5 superframes
        CHECK(first0 == 0 && ok0 == (int)s.frames[0].size() && s.frames[0].size() == 18 * 6, "channel 0: %d of %zu frames are the generator's, in order from %d", ok0, s.frames[0].size(), first0);
        CHECK(first1 == 0 && ok1 == (int)s.frames[1].size() && s.frames[1].size() == 18 * 5, "channel 1: %d of %zu frames are the generator's, in order from %d", ok1, s.frames[1].size(), first1);
        CHECK(s.ptt[0] == 2 && s.ptt[1] == 1 && s.end[0] == 2 && s.end[1] == 1, "MAC_PTT %d / %d, MAC_END_PTT %d / %d", s.ptt[0], s.ptt[1], s.end[0], s.end[1]);
        CHECK(s.startCall[1].tg == 2402 && s.startCall[1].src == 7654321 && !s.startCall[1].encrypted, "channel 1's call: TG %d from %d", s.startCall[1].tg, s.startCall[1].src);
        CHECK(s.encStarts[0] == 1 && s.lastCall[0].tg == 3303 && s.lastCall[0].encrypted && s.lastCall[0].endSeen && s.lastCall[0].algId == 0x84,
              "channel 0's second call is encrypted (ALGID %02X), ended with MAC_END_PTT", s.lastCall[0].algId);
        CHECK(s.lastCall[1].endSeen && !ct.call(1).active, "channel 1's call ended with its END_PTT");
    }
    {   // joining in the middle of a call (no MAC_PTT heard): MAC_ACTIVE on the SACCH gives the talkgroup
        Generator g = makeGen();
        std::vector<uint8_t> dib;
        for (int s = 0; s < 8; s++) { auto v = g.superframe(s); if (s >= 4) dib.insert(dib.end(), v.begin() + (s == 4 ? 900 : 0), v.end()); }
        Framer f; f.setSeed(wacn, sys, nac);
        CallTracker ct;
        Seen s;
        bool late = false; int lateTg = -1;
        for (uint8_t d : dib) {
            f.push(d);
            for (const Event& e : f.take()) {
                ct.onEvent(e);
                if (e.chan == 1 && ct.call(1).active && ct.call(1).lateEntry && ct.call(1).tg >= 0) { late = true; lateTg = ct.call(1).tg; }
            }
        }
        CHECK(late && lateTg == 2402, "late entry: channel 1's talkgroup %d from MAC_ACTIVE", lateTg);
    }

    // ---------------------------------------------------------------- the modem
    auto modemRun = [&](double fs, double off, double snrDb, double cfo, const char* what) {
        Generator g = makeGen();
        std::vector<uint8_t> dib;
        for (int i = 0; i < 300; i++) dib.push_back((uint8_t)(i % 4));
        for (int s = 0; s < 14; s++) { auto v = g.superframe(s); dib.insert(dib.end(), v.begin(), v.end()); }
        std::vector<cf32> iq = modulate(dib, fs, off + cfo, 0.3f);
        if (snrDb < 90) {   // noise in the 12.5 kHz channel
            std::mt19937 rng(11);
            std::normal_distribution<float> nd(0.f, 1.f);
            const double sig = 0.3 * 0.3, n0 = sig / std::pow(10.0, snrDb / 10) * fs / 12500.0;
            const float sd = (float)std::sqrt(n0 / 2);
            for (cf32& v : iq) v += cf32(sd * nd(rng), sd * nd(rng));
        }
        Demod dm;
        dm.configure(fs, off);
        std::vector<uint8_t> got;
        for (size_t i = 0; i < iq.size(); i += 4096) dm.feed(iq.data() + i, std::min<size_t>(4096, iq.size() - i), got);
        Framer f; f.setSeed(wacn, sys, nac);
        CallTracker ct;
        Seen s;
        run(f, ct, got, s);
        int f0 = -1, f1 = -1;
        const int ok0 = matching(s.frames[0], 0, &f0), ok1 = matching(s.frames[1], 1, &f1);
        printf("  modem %s: %zu dibits from %zu, CFO %.0f Hz, channel 0 %d / %zu, channel 1 %d / %zu good frames, MAC %llu / %llu\n", what, got.size(), dib.size(), dm.cfoHz(),
               ok0, s.frames[0].size(), ok1, s.frames[1].size(), (unsigned long long)f.macOk(), (unsigned long long)(f.macOk() + f.macBad()));
        CHECK(s.frames[0].size() >= 18 * 5 && ok0 >= (int)s.frames[0].size() * 95 / 100, "%s: channel 0 frames %d / %zu", what, ok0, s.frames[0].size());
        CHECK(s.frames[1].size() >= 18 * 4 && ok1 >= (int)s.frames[1].size() * 95 / 100, "%s: channel 1 frames %d / %zu", what, ok1, s.frames[1].size());
        CHECK(s.ptt[1] == 1 && s.end[1] == 1 && s.startCall[1].tg == 2402, "%s: channel 1's call start and end", what);
    };
    modemRun(48000, 0, 99, 0, "48 kHz, clean");
    modemRun(500000, -60000, 25, 350, "500 kHz, 25 dB, 350 Hz off");
    return testSummary("p25 phase 2");
}
