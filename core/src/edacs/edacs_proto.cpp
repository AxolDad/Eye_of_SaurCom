// EDACS frame, BCH(40,28), majority vote, messages and test signals (see edacs_proto.h for the sources and what is not verified).
#include "dect2/edacs/edacs_proto.h"
#include <cmath>
#include <cstdio>
#include <map>

namespace dect2::edacs {

// ------------------------------------------------------------------------------------------------ BCH

uint32_t bchCheck(uint32_t data28) {
    // remainder of data(x) * x^12 divided by g(x)
    uint64_t r = (uint64_t)(data28 & 0xFFFFFFF) << 12;
    for (int i = 39; i >= 12; i--) if (r >> i & 1) r ^= (uint64_t)kBchGen << (i - 12);
    return (uint32_t)(r & 0xFFF);
}

uint64_t bchEncode(uint32_t data28) { return ((uint64_t)(data28 & 0xFFFFFFF) << 12) | bchCheck(data28); }

namespace {
uint32_t syndrome(uint64_t w) {   // the remainder of the received word: 0 for a codeword
    for (int i = 39; i >= 12; i--) if (w >> i & 1) w ^= (uint64_t)kBchGen << (i - 12);
    return (uint32_t)(w & 0xFFF);
}
// every pattern of one or two errors by its syndrome (the code's distance is 5, so they are all different)
const std::map<uint32_t, uint64_t>& errorTable() {
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
        put(b, ~w & 0xFFFFFFFFFFull, 40);   // the middle copy is inverted
        put(b, w, 40);
    }
    return b;
}

uint64_t vote(const uint8_t* b, int& dis) {
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
    d.ok = d.bchOk || d.disagree == 0;
    return d;
}

// ------------------------------------------------------------------------------------------------ messages

Message parse(uint32_t v, const Commands& c) {
    Message m;
    m.raw = v & 0xFFFFFFF;
    m.cmd = (int)(m.raw >> 20) & 0xFF;
    if (m.cmd == c.groupVoice || m.cmd == c.groupUpdate) {
        m.kind = Message::GroupVoice;
        m.update = m.cmd == c.groupUpdate;
        m.lcn = (int)(m.raw >> 15) & 31;
        m.digital = (m.raw >> 14) & 1;
        m.emergency = (m.raw >> 13) & 1;
        m.group = (int)(m.raw & 0x7FF);
    } else if (m.cmd == c.indivVoice) {
        m.kind = Message::IndivVoice;
        m.lcn = (int)(m.raw >> 15) & 31;
        m.digital = (m.raw >> 14) & 1;
        m.lid = (int)(m.raw & 0x3FFF);
    } else if (m.cmd == c.caller) {
        m.kind = Message::Caller;
        m.lid = (int)(m.raw & 0x3FFF);
    } else if (m.cmd == c.systemId) {
        m.kind = Message::SystemId;
        m.site = (int)(m.raw >> 16) & 15;
        m.sysId = (int)(m.raw & 0xFFFF);
    } else if (m.cmd == c.idle) {
        m.kind = Message::Idle;
    }
    return m;
}

uint32_t build(const Message& m, const Commands& c) {
    switch (m.kind) {
    case Message::GroupVoice:
        return (uint32_t)(m.update ? c.groupUpdate : c.groupVoice) << 20 | (uint32_t)(m.lcn & 31) << 15 | (uint32_t)m.digital << 14 | (uint32_t)m.emergency << 13 | (uint32_t)(m.group & 0x7FF);
    case Message::IndivVoice:
        return (uint32_t)c.indivVoice << 20 | (uint32_t)(m.lcn & 31) << 15 | (uint32_t)m.digital << 14 | (uint32_t)(m.lid & 0x3FFF);
    case Message::Caller: return (uint32_t)c.caller << 20 | (uint32_t)(m.lid & 0x3FFF);
    case Message::SystemId: return (uint32_t)c.systemId << 20 | (uint32_t)(m.site & 15) << 16 | (uint32_t)(m.sysId & 0xFFFF);
    case Message::Idle: return (uint32_t)c.idle << 20;
    default: return m.raw & 0xFFFFFFF;
    }
}

std::string afsText(int g, const AfsSplit& s) {
    if (g < 0) return "-";
    const int sb = s.subfleetBits, fb = s.fleetBits;
    char b[32];
    snprintf(b, sizeof b, "%02d-%02d%d", g >> (fb + sb), (g >> sb) & ((1 << fb) - 1), g & ((1 << sb) - 1));
    return b;
}

std::string describe(const Message& m) {
    char b[160];
    switch (m.kind) {
    case Message::GroupVoice:
        snprintf(b, sizeof b, "%s group %d (%s) on LCN %d%s%s", m.update ? "group call update:" : "group call:", m.group, afsText(m.group).c_str(), m.lcn,
                 m.digital ? ", ProVoice" : "", m.emergency ? ", EMERGENCY" : "");
        return b;
    case Message::IndivVoice: snprintf(b, sizeof b, "individual call to LID %d on LCN %d%s", m.lid, m.lcn, m.digital ? ", ProVoice" : ""); return b;
    case Message::Caller: snprintf(b, sizeof b, "caller LID %d", m.lid); return b;
    case Message::SystemId: snprintf(b, sizeof b, "system ID %04X, site %d", m.sysId, m.site); return b;
    case Message::Idle: return "idle";
    default: snprintf(b, sizeof b, "command %02X, data %07X", m.cmd, m.raw); return b;
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

std::vector<cf32> workingChannel(double fs, double offsetHz, double voiceSec, double toneHz, int dropFrames, float amp, bool provoice, uint32_t seed) {
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
    out.reserve(fm.size() + (size_t)(dropFrames * kFrameBits / kBaud * fs) + 1);
    double phase = 0;
    for (float f : fm) {
        phase += 2 * pi * (f + offsetHz) / fs;
        if (phase > pi) phase -= 2 * pi; else if (phase < -pi) phase += 2 * pi;
        out.push_back(cf32(amp * (float)std::cos(phase), amp * (float)std::sin(phase)));
    }
    if (dropFrames > 0) {
        std::vector<uint8_t> bits;
        Message idle; idle.kind = Message::Idle;
        for (int k = 0; k < dropFrames; k++) { auto f = encodeFrame(build(idle), build(idle)); bits.insert(bits.end(), f.begin(), f.end()); }
        const auto d = modulate(bits, fs, offsetHz, 2400.0, amp);
        out.insert(out.end(), d.begin(), d.end());
    }
    return out;
}

} // namespace dect2::edacs
