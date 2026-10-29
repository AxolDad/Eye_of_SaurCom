// P25 Phase 2 TDMA outbound: codes, scrambler, MAC PDUs, bursts, the frame layer and the call tracker (see p25p2.h for the sources and for
// which bit positions are this implementation's own choices).
#include "dect2/p25/p25p2.h"
#include "dect2/p25/p25_proto.h"
#include <algorithm>
#include <cstring>

namespace dect2 {
namespace p25p2 {

const char* duidName(int d) {
    switch (d) {
    case k4V: return "4V";
    case k2V: return "2V";
    case kFacch: return "FACCH";
    case kFacchClear: return "FACCH (unscrambled)";
    case kSacch: return "SACCH";
    case kSacchClear: return "SACCH (unscrambled)";
    default: return "?";
    }
}
bool duidScrambled(int d) { return d == k4V || d == k2V || d == kFacch || d == kSacch; }

const char* macName(int op) {
    switch (op) {
    case kMacSignal: return "MAC_SIGNAL";
    case kMacPtt: return "MAC_PTT";
    case kMacEndPtt: return "MAC_END_PTT";
    case kMacIdle: return "MAC_IDLE";
    case kMacActive: return "MAC_ACTIVE";
    case kMacHangtime: return "MAC_HANGTIME";
    default: return "MAC (reserved)";
    }
}

// ------------------------------------------------------------------------------------------------ codes

uint8_t duidEncode(int d) {
    const int d1 = (d >> 3) & 1, d2 = (d >> 2) & 1, d3 = (d >> 1) & 1, d4 = d & 1;
    const int p1 = d1 ^ d2 ^ d4, p2 = d1 ^ d3 ^ d4, p3 = d2 ^ d3 ^ d4, p4 = d1 ^ d2 ^ d3 ^ d4 ^ p1 ^ p2 ^ p3;
    return (uint8_t)((d & 15) << 4 | p1 << 3 | p2 << 2 | p3 << 1 | p4);
}

int duidDecode(uint8_t v, int* errs) {
    int best = -1, bd = 99;
    for (int d = 0; d < 16; d++) {
        const int dist = __builtin_popcount((unsigned)(duidEncode(d) ^ v));
        if (dist < bd) { bd = dist; best = d; }
    }
    if (errs) *errs = bd;
    return bd <= 1 ? best : -1;
}

namespace {
uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
struct IschCode {
    uint64_t rows[9];
    uint64_t cw[512];
    IschCode() {
        uint64_t s = 0x1C5C2D2Aull;   // fixed: the code is this implementation's own (40,9)
        for (auto& r : rows) r = splitmix(s) & 0xFFFFFFFFFFull;
        for (int i = 0; i < 512; i++) {
            uint64_t v = 0;
            for (int b = 0; b < 9; b++) if (i >> (8 - b) & 1) v ^= rows[b];
            cw[i] = v;
        }
    }
};
const IschCode& ischCode() { static const IschCode c; return c; }
} // namespace

uint64_t ischEncode(int info) { return ischCode().cw[info & 511]; }

int ischDecode(uint64_t cw, int* dist) {
    const IschCode& c = ischCode();
    int best = -1, bd = 99;
    for (int i = 0; i < 512; i++) {
        const int d = __builtin_popcountll((c.cw[i] ^ cw) & 0xFFFFFFFFFFull);
        if (d < bd) { bd = d; best = i; }
    }
    if (dist) *dist = bd;
    return bd <= 8 ? best : -1;
}

uint16_t crc12(const uint8_t* bits, int n) {   // x^12 + x^11 + x^3 + x^2 + x + 1, register preset to ones, result complemented
    uint16_t r = 0xFFF;
    for (int i = 0; i < n; i++) {
        const int fb = ((r >> 11) & 1) ^ (bits[i] & 1);
        r = (uint16_t)((r << 1) & 0xFFF);
        if (fb) r ^= 0x80F;
    }
    return (uint16_t)(~r & 0xFFF);
}

uint64_t scramblerSeed(int wacn, int sysId, int nac) {
    const uint64_t s = ((uint64_t)(wacn & 0xFFFFF) << 24) | ((uint64_t)(sysId & 0xFFF) << 12) | (uint64_t)(nac & 0xFFF);
    return s ? s : 0xFFFFFFFFFFFull;
}

void Scrambler::seed(int wacn, int sysId, int nac) {
    seed_ = scramblerSeed(wacn, sysId, nac);
    // x^44 + x^40 + x^35 + x^34 + x^33 + x^31 + x^29 + x^27 + x^26 + x^23 + x^21 + x^20 + x^19 + x^16 + x^13 + x^12 + x^10 + x^8 + x^6 + x^5 + x^2 + 1
    // as the recurrence s[n + 44] = XOR of s[n + t] over the polynomial's lower terms t; s[0..43] is the seed, most significant bit first
    static const int taps[] = {40, 35, 34, 33, 31, 29, 27, 26, 23, 21, 20, 19, 16, 13, 12, 10, 8, 6, 5, 2, 0};
    std::vector<uint8_t> s(pn_.size() + 44);
    for (int i = 0; i < 44; i++) s[(size_t)i] = (uint8_t)((seed_ >> (43 - i)) & 1);
    for (size_t n = 0; n + 44 < s.size(); n++) {
        uint8_t fb = 0;
        for (int t : taps) fb ^= s[n + (size_t)t];
        s[n + 44] = fb;
    }
    for (size_t i = 0; i < pn_.size(); i++) pn_[i] = s[i];
}

// ------------------------------------------------------------------------------------------------ MAC PDUs

namespace {
void put24(std::vector<uint8_t>& v, int x) { v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
void put16(std::vector<uint8_t>& v, int x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
int get24(const uint8_t* p) { return p[0] << 16 | p[1] << 8 | p[2]; }
int get16(const uint8_t* p) { return p[0] << 8 | p[1]; }
}

std::vector<uint8_t> macPtt(int src, int tg, int algId, int keyId, const uint8_t* mi) {
    std::vector<uint8_t> v;
    v.push_back((uint8_t)(kMacPtt << 5));
    for (int i = 0; i < 9; i++) v.push_back(mi ? mi[i] : 0);
    v.push_back((uint8_t)algId);
    put16(v, keyId);
    put24(v, src);
    put16(v, tg);
    return v;   // 18 bytes
}

std::vector<uint8_t> macEndPtt(int src, int tg, int cc) {
    std::vector<uint8_t> v;
    v.push_back((uint8_t)(kMacEndPtt << 5));
    put16(v, (cc & 0xFFF) << 4);
    put24(v, src);
    put16(v, tg);
    return v;
}

std::vector<uint8_t> macWithUser(int opcode, int tg, int src, int svc, bool group) {
    std::vector<uint8_t> v;
    v.push_back((uint8_t)((opcode & 7) << 5));
    v.push_back(group ? 0x01 : 0x02);   // group voice channel user / unit to unit voice channel user
    v.push_back((uint8_t)svc);
    if (group) put16(v, tg); else put24(v, tg);
    put24(v, src);
    return v;
}

std::vector<uint8_t> macIdle() { return {(uint8_t)(kMacIdle << 5), 0x00}; }

bool macParse(const uint8_t* b, int n, MacPdu& m) {
    m = MacPdu();
    if (n < 1) return false;
    m.opcode = b[0] >> 5;
    m.offset = (b[0] >> 2) & 7;
    switch (m.opcode) {
    case kMacPtt:
        if (n < 18) return false;
        memcpy(m.mi, b + 1, 9);
        m.algId = b[10];
        m.keyId = get16(b + 11);
        m.src = get24(b + 13);
        m.tg = get16(b + 16);
        return true;
    case kMacEndPtt:
        if (n < 8) return false;
        m.colorCode = get16(b + 1) >> 4;
        m.src = get24(b + 3);
        m.tg = get16(b + 6);
        return true;
    case kMacActive: case kMacIdle: case kMacHangtime:
        if (n >= 2 && b[1] != 0) {
            m.msgOpcode = b[1];
            if (b[1] == 0x01 && n >= 8) { m.svcOpts = b[2]; m.tg = get16(b + 3); m.src = get24(b + 5); m.group = true; }
            else if (b[1] == 0x02 && n >= 9) { m.svcOpts = b[2]; m.tg = get24(b + 3); m.src = get24(b + 6); m.group = false; }
        }
        return true;
    default: return true;
    }
}

// ------------------------------------------------------------------------------------------------ bursts

namespace {
// the 156 payload dibits are the burst's dibits without the DUID positions, in order
void payloadIndex(int idx[156]) {
    int k = 0;
    for (int i = 0; i < kBurstDibits; i++) {
        bool d = false;
        for (int p : kDuidPos) d |= p == i;
        if (!d) idx[k++] = i;
    }
}
const int* payloadIdx() {
    static int idx[156];
    static bool done = false;
    if (!done) { payloadIndex(idx); done = true; }
    return idx;
}
void toHex(const uint8_t* bits, int nHex, uint8_t* hex) {
    for (int i = 0; i < nHex; i++) { int v = 0; for (int b = 0; b < 6; b++) v = v << 1 | (bits[6 * i + b] & 1); hex[i] = (uint8_t)v; }
}
void fromHex(const uint8_t* hex, int nHex, uint8_t* bits) {
    for (int i = 0; i < nHex; i++) for (int b = 0; b < 6; b++) bits[6 * i + b] = (uint8_t)((hex[i] >> (5 - b)) & 1);
}
// MAC bytes (nBytes, padded) + CRC-12 -> RS(n, k) codeword bits (6n)
void macEncode(const std::vector<uint8_t>& mac, int macBits, int n, int k, uint8_t* out) {
    uint8_t info[180] = {};
    for (int i = 0; i < macBits; i++) { const size_t by = (size_t)i / 8; info[i] = by < mac.size() ? (uint8_t)((mac[by] >> (7 - i % 8)) & 1) : 0; }
    const uint16_t c = crc12(info, macBits);
    for (int i = 0; i < 12; i++) info[macBits + i] = (uint8_t)((c >> (11 - i)) & 1);
    uint8_t hex[63] = {};
    toHex(info, k, hex);
    p25::rsEncode(hex, n, k);
    fromHex(hex, n, out);
}
bool macDecode(const uint8_t* in, int macBits, int n, int k, MacPdu& m) {
    uint8_t hex[63] = {};
    toHex(in, n, hex);
    if (!p25::rsDecode(hex, n, k)) return false;
    uint8_t info[180] = {};
    fromHex(hex, k, info);
    uint16_t c = 0;
    for (int i = 0; i < 12; i++) c = (uint16_t)(c << 1 | info[macBits + i]);
    if (crc12(info, macBits) != c) return false;
    uint8_t by[21] = {};
    for (int i = 0; i < macBits; i++) by[i / 8] = (uint8_t)(by[i / 8] | info[i] << (7 - i % 8));
    return macParse(by, macBits / 8, m);
}
} // namespace

void burstBuild(int duid, const uint8_t (*voice)[72], const uint8_t* ess, const std::vector<uint8_t>& mac, const uint8_t* scr, uint8_t dib[kBurstDibits]) {
    uint8_t p[kPayloadBits] = {};
    switch (duid) {
    case k4V:
        for (int f = 0; f < 4; f++) for (int i = 0; i < 72; i++) p[(f < 2 ? 72 * f : 168 + 72 * (f - 2)) + i] = voice[f][i] & 1;
        for (int i = 0; i < 24; i++) p[144 + i] = ess ? ess[i] & 1 : 0;
        break;
    case k2V:
        for (int i = 0; i < 72; i++) { p[i] = voice[0][i] & 1; p[240 + i] = voice[1][i] & 1; }
        for (int i = 0; i < 168; i++) p[72 + i] = ess ? ess[i] & 1 : 0;
        break;
    case kFacch: case kFacchClear: macEncode(mac, 144, 45, 26, p); break;   // 270 bits; the last 42 stay zero
    case kSacch: case kSacchClear: macEncode(mac, 168, 52, 30, p); break;   // 312 bits
    default: break;
    }
    if (scr && duidScrambled(duid)) for (int i = 0; i < kPayloadBits; i++) p[i] ^= scr[i];
    const uint8_t dv = duidEncode(duid);
    for (int j = 0; j < 4; j++) dib[kDuidPos[j]] = (uint8_t)((dv >> (6 - 2 * j)) & 3);
    const int* idx = payloadIdx();
    for (int i = 0; i < 156; i++) dib[idx[i]] = (uint8_t)(p[2 * i] << 1 | p[2 * i + 1]);
}

void burstParse(const uint8_t dib[kBurstDibits], const uint8_t* scr, Burst& b) {
    b = Burst();
    uint8_t dv = 0;
    for (int j = 0; j < 4; j++) dv = (uint8_t)(dv << 2 | (dib[kDuidPos[j]] & 3));
    b.duid = duidDecode(dv, &b.duidErrs);
    if (b.duid < 0) return;
    uint8_t p[kPayloadBits];
    const int* idx = payloadIdx();
    for (int i = 0; i < 156; i++) { p[2 * i] = (uint8_t)((dib[idx[i]] >> 1) & 1); p[2 * i + 1] = (uint8_t)(dib[idx[i]] & 1); }
    if (scr && duidScrambled(b.duid)) for (int i = 0; i < kPayloadBits; i++) p[i] ^= scr[i];
    switch (b.duid) {
    case k4V:
        b.nVoice = 4;
        for (int f = 0; f < 4; f++) for (int i = 0; i < 72; i++) b.voice[f][i] = p[(f < 2 ? 72 * f : 168 + 72 * (f - 2)) + i];
        for (int i = 0; i < 24; i++) b.ess[i] = p[144 + i];
        break;
    case k2V:
        b.nVoice = 2;
        for (int i = 0; i < 72; i++) { b.voice[0][i] = p[i]; b.voice[1][i] = p[240 + i]; }
        for (int i = 0; i < 168; i++) b.ess[i] = p[72 + i];
        break;
    case kFacch: case kFacchClear: b.macPresent = true; b.macOk = macDecode(p, 144, 45, 26, b.mac); break;
    case kSacch: case kSacchClear: b.macPresent = true; b.macOk = macDecode(p, 168, 52, 30, b.mac); break;
    default: break;
    }
}

void essBuild(int algId, int keyId, const uint8_t mi[9], uint8_t essB[4][24], uint8_t essA[168]) {
    uint8_t info[96];
    for (int i = 0; i < 8; i++) info[i] = (uint8_t)((algId >> (7 - i)) & 1);
    for (int i = 0; i < 16; i++) info[8 + i] = (uint8_t)((keyId >> (15 - i)) & 1);
    for (int i = 0; i < 72; i++) info[24 + i] = (uint8_t)(((mi ? mi[i / 8] : 0) >> (7 - i % 8)) & 1);
    uint8_t hex[44] = {};
    toHex(info, 16, hex);
    p25::rsEncode(hex, 44, 16);
    uint8_t bits[264];
    fromHex(hex, 44, bits);
    for (int f = 0; f < 4; f++) for (int i = 0; i < 24; i++) essB[f][i] = bits[24 * f + i];
    for (int i = 0; i < 168; i++) essA[i] = bits[96 + i];
}

bool essParse(const uint8_t essB[4][24], const uint8_t essA[168], int& algId, int& keyId, uint8_t mi[9]) {
    uint8_t bits[264];
    for (int f = 0; f < 4; f++) for (int i = 0; i < 24; i++) bits[24 * f + i] = essB[f][i];
    for (int i = 0; i < 168; i++) bits[96 + i] = essA[i];
    uint8_t hex[44];
    toHex(bits, 44, hex);
    if (!p25::rsDecode(hex, 44, 16)) return false;
    uint8_t info[96];
    fromHex(hex, 16, info);
    algId = 0; keyId = 0;
    for (int i = 0; i < 8; i++) algId = algId << 1 | info[i];
    for (int i = 0; i < 16; i++) keyId = keyId << 1 | info[8 + i];
    if (mi) { memset(mi, 0, 9); for (int i = 0; i < 72; i++) mi[i / 8] = (uint8_t)(mi[i / 8] | info[24 + i] << (7 - i % 8)); }
    return true;
}

// ------------------------------------------------------------------------------------------------ frame layer

void Framer::reset() {
    reg_ = 0; locked_ = false; pos_ = 0; slot_ = 0; misses_ = 0;
    essBn_[0] = essBn_[1] = 0;
    out_.clear();
}

void Framer::push(int d) {
    reg_ = ((reg_ << 2) | (uint64_t)(d & 3)) & 0xFFFFFFFFFFull;
    const int syncDist = __builtin_popcountll(reg_ ^ kSisch);
    if (!locked_) {
        if (syncDist <= 4) {   // the S-ISCH just ended: timeslot 0's burst follows
            locked_ = true; slot_ = 0; pos_ = kIschDibits; misses_ = 0;
            essBn_[0] = essBn_[1] = 0;
            Event e; e.kind = Event::Lock; out_.push_back(e);
        }
        return;
    }
    buf_[pos_++] = (uint8_t)(d & 3);
    if (syncDist <= 2 && !(slot_ == 0 && pos_ == kIschDibits)) {
        // an S-ISCH where the clock did not expect it: the symbol clock slipped. Timeslot 0 starts here.
        slot_ = 0; pos_ = kIschDibits; misses_ = 0;
        essBn_[0] = essBn_[1] = 0;
        return;
    }
    if (pos_ == kIschDibits) {   // the ISCH of this timeslot is in
        if (slot_ == 0) {
            if (syncDist <= 6) misses_ = 0;
            else if (++misses_ >= 2) {
                locked_ = false; pos_ = 0;
                Event e; e.kind = Event::Lost; out_.push_back(e);
                return;
            }
        } else {
            int dist = 0;
            const int info = ischDecode(reg_, &dist);
            if (info >= 0 && ischSlot(info) < kSlotsPerSuperframe && ischSlot(info) != slot_ && dist <= 4) slot_ = ischSlot(info);   // a clear I-ISCH says where we are
        }
    }
    if (pos_ == kSlotDibits) {
        slotDone();
        pos_ = 0;
        slot_ = (slot_ + 1) % kSlotsPerSuperframe;
        if (slot_ == 0) sf_++;
    }
}

void Framer::slotDone() {
    Burst b;
    burstParse(buf_ + kIschDibits, scr_.bits(slot_), b);
    bursts_++;
    const int c = chanOfSlot(slot_);
    if (b.duid < 0) return;
    if (b.macPresent) {
        (b.macOk ? macOk_ : macBad_)++;
        if (b.macOk) { Event e; e.kind = Event::Mac; e.chan = c; e.slot = slot_; e.mac = b.mac; e.macOk = true; out_.push_back(e); }
        return;
    }
    if (b.nVoice) {
        Event e; e.kind = Event::Voice; e.chan = c; e.slot = slot_; e.nVoice = b.nVoice;
        memcpy(e.voice, b.voice, sizeof e.voice);
        out_.push_back(e);
        if (b.duid == k4V) {
            if (essBn_[c] < 4) memcpy(essB_[c][essBn_[c]++], b.ess, 24);
        } else {   // 2V: the superframe's encryption sync is complete
            if (essBn_[c] == 4) {
                Event s; s.kind = Event::Ess; s.chan = c; s.slot = slot_;
                uint8_t mi[9];
                if (essParse(essB_[c], b.ess, s.algId, s.keyId, mi)) out_.push_back(s);
            }
            essBn_[c] = 0;
        }
    }
}

bool CallTracker::onEvent(const Event& e) {
    Call& c = c_[e.chan & 1];
    switch (e.kind) {
    case Event::Mac: {
        const MacPdu& m = e.mac;
        if (m.opcode == kMacPtt) {
            c = Call();
            c.active = true; c.src = m.src; c.tg = m.tg; c.algId = m.algId; c.keyId = m.keyId;
            c.encrypted = m.algId != kAlgClear && m.algId != 0;
            c.cryptoKnown = true;
            return true;
        }
        if (m.opcode == kMacEndPtt) {
            if (!c.active) return false;
            c.active = false; c.endSeen = true;
            if (m.src > 0) c.src = m.src;
            return true;
        }
        if (m.opcode == kMacActive && m.msgOpcode >= 0 && m.tg >= 0) {   // joined late, or the ids / service options of the call running
            const bool start = !c.active;
            if (start) { c = Call(); c.active = true; c.lateEntry = true; }
            c.tg = m.tg; if (m.src > 0) c.src = m.src; c.group = m.group;
            c.emergency = (m.svcOpts & 0x80) != 0;
            if (m.svcOpts & 0x40) c.encrypted = true;
            c.cryptoKnown = true;
            return start;
        }
        if ((m.opcode == kMacIdle || m.opcode == kMacHangtime) && c.active) {   // the channel went idle (or into hang time) without the END_PTT heard
            c.active = false; c.endSeen = true;
            return true;
        }
        return false;
    }
    case Event::Ess:
        if (!c.active) return false;
        c.algId = e.algId; c.keyId = e.keyId;
        c.cryptoKnown = true;
        if (e.algId != kAlgClear && e.algId != 0 && !c.encrypted) { c.encrypted = true; return true; }
        return false;
    case Event::Voice:
        if (!c.active) { c = Call(); c.active = true; c.lateEntry = true; c.frames = (uint32_t)e.nVoice; return true; }   // voice before any MAC PDU: late entry
        c.frames += (uint32_t)e.nVoice;
        return false;
    case Event::Lost:
        for (Call& x : c_) if (x.active) x.active = false;
        return true;
    default: return false;
    }
}

// ------------------------------------------------------------------------------------------------ generator

Generator::Generator(int wacn, int sysId, int nac) : nac_(nac) { scr_.seed(wacn, sysId, nac); }

void Generator::voiceFrame(int chan, uint32_t n, uint8_t bits[72]) {
    uint64_t s = 0xA5A5ull * (uint64_t)(chan + 1) + (uint64_t)n * 0x10001ull;
    const uint64_t a = splitmix(s), b = splitmix(s);
    for (int i = 0; i < 72; i++) bits[i] = (uint8_t)((i < 64 ? a >> i : b >> (i - 64)) & 1);
}

const GenCall* Generator::active(int chan, int s, int& phase) const {
    for (const GenCall& c : calls_) {
        if (c.chan != chan) continue;
        if (s == c.startSf - 1) { phase = 0; return &c; }
        if (s >= c.startSf && s < c.startSf + c.superframes) { phase = 1; return &c; }
        if (s == c.startSf + c.superframes) { phase = 2; return &c; }
    }
    return nullptr;
}

std::vector<uint8_t> Generator::superframe(int s) {
    std::vector<uint8_t> out;
    out.reserve((size_t)kSlotsPerSuperframe * kSlotDibits);
    uint8_t essB[2][4][24] = {}, essA[2][168] = {};
    int phase[2] = {-1, -1};
    const GenCall* call[2];
    for (int c = 0; c < 2; c++) {
        call[c] = active(c, s, phase[c]);
        if (call[c]) {
            uint8_t mi[9];
            for (int i = 0; i < 9; i++) mi[i] = (uint8_t)(s * 7 + i);
            essBuild(call[c]->algId, call[c]->keyId, mi, essB[c], essA[c]);
        }
    }
    int nth[2] = {0, 0};
    for (int k = 0; k < kSlotsPerSuperframe; k++) {
        const int c = chanOfSlot(k);
        const uint64_t isch = k == 0 ? kSisch : ischEncode(ischInfo(k, c, s & 3));
        for (int i = 0; i < kIschDibits; i++) out.push_back((uint8_t)((isch >> (38 - 2 * i)) & 3));
        uint8_t dib[kBurstDibits];
        const GenCall* g = call[c];
        std::vector<uint8_t> mac;
        int duid = kFacch;
        uint8_t v[4][72] = {};
        const uint8_t* ess = nullptr;
        if (sacchSlot(k)) {
            duid = kSacch;
            mac = g && phase[c] <= 1 ? macWithUser(kMacActive, g->tg, g->src, g->algId != kAlgClear ? 0x40 : 0) : macIdle();   // the PTT went out in slot 8 / 9
        } else if (g && phase[c] == 1) {
            const int j = nth[c]++;
            duid = j < 4 ? k4V : k2V;
            const int nv = j < 4 ? 4 : 2;
            for (int f = 0; f < nv; f++) voiceFrame(c, vn_[c]++, v[f]);
            ess = j < 4 ? essB[c][j] : essA[c];
        } else if (g && phase[c] == 0) {
            mac = nth[c]++ == 4 ? macPtt(g->src, g->tg, g->algId, g->keyId) : macIdle();   // the PTT just before the voice starts
        } else if (g && phase[c] == 2) {
            mac = nth[c]++ == 0 ? macEndPtt(g->src, g->tg, nac_) : macIdle();
        } else {
            mac = macIdle();
        }
        burstBuild(duid, v, ess, mac, scr_.bits(k), dib);
        out.insert(out.end(), dib, dib + kBurstDibits);
    }
    return out;
}

} // namespace p25p2
} // namespace dect2
