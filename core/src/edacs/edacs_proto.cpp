// EDACS frame, BCH(40,28), majority vote, messages and test signals, derived from dsd-fme (ISC licence; edacs_proto.h names the files).
#include "dect2/edacs/edacs_proto.h"
#include <cmath>
#include <cstdio>
#include <map>

namespace dect2::edacs {

// ------------------------------------------------------------------------------------------------ BCH (dsd-fme src/edacs-bch3.c)

uint32_t bchGenerator() {
    // gen_poly() of edacs-bch3.c: GF(2^6) from x^6 + x + 1, t = 2: the product of (x - alpha^j) over the cyclotomic cosets of 1 .. 2t
    static const uint32_t g = [] {
        int exp[63], x = 1;
        for (int i = 0; i < 63; i++) { exp[i] = x; x <<= 1; if (x & 64) x ^= 0x43; }
        auto mul = [&](int a, int b) { if (!a || !b) return 0; int la = 0, lb = 0; while (exp[la] != a) la++; while (exp[lb] != b) lb++; return exp[(la + lb) % 63]; };
        bool in[63] = {};
        for (int r = 1; r <= 4; r++) { int j = r; do { in[j] = true; j = j * 2 % 63; } while (j != r); }
        int poly[64] = {1};   // coefficients in GF(64), poly[i] of x^i
        int deg = 0;
        for (int j = 1; j < 63; j++) {
            if (!in[j]) continue;
            for (int i = deg + 1; i > 0; i--) poly[i] = poly[i - 1] ^ mul(poly[i], exp[j]);
            poly[0] = mul(poly[0], exp[j]);
            deg++;
        }
        uint32_t v = 0;
        for (int i = 0; i <= deg; i++) v |= (uint32_t)(poly[i] & 1) << i;   // the product has binary coefficients
        return v;
    }();
    return g;
}

uint32_t bchCheck(uint32_t data28) {   // encode_bch(): the remainder of x^12 d(x) / g(x)
    const uint64_t gen = bchGenerator();
    uint64_t r = (uint64_t)(data28 & 0xFFFFFFF) << 12;
    for (int i = 39; i >= 12; i--) if (r >> i & 1) r ^= gen << (i - 12);
    return (uint32_t)(r & 0xFFF);
}

uint64_t bchEncode(uint32_t data28) { return ((uint64_t)(data28 & 0xFFFFFFF) << 12) | bchCheck(data28); }

namespace {
uint32_t syndrome(uint64_t w) {
    const uint64_t gen = bchGenerator();
    for (int i = 39; i >= 12; i--) if (w >> i & 1) w ^= gen << (i - 12);
    return (uint32_t)(w & 0xFFF);
}
const std::map<uint32_t, uint64_t>& errorTable() {   // every pattern of one or two errors by its syndrome (distance 5: all different)
    static const std::map<uint32_t, uint64_t> t = [] {
        std::map<uint32_t, uint64_t> m;
        for (int i = 0; i < 40; i++) {
            m[syndrome(1ull << i)] = 1ull << i;
            for (int j = i + 1; j < 40; j++) m[syndrome((1ull << i) | (1ull << j))] = (1ull << i) | (1ull << j);
        }
        return m;
    }();
    return t;
}
} // namespace

bool bchDecode(uint64_t& w, int* corrected) {
    w &= 0xFFFFFFFFFFull;
    if (corrected) *corrected = 0;
    const uint32_t s = syndrome(w);
    if (!s) return true;
    const auto& t = errorTable();
    const auto it = t.find(s);
    if (it == t.end()) return false;
    w ^= it->second;
    if (corrected) *corrected = __builtin_popcountll(it->second);
    return true;
}

// ------------------------------------------------------------------------------------------------ frame

static void put(std::vector<uint8_t>& b, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1)); }

std::vector<uint8_t> encodeFrame(uint32_t msgA, uint32_t msgB) {
    std::vector<uint8_t> b;
    put(b, kSync, kSyncBits);
    for (uint32_t m : {msgA, msgB}) {
        const uint64_t w = bchEncode(m);
        put(b, w, 40);
        put(b, ~w & 0xFFFFFFFFFFull, 40);
        put(b, w, 40);
    }
    return b;
}

uint64_t vote(const uint8_t* b, int& dis) {   // dsd-fme edacsVoteFr
    uint64_t v = 0;
    dis = 0;
    for (int i = 0; i < kMsgBits; i++) {
        const int x = b[i] & 1, y = (b[40 + i] & 1) ^ 1, z = b[80 + i] & 1;
        const int s = x + y + z;
        if (s == 1 || s == 2) dis++;
        v = (v << 1) | (uint64_t)(s >= 2);
    }
    return v;
}

Decoded decodeMessage(const uint8_t* bits120) {
    Decoded d;
    uint64_t w = vote(bits120, d.disagree);
    d.bchOk = bchDecode(w, &d.corrected);
    d.data = (uint32_t)(w >> 12) & 0xFFFFFFF;
    d.ok = d.bchOk;
    return d;
}

// ------------------------------------------------------------------------------------------------ messages (dsd-fme src/edacs-fme.c, edacs())

const char* lcnStatus(int lcn) {
    if (lcn == 26 || lcn == 27) return "reserved status";
    if (lcn == 28) return "convert to callee";
    if (lcn == 29) return "call queued";
    if (lcn == 30) return "system busy";
    if (lcn == 31) return "call denied";
    return "";
}

Message parse(uint32_t m1, uint32_t m2, Mode mode, uint32_t esk) {
    Message m;
    m1 = (m1 ^ (esk << 20)) & 0xFFFFFFF;
    m2 = (m2 ^ (esk << 20)) & 0xFFFFFFF;
    m.m1 = m1; m.m2 = m2;
    if (mode == Mode::Extended) {
        const int mt1 = (int)(m1 >> 23) & 0x1F, mt2 = (int)(m1 >> 19) & 0xF;
        if (mt1 == 0x1F) {
            if (mt2 == 0x0) { m.kind = Message::TestCall; m.ccLcn = (int)(m1 >> 13) & 0x1F; m.lcn = (int)(m1 >> 7) & 0x1F; }
            else if (mt2 == 0x8) { m.kind = Message::SystemInfo; m.sysId = (int)(m1 & 0xFFFF); m.ccLcn = (int)(m2 & 0x1F); }
            else if (mt2 == 0xA) { m.kind = Message::SiteId; m.site = (int)(((m1 & 0x7000) >> 7) | (m1 & 0x1F)); m.area = (int)(m1 >> 5) & 0x7F; }
            else if (mt2 == 0xC) { m.kind = Message::Patch; m.group = (int)(m1 & 0xFF); m.target = (int)(m2 & 0xFFFFF); }
        } else if (mt1 == 0x3 || mt1 == 0x6) {   // group voice grant (0x3 digital / ProVoice, 0x6 analog)
            m.kind = Message::GroupVoice;
            m.digital = mt1 == 0x3;
            m.lcn = (int)(m1 >> 17) & 0x1F; m.update = (m1 >> 16) & 1; m.group = (int)(m1 & 0xFFFF);
            m.emergency = (m2 >> 20) & 1; m.source = (int)(m2 & 0xFFFFF);
        } else if (mt1 == 0x10) {   // individual call
            m.kind = Message::IndivVoice; m.individual = true;
            m.lcn = (int)(m2 >> 20) & 0x1F; m.digital = (m1 >> 21) & 1; m.update = (m1 >> 20) & 1;
            m.target = (int)(m1 & 0xFFFFF); m.source = (int)(m2 & 0xFFFFF);
            if (m.target == 0 && m.source == 0) m.kind = Message::TestCall;
        } else if (mt1 == 0x2) {   // data group grant
            m.kind = Message::DataCall;
            m.lcn = (int)(m1 >> 17) & 0x1F; m.group = (int)(m1 & 0xFFFF); m.source = (int)(m2 & 0xFFFFF);
        } else if (mt1 == 0x16) {   // system all-call
            m.kind = Message::AllCall;
            m.lcn = (int)(m1 >> 17) & 0x1F; m.digital = (m1 >> 16) & 1; m.update = (m1 >> 15) & 1; m.group = 0; m.source = (int)(m2 & 0xFFFFF);
        }
        return m;
    }
    const int mta = (int)(m1 >> 25) & 7, mtb = (int)(m1 >> 22) & 7, mtd = (int)(m1 >> 17) & 0x1F;
    if (mta <= 3) {   // voice group channel assignment (TSB-69.3 6.2.4.1 / 6.2.4.2)
        m.kind = Message::GroupVoice;
        m.digital = mta & 2; m.emergency = mta & 1;
        m.source = (int)(((m1 & 0x1FC0000) >> 11) | ((m2 & 0xFE0000) >> 17));
        m.lcn = (int)(m1 >> 12) & 0x1F; m.group = (int)(m1 & 0x7FF);
    } else if (mta == 5) {   // data call channel assignment (6.2.4.3)
        m.kind = Message::DataCall;
        m.lcn = (int)(m1 >> 15) & 0x1F; m.individual = (m1 >> 14) & 1;
        if (m.individual) m.target = (int)(m1 & 0x3FFF); else m.group = (int)(m1 & 0x7FF);
    } else if (mta == 7) {
        if (mtb == 1) {   // interconnect channel assignment (6.2.4.6)
            const int mtc = (int)(m1 >> 20) & 3;
            m.kind = Message::Interconnect;
            m.lcn = (int)(m1 >> 15) & 0x1F; m.individual = (m1 >> 14) & 1; m.digital = mtc == 2 || mtc == 3;
            if (m.individual) m.target = (int)(m1 & 0x3FFF); else m.group = (int)(m1 & 0x7FF);
        } else if (mtb == 3) {   // channel update (6.2.4.7)
            const int mtc = (int)(m1 >> 20) & 3;
            m.kind = Message::ChannelUpdate; m.update = true;
            m.lcn = (int)(m1 >> 15) & 0x1F; m.individual = (m1 >> 14) & 1; m.digital = mtc == 1 || mtc == 3;
            if (m.individual) { m.target = (int)(m1 & 0x3FFF); m.source = (int)(m2 & 0x3FFF); }
            else { m.group = (int)(m1 & 0x7FF); m.emergency = (m1 >> 13) & 1; }
        } else if (mtb == 5) {   // individual call channel assignment (6.2.4.9)
            m.kind = Message::IndivVoice; m.individual = true;
            m.lcn = (int)(m1 >> 15) & 0x1F; m.digital = (m1 >> 14) & 1; m.target = (int)(m1 & 0x3FFF); m.source = (int)(m2 & 0x3FFF);
            if (m.target == 0 && m.source == 0) m.kind = Message::TestCall;
        } else if (mtb == 6) {   // console unkey / drop (6.2.4.10)
            m.kind = Message::UnkeyDrop;
            m.drop = (m1 >> 19) & 1; m.lcn = (int)(m1 >> 14) & 0x1F; m.source = (int)(m1 & 0x3FFF);
        } else if (mtb == 7) {
            if (mtd >= 0x08 && mtd <= 0x0B) { m.kind = Message::SiteId; m.ccLcn = (int)(m1 >> 12) & 0x1F; m.site = (int)(m1 & 0x1F); }   // 6.2.4.18
            else if (mtd == 0x0F) {   // system all-call (6.2.4.19)
                m.kind = Message::AllCall;
                m.lcn = (int)(m1 >> 12) & 0x1F; m.digital = (m1 >> 11) & 1; m.update = (m1 >> 10) & 1; m.group = 0;
                m.source = (int)((m1 & 0x7F) | ((m2 & 0xFE) << 6));
            } else if (mtd == 0x06) { m.kind = Message::TestCall; m.ccLcn = (int)(m1 >> 12) & 0x1F; m.lcn = (int)(m1 >> 7) & 0x1F; }   // 6.2.4.16
        }
    }
    return m;
}

void build(const Message& m, Mode mode, uint32_t& a, uint32_t& b, uint32_t esk) {
    a = b = 0;
    auto L = [](int v) { return (uint32_t)(v < 0 ? 0 : v); };
    if (mode == Mode::Extended) {
        switch (m.kind) {
        case Message::GroupVoice:
            a = (m.digital ? 0x3u : 0x6u) << 23 | (L(m.lcn) & 0x1F) << 17 | (uint32_t)m.update << 16 | (L(m.group) & 0xFFFF);
            b = (uint32_t)m.emergency << 20 | (L(m.source) & 0xFFFFF) | 1u << 21;
            break;
        case Message::IndivVoice:
            a = 0x10u << 23 | (uint32_t)m.digital << 21 | (uint32_t)m.update << 20 | (L(m.target) & 0xFFFFF);
            b = (L(m.lcn) & 0x1F) << 20 | (L(m.source) & 0xFFFFF);
            break;
        case Message::DataCall: a = 0x2u << 23 | (L(m.lcn) & 0x1F) << 17 | (L(m.group) & 0xFFFF); b = L(m.source) & 0xFFFFF; break;
        case Message::AllCall: a = 0x16u << 23 | (L(m.lcn) & 0x1F) << 17 | (uint32_t)m.digital << 16 | (uint32_t)m.update << 15; b = L(m.source) & 0xFFFFF; break;
        case Message::SystemInfo: a = 0x1Fu << 23 | 0x8u << 19 | (L(m.sysId) & 0xFFFF); b = L(m.ccLcn) & 0x1F; break;
        case Message::SiteId: a = 0x1Fu << 23 | 0xAu << 19 | (L(m.site) & 0xE0) << 7 | (L(m.area) & 0x7F) << 5 | (L(m.site) & 0x1F); break;
        default: a = m.m1; b = m.m2; break;
        }
    } else {
        switch (m.kind) {
        case Message::GroupVoice: {
            const uint32_t src = L(m.source) & 0x3FFF;
            a = ((m.digital ? 2u : 0u) | (m.emergency ? 1u : 0u)) << 25 | (src >> 7 & 0x7F) << 18 | (L(m.lcn) & 0x1F) << 12 | 1u << 11 | (L(m.group) & 0x7FF);
            b = (src & 0x7F) << 17;
            break;
        }
        case Message::IndivVoice: a = 7u << 25 | 5u << 22 | (L(m.lcn) & 0x1F) << 15 | (uint32_t)m.digital << 14 | (L(m.target) & 0x3FFF); b = L(m.source) & 0x3FFF; break;
        case Message::ChannelUpdate:
            a = 7u << 25 | 3u << 22 | (m.digital ? 1u : 0u) << 20 | (L(m.lcn) & 0x1F) << 15 | (uint32_t)m.individual << 14;
            if (m.individual) { a |= L(m.target) & 0x3FFF; b = L(m.source) & 0x3FFF; } else a |= (uint32_t)m.emergency << 13 | (L(m.group) & 0x7FF);
            break;
        case Message::DataCall: a = 5u << 25 | (L(m.lcn) & 0x1F) << 15 | (uint32_t)m.individual << 14 | (m.individual ? L(m.target) & 0x3FFF : L(m.group) & 0x7FF); break;
        case Message::UnkeyDrop: a = 7u << 25 | 6u << 22 | (uint32_t)m.drop << 19 | (L(m.lcn) & 0x1F) << 14 | (L(m.source) & 0x3FFF); break;
        case Message::SiteId: a = 7u << 25 | 7u << 22 | 0x08u << 17 | (L(m.ccLcn) & 0x1F) << 12 | (L(m.site) & 0x1F); break;
        case Message::AllCall:
            a = 7u << 25 | 7u << 22 | 0x0Fu << 17 | (L(m.lcn) & 0x1F) << 12 | (uint32_t)m.digital << 11 | (uint32_t)m.update << 10 | (L(m.source) & 0x7F);
            b = (L(m.source) >> 7 & 0x7F) << 1;
            break;
        default: a = m.m1; b = m.m2; break;
        }
    }
    a = (a ^ (esk << 20)) & 0xFFFFFFF;
    b = (b ^ (esk << 20)) & 0xFFFFFFF;
}

std::string afsText(int g, const AfsSplit& s) {
    if (g < 0) return "-";
    const int sb = s.subfleetBits, fb = s.fleetBits;
    char b[32];
    snprintf(b, sizeof b, "%02d-%02d%d", g >> (fb + sb), (g >> sb) & ((1 << fb) - 1), g & ((1 << sb) - 1));
    return b;
}

std::string describe(const Message& m, Mode mode) {
    char b[200];
    const char* how = m.digital ? "digital (ProVoice)" : "analog";
    const std::string grp = mode == Mode::Standard ? std::to_string(m.group) + " (" + afsText(m.group) + ")" : std::to_string(m.group);
    switch (m.kind) {
    case Message::GroupVoice: snprintf(b, sizeof b, "%s group call %s: group %s, LCN %d%s%s", how, m.update ? "update" : "assignment", grp.c_str(), m.lcn, m.emergency ? ", EMERGENCY" : "", lcnStatus(m.lcn)); return b;
    case Message::IndivVoice: snprintf(b, sizeof b, "%s individual call: %d -> %d, LCN %d", how, m.source, m.target, m.lcn); return b;
    case Message::AllCall: snprintf(b, sizeof b, "%s system all-call from %d, LCN %d", how, m.source, m.lcn); return b;
    case Message::DataCall: snprintf(b, sizeof b, "data call to %s %d, LCN %d", m.individual ? "LID" : "group", m.individual ? m.target : m.group, m.lcn); return b;
    case Message::Interconnect: snprintf(b, sizeof b, "interconnect call, LCN %d", m.lcn); return b;
    case Message::ChannelUpdate:
        if (m.individual) snprintf(b, sizeof b, "%s individual channel update: %d -> %d, LCN %d", how, m.source, m.target, m.lcn);
        else snprintf(b, sizeof b, "%s group channel update: group %s, LCN %d%s", how, grp.c_str(), m.lcn, m.emergency ? ", EMERGENCY" : "");
        return b;
    case Message::UnkeyDrop: snprintf(b, sizeof b, "console %s, LCN %d", m.drop ? "drop" : "unkey", m.lcn); return b;
    case Message::SiteId: snprintf(b, sizeof b, "site ID %d%s, control channel LCN %d", m.site, m.area >= 0 ? (", area " + std::to_string(m.area)).c_str() : "", m.ccLcn); return b;
    case Message::SystemInfo: snprintf(b, sizeof b, "system ID %04X, control channel LCN %d", m.sysId, m.ccLcn); return b;
    case Message::TestCall: snprintf(b, sizeof b, "test call, working channel LCN %d", m.lcn); return b;
    case Message::Patch: snprintf(b, sizeof b, "dynamic regroup: group %d, target %d", m.group, m.target); return b;
    default: snprintf(b, sizeof b, "%07X %07X", m.m1, m.m2); return b;
    }
}

// ------------------------------------------------------------------------------------------------ test signals

std::vector<cf32> modulate(const std::vector<uint8_t>& bits, double fs, double offsetHz, double devHz, float amp, bool invert, double baud) {
    const double pi = 3.14159265358979323846;
    const size_t n = (size_t)std::llround((double)bits.size() / baud * fs);
    std::vector<cf32> out(n);
    double phase = 0, lev = 0;
    const double a = 1.0 - std::exp(-2 * pi * 0.75 * baud / fs);   // a little edge smoothing
    for (size_t i = 0; i < n; i++) {
        size_t k = (size_t)((double)i * baud / fs);
        if (k >= bits.size()) k = bits.size() - 1;
        lev += a * ((bits[k] ? 1.0 : -1.0) * (invert ? -1.0 : 1.0) - lev);
        phase += 2 * pi * (lev * devHz + offsetHz) / fs;
        if (phase > pi) phase -= 2 * pi; else if (phase < -pi) phase += 2 * pi;
        out[i] = cf32(amp * (float)std::cos(phase), amp * (float)std::sin(phase));
    }
    return out;
}

std::vector<cf32> workingChannel(double fs, double offsetHz, double voiceSec, double toneHz, int dropBits, float amp, bool provoice, uint32_t seed) {
    const double pi = 3.14159265358979323846;
    std::vector<float> fm;   // instantaneous frequency (Hz) per sample
    const size_t nv = (size_t)(voiceSec * fs);
    uint32_t r = seed * 2654435761u + 1;
    auto rnd = [&]() { r = r * 1664525u + 1013904223u; return (r >> 16) & 1; };
    double lsdLev = 0, vph = 0, dlev = 0;
    int lsdBit = 0, dBit = 0;
    const double lsdA = 1 - std::exp(-2 * pi * 120.0 / fs), dA = 1 - std::exp(-2 * pi * 0.75 * kBaud / fs);
    for (size_t i = 0; i < nv; i++) {
        if (i % (size_t)(fs / 150.0) == 0) lsdBit = (int)rnd();   // the low-speed data, 150 bit/s
        lsdLev += lsdA * ((lsdBit ? 1.0 : -1.0) - lsdLev);
        double f = 400.0 * lsdLev;
        if (provoice) {
            if (i % (size_t)std::llround(fs / kBaud) == 0) dBit = (int)rnd();
            dlev += dA * ((dBit ? 1.0 : -1.0) - dlev);
            f = 2400.0 * dlev;
        } else {
            f += 2500.0 * std::sin(vph);
            vph += 2 * pi * toneHz / fs;
        }
        fm.push_back((float)f);
    }
    std::vector<cf32> out;
    out.reserve(fm.size() + (size_t)(dropBits / kBaud * fs) + 1);
    double phase = 0;
    for (float f : fm) {
        phase += 2 * pi * (f + offsetHz) / fs;
        if (phase > pi) phase -= 2 * pi; else if (phase < -pi) phase += 2 * pi;
        out.push_back(cf32(amp * (float)std::cos(phase), amp * (float)std::sin(phase)));
    }
    if (dropBits > 0) {   // the end of transmission: dotting (dsd-fme DOTTING_SEQUENCE_A / B)
        std::vector<uint8_t> bits;
        for (int k = 0; k < dropBits; k++) bits.push_back((uint8_t)(k & 1));
        const auto d = modulate(bits, fs, offsetHz, 2400.0, amp);
        out.insert(out.end(), d.begin(), d.end());
    }
    return out;
}

} // namespace dect2::edacs
