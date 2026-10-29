// P25 Phase 2 TDMA outbound (core/src/p25/p25p2_*.cpp), checked against the two GPL-3 implementations it is derived from:
//   - the DUID codewords of SDRTrunk (DataUnitID.java) decode to their values through OP25's table (p25p2_duid.cc);
//   - the 128 I-ISCH codewords (OP25 p25p2_isch.cc) are at distance 16, decode with 7 bit errors, the S-ISCH is told apart;
//   - the scrambling sequence of SDRTrunk's LFSR (LinearFeedbackShiftRegister.java) equals OP25's (apps/tdma/lfsr.py, ported below with its
//     44 x 44 matrix) for OP25's and SDRTrunk's own example systems and others;
//   - CRC-12 equals OP25's buffer-division form (p25p2_tdma.cc crc12, ported below);
//   - the voice frame interleave is a permutation that matches OP25's first entries (p25p2_vf.cc);
// and on its own: RS(63,35) with erasures and errors, MAC PDUs through FACCH / SACCH (the punctured codes, errors corrected), ESS, the frame
// layer on a synthetic two-channel carrier laid out as OP25 / SDRTrunk read it (each channel's AMBE+2 code words in order to the vocoder,
// MAC_PTT / MAC_END_PTT, encryption, late entry, an inverted spectrum), and the H-DQPSK modem.
#include "dect2/p25/p25p2.h"
#include <cmath>
#include <cstring>
#include <random>
#include <set>
#include <vector>
using namespace dect2;
using namespace dect2::p25p2;
#include "../common/test_util.h"

namespace {

// ---- OP25 apps/tdma/lfsr.py (GPL-3, Max H. Parke KA1RBI), ported for the cross-check: the seed times the matrix M, then the 6-part register
const uint64_t kOp25M[44] = {
    0x88410800200ull, 0x44208400100ull, 0x22104200080ull, 0x11082100040ull,
    0x08841080020ull, 0x04420840010ull, 0x02210420008ull, 0x01108210004ull,
    0x00884108002ull, 0x00442084001ull, 0x00221042000ull, 0x00110821000ull,
    0x00088410800ull, 0x00044208400ull, 0x00022104200ull, 0x00011082100ull,
    0x00008841080ull, 0x00004420840ull, 0x00002210420ull, 0x00001108210ull,
    0x00000884108ull, 0x00000442084ull, 0x00000221042ull, 0x00000110821ull,
    0x00000088410ull, 0x00000044208ull, 0x00000022104ull, 0x00000011082ull,
    0x00000008841ull, 0x00000004420ull, 0x00000002210ull, 0x00000001108ull,
    0x00000000884ull, 0x00000000442ull, 0x00000000221ull, 0x00000000110ull,
    0x00000000088ull, 0x00000000044ull, 0x00000000022ull, 0x00000000011ull,
    0x00000000008ull, 0x00000000004ull, 0x00000000002ull, 0x00000000001ull,
};
uint64_t op25CycReg(uint64_t reg) {
    uint64_t s1 = reg >> 40 & 0xf, s2 = reg >> 35 & 0x1f, s3 = reg >> 29 & 0x3f, s4 = reg >> 24 & 0x1f, s5 = reg >> 10 & 0x3fff, s6 = reg & 0x3ff;
    const uint64_t cy1 = s1 >> 3 & 1, cy2 = s2 >> 4 & 1, cy3 = s3 >> 5 & 1, cy4 = s4 >> 4 & 1, cy5 = s5 >> 13 & 1, cy6 = s6 >> 9 & 1;
    s1 = (s1 << 1 & 0xf) | (cy1 ^ cy2);
    s2 = (s2 << 1 & 0x1f) | (cy1 ^ cy3);
    s3 = (s3 << 1 & 0x3f) | (cy1 ^ cy4);
    s4 = (s4 << 1 & 0x1f) | (cy1 ^ cy5);
    s5 = (s5 << 1 & 0x3fff) | (cy1 ^ cy6);
    s6 = (s6 << 1 & 0x3ff) | cy1;
    return s1 << 40 | s2 << 35 | s3 << 29 | s4 << 24 | s5 << 10 | s6;
}
std::vector<uint8_t> op25XorBits(int nac, int sysid, int wacn) {
    const uint64_t seed = 16777216ull * (uint64_t)wacn + 4096ull * (uint64_t)sysid + (uint64_t)nac;
    uint64_t reg = 0;
    for (int i = 0; i < 44; i++) if (seed >> (43 - i) & 1) reg ^= kOp25M[i];
    std::vector<uint8_t> s;
    for (int i = 0; i < 4320; i++) { s.push_back((uint8_t)(reg >> 43 & 1)); reg = op25CycReg(reg); }
    return s;
}
// ---- OP25 p25p2_tdma.cc crc12, ported: the bits with 12 zeros appended, divided by {1,1,0,0,0,1,0,0,1,0,1,1,1}, the remainder inverted
uint16_t op25Crc12(const uint8_t* bits, int len) {
    static const uint8_t poly[13] = {1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1};
    std::vector<uint8_t> buf((size_t)len + 12, 0);
    for (int i = 0; i < len; i++) buf[(size_t)i] = bits[i];
    for (int i = 0; i < len; i++) if (buf[(size_t)i]) for (int j = 0; j < 13; j++) buf[(size_t)(i + j)] ^= poly[j];
    uint16_t crc = 0;
    for (int i = 0; i < 12; i++) crc = (uint16_t)(crc << 1 | buf[(size_t)(len + i)]);
    return (uint16_t)(crc ^ 0xfff);
}

struct Seen {
    std::vector<std::vector<char>> fr[2];   // the code words each channel handed to the vocoder, in order
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
            if (e.kind == Event::Voice) for (int i = 0; i < e.nVoice; i++) { char fr[4][24]; voiceDeinterleave(e.voice[i], fr); s.fr[e.chan].emplace_back(&fr[0][0], &fr[0][0] + 96); }
            if (e.kind == Event::Mac) {
                if (e.mac.opcode == kMacPtt) { s.ptt[e.chan]++; s.startCall[e.chan] = c; if (c.encrypted) s.encStarts[e.chan]++; }
                if (e.mac.opcode == kMacEndPtt) s.end[e.chan]++;
            }
            if (was && !c.active) s.lastCall[e.chan] = c;
        }
    }
}

// how many of a channel's frames are the generator's, in order from the first; firstIdx: the generator's number of the first
int matching(const std::vector<std::vector<char>>& got, int chan, int* firstIdx) {
    if (got.empty()) return 0;
    auto ref = [&](int n) { uint32_t c[4]; Generator::voiceWords(chan, (uint32_t)n, c); char fr[4][24]; voiceFr(c, fr); return std::vector<char>(&fr[0][0], &fr[0][0] + 96); };
    int start = -1;
    for (int n = 0; n < 4000 && start < 0; n++) if (ref(n) == got[0]) start = n;
    if (firstIdx) *firstIdx = start;
    if (start < 0) return 0;
    int ok = 0;
    for (size_t i = 0; i < got.size(); i++) ok += ref(start + (int)i) == got[i];
    return ok;
}

} // namespace

int main() {
    {   // bursts from real Phase 2 voice channels: SigIDWiki "Phase2vicpol.mp3" and "APCO25P2voice.mp3" (discriminator audio), sliced to dibits
        // by a symbol slicer of our own (the recordings are not redistributed: only these 160-dibit timeslots, in hex, two dibits per digit).
        // The S-ISCH, the I-ISCH slot numbers and the DUIDs of those recordings decode with these tables; these unscrambled FACCH bursts
        // carry MAC_END_PTT, which passes RS(63,35) and CRC-12 (the second one only after the RS corrects it).
        struct Real { const char* hex; int tg; const char* src; };
        const Real r[] = {
            {"D70058C00000000000300000000FFFFFF0FFD55DDF5F554E42A3D892FB5EBD58B962704C2949581B", 3411, "Phase2vicpol.mp3"},
            {"D70058C00000000000300000000FFFFBF0FFD55DCF5E454E42A3D892FB5EBD58B962704C2949581B", 3411, "Phase2vicpol.mp3 (errors)"},
            {"D70058400000000000300000000FFFFFF7FFD55DDF5F5594288BCE4D8E862D397F4B6E4E10280A3F", 32101, "APCO25P2voice.mp3"},
        };
        static const uint8_t zero[320] = {};
        for (const Real& x : r) {
            uint8_t d[kBurstDibits];
            for (int k = 0; k < 80; k++) {
                const char ch = x.hex[k];
                const int v = ch <= '9' ? ch - '0' : ch - 'A' + 10;
                d[2 * k] = (uint8_t)(v >> 2); d[2 * k + 1] = (uint8_t)(v & 3);
            }
            Burst b;
            burstParse(d, zero, b);
            CHECK(b.duid == 15 && b.macOk && b.mac.opcode == kMacEndPtt && b.mac.tg == x.tg && b.mac.src == 0xFFFFFF,
                  "real burst %s: DUID %d, MAC ok %d, opcode %d, TG %d, source %X", x.src, b.duid, (int)b.macOk, b.mac.opcode, b.mac.tg, (unsigned)b.mac.src);
        }
    }

    // ---------------------------------------------------------------- DUID: SDRTrunk's codewords through OP25's table
    {
        static const uint8_t sdr[16] = {0x00, 0x17, 0x2E, 0x39, 0x4B, 0x5C, 0x65, 0x72, 0x8D, 0x9A, 0xA3, 0xB4, 0xC6, 0xD1, 0xE8, 0xFF};
        int bad = 0, single = 0;
        for (int d = 0; d < 16; d++) {
            if (duidEncode(d) != sdr[d] || duidDecode(sdr[d]) != d) bad++;
            for (int b = 0; b < 8; b++) if (duidDecode((uint8_t)(sdr[d] ^ (1 << b))) == d) single++;
        }
        CHECK(bad == 0, "DUID: %d of SDRTrunk's codewords do not decode to their value through OP25's table", bad);
        CHECK(single == 128, "DUID: %d of 128 single bit errors corrected", single);
    }
    // ---------------------------------------------------------------- ISCH (OP25 p25p2_isch.cc)
    {
        int minD = 99;
        for (int a = 0; a < 128; a++) for (int b = a + 1; b < 128; b++) minD = std::min(minD, __builtin_popcountll(ischEncode(a) ^ ischEncode(b)));
        CHECK(minD == 16, "I-ISCH distance %d (OP25: a (40,9,16) code)", minD);
        CHECK(ischEncode(0) == 0x184229D461ull && ischEncode(8) == 0x1146A44F13ull, "OP25's codewords 0 and 8");
        std::mt19937 rng(5);
        int bad = 0;
        for (int i = 0; i < 128; i++) {
            uint64_t cw = ischEncode(i);
            std::set<int> pos; while (pos.size() < 7) pos.insert((int)(rng() % 40));
            for (int p : pos) cw ^= 1ull << p;
            if (ischDecode(cw) != i) bad++;
        }
        CHECK(bad == 0, "I-ISCH: %d of 128 not decoded with 7 bit errors", bad);
        CHECK(ischDecode(kSisch) == -2 && ischDecode(kSisch ^ 0x30000000Full) == -2, "the S-ISCH (with 3 errors) is the sync");
        // the slots: I-ISCH before 0, 1, 4, 5, 8, 9 (OP25 p25p2_sync.cc expected_sync), the information gives them back
        int ok = 0;
        for (int k : {0, 1, 4, 5, 8, 9}) ok += ischSlot(ischDecode(ischEncode(ischInfo(k, 1, 2)))) == k && !ischIsSync(k);
        CHECK(ok == 6 && ischIsSync(2) && ischIsSync(11), "timeslots from the I-ISCH");
        CHECK(chanOfSlot(10) == 1 && chanOfSlot(11) == 0 && chanOfSlot(4) == 0 && chanOfSlot(7) == 1, "timeslot 10 is channel 1's SACCH, 11 channel 0's");
    }
    // ---------------------------------------------------------------- the scrambling sequence: SDRTrunk's LFSR = OP25's
    {
        struct Sys { int wacn, sys, nac; } systems[] = {{0x1, 0x18, 0x293}, {0xBEE00, 0x1C7, 0x1C1}, {0xBEE07, 0x40F, 0x04E}, {0xFFFFF, 0xFFF, 0xFFF}, {0x12345, 0x678, 0x9AB}};
        for (const Sys& y : systems) {
            Scrambler s;
            s.seed(y.wacn, y.sys, y.nac);
            const std::vector<uint8_t> op = op25XorBits(y.nac, y.sys, y.wacn);
            int diff = 0;
            for (int i = 0; i < 4320; i++) diff += s.sequence()[(size_t)i] != op[(size_t)i];
            uint64_t first = 0;
            for (int i = 0; i < 44; i++) first = first << 1 | s.sequence()[(size_t)i];
            CHECK(diff == 0 && first == scramblerSeed(y.wacn, y.sys, y.nac), "WACN %05X system %03X NAC %03X: %d of 4320 bits differ from OP25's sequence", y.wacn, y.sys, y.nac, diff);
        }
    }
    // ---------------------------------------------------------------- CRC-12 = OP25's
    {
        std::mt19937 rng(9);
        int bad = 0, undetected = 0;
        for (int k = 0; k < 200; k++) {
            uint8_t bits[168];
            for (uint8_t& b : bits) b = (uint8_t)(rng() & 1);
            const int len = k & 1 ? 168 : 144;
            const uint16_t c = crc12(bits, len);
            if (c != op25Crc12(bits, len)) bad++;
            bits[rng() % len] ^= 1;
            if (crc12(bits, len) == c) undetected++;
        }
        CHECK(bad == 0 && undetected == 0, "CRC-12: %d differ from OP25's, %d single errors undetected", bad, undetected);
    }
    // ---------------------------------------------------------------- RS(63,35) with erasures
    {
        std::mt19937 rng(3);
        int bad = 0, n = 0;
        for (int k = 0; k < 300; k++) {
            uint8_t cw[63] = {};
            for (int i = 0; i < 35; i++) cw[i] = (uint8_t)(rng() & 63);
            rs63Encode(cw);
            uint8_t rx[63];
            memcpy(rx, cw, 63);
            const int ne = (int)(rng() % 12), nerr = (28 - ne) / 2;
            std::set<int> pos;
            while ((int)pos.size() < ne + nerr) pos.insert((int)(rng() % 63));
            std::vector<int> eras;
            int i = 0;
            for (int p : pos) { if (i++ < ne) eras.push_back(p); rx[p] ^= (uint8_t)(1 + rng() % 63); }
            n++;
            if (!rs63Decode(rx, eras) || memcmp(rx, cw, 63)) bad++;
        }
        CHECK(bad == 0, "RS(63,35): %d of %d words with e erasures and (28 - e) / 2 errors not corrected", bad, n);
    }
    // ---------------------------------------------------------------- MAC PDUs through FACCH and SACCH
    {
        Scrambler scr; scr.seed(0xBEE00, 0x3A1, 0x293);
        uint8_t mi[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        struct Case { int duid; std::vector<uint8_t> mac; int op; int damage; };
        const Case cases[] = {
            {kFacch, macPtt(1234567, 1201, 0x80, 0, mi), kMacPtt, 9},
            {kFacch, macEndPtt(1234567, 1201, 0x293), kMacEndPtt, 9},
            {kSacch, macWithUser(kMacActive, 1201, 1234567, 0x80), kMacActive, 11},
            {kSacchClear, macWithUser(kMacIdle, 77, 42, 0), kMacIdle, 11},
            {kFacchClear, macPtt(99, 4321, 0x84, 0x1234, mi), kMacPtt, 9},
        };
        for (const Case& c : cases) {
            uint8_t dib[kBurstDibits];
            burstBuild(c.duid, nullptr, nullptr, c.mac, scr.bits(3), dib);
            Burst b;
            burstParse(dib, scr.bits(3), b);
            CHECK(b.duid == c.duid && b.macPresent && b.macOk && b.mac.opcode == c.op, "%s %s", duidName(c.duid), macName(c.op));
            // damaged hexbits: FACCH (9 punctured) corrects 9, SACCH (6 punctured) 11 (one dibit per hexbit, spread over the spans)
            uint8_t d2[kBurstDibits];
            memcpy(d2, dib, sizeof d2);
            const int at[11] = {2, 10, 20, 30, 40, 50, 60, 95, 110, 130, 150};
            for (int i = 0; i < c.damage; i++) d2[at[i]] ^= 3;
            burstParse(d2, scr.bits(3), b);
            CHECK(b.macOk && b.mac.opcode == c.op && b.rsCorrected >= c.damage - 2, "%s %s with %d damaged dibits (%d corrected)", duidName(c.duid), macName(c.op), c.damage, b.rsCorrected);
            if (duidScrambled(c.duid)) { burstParse(dib, scr.bits(4), b); CHECK(!b.macOk, "%s refused with the wrong timeslot's scrambling", duidName(c.duid)); }
        }
        uint8_t dib[kBurstDibits];
        burstBuild(kFacch, nullptr, nullptr, macPtt(1234567, 1201, 0x84, 0x55AA, mi), scr.bits(0), dib);
        Burst b;
        burstParse(dib, scr.bits(0), b);
        CHECK(b.macOk && b.mac.src == 1234567 && b.mac.tg == 1201 && b.mac.algId == 0x84 && b.mac.keyId == 0x55AA && !memcmp(b.mac.mi, mi, 9), "MAC_PTT fields");
        burstBuild(kFacch, nullptr, nullptr, macEndPtt(7654321, 2402, 0x293), scr.bits(1), dib);
        burstParse(dib, scr.bits(1), b);
        CHECK(b.macOk && b.mac.src == 7654321 && b.mac.tg == 2402 && b.mac.colorCode == 0x293, "MAC_END_PTT fields (colour code, source at byte 13, group at 16)");
        burstBuild(kSacch, nullptr, nullptr, macWithUser(kMacActive, 3003, 5005, 0xC0), scr.bits(10), dib);
        burstParse(dib, scr.bits(10), b);
        CHECK(b.macOk && b.mac.msgOpcode == 1 && b.mac.tg == 3003 && b.mac.src == 5005 && b.mac.svcOpts == 0xC0, "MAC_ACTIVE group voice channel user");
        burstBuild(kSacch, nullptr, nullptr, macWithUser(kMacActive, 700001, 5005, 0, false), scr.bits(11), dib);
        burstParse(dib, scr.bits(11), b);
        CHECK(b.macOk && b.mac.msgOpcode == 2 && b.mac.tg == 700001 && !b.mac.group, "MAC_ACTIVE unit to unit voice channel user");
        {   // the voice channel user message after other MAC messages (OP25 decode_mac_msg walks the list by mac_msg_len)
            std::vector<uint8_t> v = {(uint8_t)(kMacActive << 5), 0x08, 0x03, 0x00, 0x30, 0, 0, 0, 0, 0};   // null-avoid (3 bytes), then op 0x30 (5 bytes)
            v.resize(4);
            v.insert(v.end(), {0x30, 0x11, 0x22, 0x33, 0x44});
            v.insert(v.end(), {0x01, 0x40, 0x0B, 0xB9, 0x00, 0x13, 0x88});   // group voice channel user: svc 0x40, TG 3001, source 5000
            MacPdu m;
            CHECK(macParse(v.data(), (int)v.size(), m) && m.tg == 3001 && m.src == 5000 && m.svcOpts == 0x40 && m.group && m.msgOpcode == 0x30,
                  "the voice channel user as the third MAC message: tg %d src %d", m.tg, m.src);
        }
    }
    {   // the encryption sync, with three hexbits wrong
        uint8_t essB[16], essA[28], mi[9] = {9, 8, 7, 6, 5, 4, 3, 2, 1}, mo[9];
        essBuild(0xAA, 0x0102, mi, essB, essA);
        essA[3] ^= 5; essA[20] ^= 1; essB[7] ^= 33;
        int alg = 0, key = 0;
        CHECK(essParse(essB, essA, alg, key, mo) && alg == 0xAA && key == 0x0102 && !memcmp(mi, mo, 9), "ESS round trip with errors: alg %02X key %04X", alg, key);
    }
    {   // the voice interleave (OP25 p25p2_vf.cc): a permutation; dibit 0 is c0 bit 23 then c0 bit 5, dibit 1 c1 bit 10 then c2 bit 3
        int ok = 0;
        for (int k = 0; k < 4; k++) {
            static const int len[4] = {24, 23, 11, 14};
            for (int j = 0; j < len[k]; j++) {
                uint32_t c[4] = {0, 0, 0, 0};
                c[k] = 1u << j;
                uint8_t d[36];
                voiceInterleave(c, d);
                int ones = 0;
                for (uint8_t x : d) ones += __builtin_popcount(x);
                char fr[4][24];
                voiceDeinterleave(d, fr);
                ok += ones == 1 && fr[k][j] == 1;
            }
        }
        CHECK(ok == 72, "the voice interleave is a permutation of the 72 bits (%d)", ok);
        uint32_t c[4] = {1u << 23 | 1u << 5, 1u << 10, 1u << 3, 0};
        uint8_t d[36];
        voiceInterleave(c, d);
        CHECK(d[0] == 3 && d[1] == 3 && d[2] == 0, "OP25's first positions");
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
    for (int rev = 0; rev < 2; rev++) {
        Generator g = makeGen();
        std::vector<uint8_t> dib;
        for (int i = 0; i < 37; i++) dib.push_back((uint8_t)(i * 3 % 4));
        for (int s = 0; s < 14; s++) { auto v = g.superframe(s); dib.insert(dib.end(), v.begin(), v.end()); }
        if (rev) for (uint8_t& d : dib) d ^= 2;   // an inverted spectrum
        Framer f; f.setSeed(wacn, sys, nac);
        CallTracker ct;
        Seen s;
        run(f, ct, dib, s);
        printf("  frame layer%s: %llu superframes, %llu bursts, MAC %llu good %llu bad; channel 0: %zu frames, channel 1: %zu frames\n", rev ? " (inverted)" : "",
               (unsigned long long)f.superframes(), (unsigned long long)f.bursts(), (unsigned long long)f.macOk(), (unsigned long long)f.macBad(), s.fr[0].size(), s.fr[1].size());
        CHECK(s.locks == 1 && s.lost == 0 && f.macBad() == 0, "locked once (%d), never lost (%d), no bad MAC PDU (%llu)", s.locks, s.lost, (unsigned long long)f.macBad());
        int first0 = -1, first1 = -1;
        const int ok0 = matching(s.fr[0], 0, &first0), ok1 = matching(s.fr[1], 1, &first1);
        CHECK(first0 == 0 && ok0 == (int)s.fr[0].size() && s.fr[0].size() == 18 * 6, "channel 0: %d of %zu frames are the generator's, in order from %d", ok0, s.fr[0].size(), first0);
        CHECK(first1 == 0 && ok1 == (int)s.fr[1].size() && s.fr[1].size() == 18 * 5, "channel 1: %d of %zu frames are the generator's, in order from %d", ok1, s.fr[1].size(), first1);
        CHECK(s.ptt[0] == 2 && s.ptt[1] == 1 && s.end[0] == 2 && s.end[1] == 1, "MAC_PTT %d / %d, MAC_END_PTT %d / %d", s.ptt[0], s.ptt[1], s.end[0], s.end[1]);
        CHECK(s.startCall[1].tg == 2402 && s.startCall[1].src == 7654321 && !s.startCall[1].encrypted, "channel 1's call: TG %d from %d", s.startCall[1].tg, s.startCall[1].src);
        CHECK(s.encStarts[0] == 1 && s.lastCall[0].tg == 3303 && s.lastCall[0].encrypted && s.lastCall[0].endSeen && s.lastCall[0].algId == 0x84,
              "channel 0's second call is encrypted (ALGID %02X), ended with MAC_END_PTT", s.lastCall[0].algId);
    }
    {   // joining in the middle of a call (no MAC_PTT heard): MAC_ACTIVE on the SACCH gives the talkgroup, the ESS the encryption
        Generator g = makeGen();
        std::vector<uint8_t> dib;
        for (int s = 0; s < 8; s++) { auto v = g.superframe(s); if (s >= 4) dib.insert(dib.end(), v.begin() + (s == 4 ? 900 : 0), v.end()); }
        Framer f; f.setSeed(wacn, sys, nac);
        CallTracker ct;
        bool late = false, knownEarly = false; int lateTg = -1;
        for (uint8_t d : dib) {
            f.push(d);
            for (const Event& e : f.take()) {
                ct.onEvent(e);
                if (e.chan == 1 && e.kind == Event::Voice && ct.call(1).frames <= 4 && ct.call(1).cryptoKnown) knownEarly = true;
                if (e.chan == 1 && ct.call(1).active && ct.call(1).lateEntry && ct.call(1).tg >= 0) { late = true; lateTg = ct.call(1).tg; }
            }
        }
        CHECK(late && lateTg == 2402 && !knownEarly, "late entry: channel 1's talkgroup %d from MAC_ACTIVE, nothing known about encryption at its first voice", lateTg);
    }

    // ---------------------------------------------------------------- the modem
    auto modemRun = [&](double fs, double off, double snrDb, double cfo, const char* what) {
        Generator g = makeGen();
        std::vector<uint8_t> dib;
        for (int i = 0; i < 300; i++) dib.push_back((uint8_t)(i % 4));
        for (int s = 0; s < 14; s++) { auto v = g.superframe(s); dib.insert(dib.end(), v.begin(), v.end()); }
        std::vector<cf32> iq = modulate(dib, fs, off + cfo, 0.3f);
        if (snrDb < 90) {
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
        const int ok0 = matching(s.fr[0], 0, &f0), ok1 = matching(s.fr[1], 1, &f1);
        printf("  modem %s: CFO %.0f Hz, channel 0 %d / %zu, channel 1 %d / %zu good frames, MAC %llu / %llu\n", what, dm.cfoHz(),
               ok0, s.fr[0].size(), ok1, s.fr[1].size(), (unsigned long long)f.macOk(), (unsigned long long)(f.macOk() + f.macBad()));
        CHECK(s.fr[0].size() >= 18 * 5 && ok0 >= (int)s.fr[0].size() * 95 / 100, "%s: channel 0 frames %d / %zu", what, ok0, s.fr[0].size());
        CHECK(s.fr[1].size() >= 18 * 4 && ok1 >= (int)s.fr[1].size() * 95 / 100, "%s: channel 1 frames %d / %zu", what, ok1, s.fr[1].size());
        CHECK(s.ptt[1] == 1 && s.end[1] == 1 && s.startCall[1].tg == 2402, "%s: channel 1's call start and end", what);
    };
    modemRun(48000, 0, 99, 0, "48 kHz, clean");
    modemRun(500000, -60000, 25, 350, "500 kHz, 25 dB, 350 Hz off");
    return testSummary("p25 phase 2");
}
