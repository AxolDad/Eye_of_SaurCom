// LTR and Passport word encode / parse / check and the test-signal modulator (see ltr_proto.h for the sources).
// The checksum tables and the Passport message types and band plan are ported from SDRTrunk (GPL-3, Copyright Dennis Sheirer):
// src/main/java/io/github/dsheirer/edac/CRCLTR.java, edac/CRCPassport.java, module/decode/passport/PassportMessage.java and
// PassportBand.java. This file is GPL-3 like the rest of Eye of SaurCom.
#include "dect2/ltr/ltr_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2::ltr {

static void put(std::vector<uint8_t>& b, uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((v >> i) & 1); }
static uint32_t get(const uint8_t* b, int& p, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = (v << 1) | (b[p++] & 1); return v; }

// CRCLTR.sCHECKSUMS: the value XORed in for each set bit 9 - 32 (area, go-to 4..0, home 4..0, group 7..0, free 4..0)
static const uint8_t kLtrChk[24] = {0x38, 0x1C, 0x0E, 0x46, 0x23, 0x51, 0x68, 0x75, 0x7A, 0x3D, 0x1F, 0x4F,
                                    0x26, 0x52, 0x29, 0x15, 0x0B, 0x45, 0x62, 0x31, 0x19, 0x0D, 0x07, 0x43};

static uint32_t info24(const Word& w) {
    return ((uint32_t)(w.area & 1) << 23) | ((uint32_t)(w.gotoCh & 31) << 18) | ((uint32_t)(w.home & 31) << 13) | ((uint32_t)(w.id & 255) << 5) |
           (uint32_t)(w.freeCh & 31);
}

int checksum(const Word& w) {
    const uint32_t v = info24(w);
    int c = 0;
    for (int i = 0; i < 24; i++) if ((v >> (23 - i)) & 1) c ^= kLtrChk[i];
    return c;
}

int checkWord(const Word& w) {
    const int c = checksum(w);
    if (c == w.chk) return 1;
    if ((c ^ 127) == w.chk) return 2;
    return 0;
}

std::vector<uint8_t> encodeWord(const Word& w, bool isw) {
    std::vector<uint8_t> b;
    put(b, kSync, 9);
    put(b, info24(w), 24);
    put(b, (uint32_t)checksum(w), 7);
    if (isw) for (auto& x : b) x ^= 1;
    return b;
}

bool parseWord(const uint8_t* b, Word& w) {
    int p = 0;
    if (get(b, p, 9) != kSync) return false;
    w.area = (int)get(b, p, 1);
    w.gotoCh = (int)get(b, p, 5);
    w.home = (int)get(b, p, 5);
    w.id = (int)get(b, p, 8);
    w.freeCh = (int)get(b, p, 5);
    w.chk = (int)get(b, p, 7);
    return true;
}

// ---- Passport

// CRCPassport.sCHECKSUMS: bits 9 - 59 (DCC 1..0, LCN 10..0, SITE 6..0, GROUP 15..0, TYPE 3..0, FREE 10..0)
static const uint8_t kPassChk[51] = {
    0x6E, 0xBF, 0xD6, 0xE3, 0xF8, 0x7C, 0x3E, 0x97, 0xC2, 0xE9, 0x75, 0x3B, 0x94, 0x4A, 0xAD, 0x57, 0xA2, 0xD9, 0x6D, 0x37, 0x92, 0xC1, 0x61, 0x31, 0x19, 0x0D,
    0x07, 0x8A, 0xCD, 0x67, 0xBA, 0xD5, 0x6B, 0xBC, 0x5E, 0xA7, 0xDA, 0xE5, 0x73, 0xB0, 0x58, 0x2C, 0x16, 0x83, 0xC8, 0x64, 0x32, 0x91, 0x49, 0x25, 0x13};

static uint64_t passInfo(const PassportWord& p) {   // bits 9 - 59 as a 51-bit number, bit 9 the MSB
    return ((uint64_t)(p.dcc & 3) << 49) | ((uint64_t)(p.lcn & 0x7FF) << 38) | ((uint64_t)(p.site & 0x7F) << 31) | ((uint64_t)(p.group & 0xFFFF) << 15) |
           ((uint64_t)(p.type & 0xF) << 11) | (uint64_t)(p.freeCh & 0x7FF);
}

int passportChecksum(const PassportWord& p) {
    const uint64_t v = passInfo(p);
    int c = 0;
    for (int i = 0; i < 51; i++) if ((v >> (50 - i)) & 1) c ^= kPassChk[i];
    return c;
}

std::vector<uint8_t> encodePassport(const PassportWord& p) {
    std::vector<uint8_t> b;
    put(b, kSync, 9);
    const uint64_t v = passInfo(p);
    for (int i = 50; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1));
    put(b, (uint32_t)passportChecksum(p), 8);
    return b;
}

bool parsePassport(const uint8_t* b, PassportWord& w) {
    int p = 0;
    if (get(b, p, 9) != kSync) return false;
    w.dcc = (int)get(b, p, 2);
    w.lcn = (int)get(b, p, 11);
    w.site = (int)get(b, p, 7);
    w.group = (int)get(b, p, 16);
    w.type = (int)get(b, p, 4);
    w.freeCh = (int)get(b, p, 11);
    w.chk = (int)get(b, p, 8);
    return true;
}

bool passportOk(const PassportWord& p) {
    // CRCPassport.check() after correct(): a residual of 0 passes. correct() flips check bits 66 / 67 for residuals 1, 2 and 3 (which then
    // pass); its other cases (136, 138, 139, 140, 142, 153) flip check bits too but leave a residual of 137 or 152, which check() refuses.
    const int r = passportChecksum(p) ^ p.chk;
    return r >= 0 && r <= 3;
}

PassportKind passportKind(const PassportWord& p) {   // PassportMessage.getMessageType()
    switch (p.type) {
    case 0: case 2: return PassportKind::CallStart;
    case 1:
        if (p.freeCh == 2042) return PassportKind::TgAssign;
        if (p.lcn < 1792) return PassportKind::CallStart;
        if (p.lcn == 1792 || p.lcn == 1793) return PassportKind::Idle;
        if (p.lcn == 2047) return PassportKind::CallEnd;
        return PassportKind::Unknown;
    case 5: return PassportKind::Page;
    case 6: return PassportKind::RadioId;
    case 9: return PassportKind::Data;
    case 11: return PassportKind::Register;
    default: return PassportKind::Unknown;
    }
}

const char* passportKindName(PassportKind k) {
    switch (k) {
    case PassportKind::CallStart: return "call";
    case PassportKind::TgAssign: return "talkgroup assignment";
    case PassportKind::Idle: return "idle";
    case PassportKind::CallEnd: return "call end";
    case PassportKind::Page: return "page";
    case PassportKind::RadioId: return "radio ID";
    case PassportKind::Data: return "data";
    case PassportKind::Register: return "registration";
    default: return "unknown";
    }
}

double passportBandBaseHz(int band) {   // PassportBand: 800, 900, 400, 420 ... 490, CAN, NTIA, reserved, 216, reserved, 700
    static const double base[16] = {800e6, 900e6, 400e6, 420e6, 440e6, 450e6, 460e6, 470e6, 480e6, 490e6, 0, 0, 0, 216e6, 0, 700e6};
    return band >= 0 && band < 16 ? base[band] : 0;
}

const char* passportBandName(int band) {
    static const char* n[16] = {"800", "900", "400", "420", "440", "450", "460", "470", "480", "490", "CAN", "NTIA", "reserved", "216", "reserved", "700"};
    return band >= 0 && band < 16 ? n[band] : "unknown";
}

bool plausible(const Word& w) {
    return w.home >= 1 && w.home <= 20 && (w.gotoCh <= 20 || w.gotoCh >= 29) && (w.freeCh <= 20 || w.freeCh == 31);
}

std::string talkgroup(const Word& w) {
    char s[24];
    snprintf(s, sizeof s, "%d-%02d-%03d", w.area, w.home, w.id);
    return s;
}

std::string kindOf(int id) {
    if (id == 255) return "idle";
    if (id == 254) return "callsign ID";
    if (id == 253) return "busy";
    if (id >= 240 && id <= 247) return "token";
    return "";
}

std::string tgText(int n) {
    char s[24];
    snprintf(s, sizeof s, "%d-%02d-%03d", n / 100000, (n / 1000) % 100, n % 1000);
    return s;
}

std::vector<cf32> transmission(double fs, double offsetHz, int area, int rep, int home, int id, double voiceSec, double toneHz, int turnOff, float amp) {
    std::vector<uint8_t> bits;
    for (int i = 0; i < 40; i++) bits.push_back((uint8_t)(i & 1));
    Word w; w.area = area; w.gotoCh = rep; w.home = home; w.id = id; w.freeCh = rep == 1 ? 2 : 1;
    const int words = std::max(1, (int)(voiceSec * kBaud / 52));
    for (int k = 0; k < words; k++) {
        const auto e = encodeWord(w);
        bits.insert(bits.end(), e.begin(), e.end());
        for (int i = 0; i < 12; i++) bits.push_back((uint8_t)((i * 5 + k) & 1));
    }
    std::vector<cf32> out = modulate(bits, fs, offsetHz, 500.0, amp, toneHz, 2500.0);
    if (turnOff > 0) {
        std::vector<uint8_t> tb;
        Word t = w; t.gotoCh = kTurnOff;
        for (int k = 0; k < turnOff; k++) {
            const auto e = encodeWord(t);
            tb.insert(tb.end(), e.begin(), e.end());
            for (int i = 0; i < 12; i++) tb.push_back((uint8_t)(i & 1));
        }
        const auto o = modulate(tb, fs, offsetHz, 500.0, amp);
        out.insert(out.end(), o.begin(), o.end());
    }
    return out;
}

std::vector<cf32> modulate(const std::vector<uint8_t>& bits, double fs, double offsetHz, double devHz, float amp, double voiceHz, double voiceDevHz, bool invert) {
    const double pi = 3.14159265358979323846;
    const size_t n = (size_t)std::llround((double)bits.size() / kBaud * fs);
    std::vector<cf32> out(n);
    double phase = 0, lev = 0, vph = 0;
    const double a = 1.0 - std::exp(-2 * pi * 150.0 / fs);   // the data is smoothed to the subaudible band
    for (size_t i = 0; i < n; i++) {
        size_t k = (size_t)((double)i * kBaud / fs);
        if (k >= bits.size()) k = bits.size() - 1;
        const double target = (bits[k] ? 1.0 : -1.0) * (invert ? -1.0 : 1.0);
        lev += a * (target - lev);
        double f = lev * devHz + offsetHz;
        if (voiceHz > 0) { f += voiceDevHz * std::sin(vph); vph += 2 * pi * voiceHz / fs; }
        phase += 2 * pi * f / fs;
        if (phase > pi) phase -= 2 * pi; else if (phase < -pi) phase += 2 * pi;
        out[i] = cf32(amp * (float)std::cos(phase), amp * (float)std::sin(phase));
    }
    return out;
}

} // namespace dect2::ltr
