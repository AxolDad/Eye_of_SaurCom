// P25 Phase 2 TDMA outbound: codes, scrambler, MAC PDUs, bursts, the frame layer and the call tracker (p25p2.h names the sources).
// The tables and bit positions here are derived from OP25 (GPL-3, boatbod/op25, op25/gr-op25_repeater) and SDRTrunk (GPL-3, DSheirer/sdrtrunk);
// each one names its file. This file is part of Eye of SaurCom, GPL-3.
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
    case kLcch: return "LCCH";
    case kLcchClear: return "LCCH (unscrambled)";
    default: return "?";
    }
}
bool duidScrambled(int d) { return d == k4V || d == k2V || d == kFacch || d == kSacch || d == kLcch; }

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

// ------------------------------------------------------------------------------------------------ DUID

namespace {
// SDRTrunk enumeration/DataUnitID.java: the codeword of each DUID value
const uint8_t kDuidCw[16] = {0x00, 0x17, 0x2E, 0x39, 0x4B, 0x5C, 0x65, 0x72, 0x8D, 0x9A, 0xA3, 0xB4, 0xC6, 0xD1, 0xE8, 0xFF};
// OP25 lib/p25p2_duid.cc _duid_lookup: received codeword -> DUID value (-1: cannot tell)
const int8_t kDuidLookup[256] = {
     0,  0,  0, -1,  0, -1, -1,  1,  0, -1, -1,  4, -1,  8,  2, -1,
     0, -1, -1,  1, -1,  1,  1,  1, -1,  3,  9, -1,  5, -1, -1,  1,
     0, -1, -1, 10, -1,  6,  2, -1, -1,  3,  2, -1,  2, -1,  2,  2,
    -1,  3,  7, -1, 11, -1, -1,  1,  3,  3, -1,  3, -1,  3,  2, -1,
     0, -1, -1,  4, -1,  6, 12, -1, -1,  4,  4,  4,  5, -1, -1,  4,
    -1, 13,  7, -1,  5, -1, -1,  1,  5, -1, -1,  4,  5,  5,  5, -1,
    -1,  6,  7, -1,  6,  6, -1,  6, 14, -1, -1,  4, -1,  6,  2, -1,
     7, -1,  7,  7, -1,  6,  7, -1, -1,  3,  7, -1,  5, -1, -1, 15,
     0, -1, -1, 10, -1,  8, 12, -1, -1,  8,  9, -1,  8,  8, -1,  8,
    -1, 13,  9, -1, 11, -1, -1,  1,  9, -1,  9,  9, -1,  8,  9, -1,
    -1, 10, 10, 10, 11, -1, -1, 10, 14, -1, -1, 10, -1,  8,  2, -1,
    11, -1, -1, 10, 11, 11, 11, -1, -1,  3,  9, -1, 11, -1, -1, 15,
    -1, 13, 12, -1, 12, -1, 12, 12, 14, -1, -1,  4, -1,  8, 12, -1,
    13, 13, -1, 13, -1, 13, 12, -1, -1, 13,  9, -1,  5, -1, -1, 15,
    14, -1, -1, 10, -1,  6, 12, -1, 14, 14, 14, -1, 14, -1, -1, 15,
    -1, 13,  7, -1, 11, -1, -1, 15, 14, -1, -1, 15, -1, 15, 15, 15,
};
// OP25 lib/p25p2_isch.cc: the I-ISCH codeword of each 7-bit information value
const uint64_t kIsch[128] = {
    0x184229D461ull, 0x18761451F6ull, 0x181AE27E2Full, 0x182EDFFBB8ull,
    0x18DF8A7510ull, 0x18EBB7F087ull, 0x188741DF5Eull, 0x18B37C5AC9ull,
    0x1146A44F13ull, 0x117299CA84ull, 0x111E6FE55Dull, 0x112A5260CAull,
    0x11DB07EE62ull, 0x11EF3A6BF5ull, 0x1183CC442Cull, 0x11B7F1C1BBull,
    0x1A4A2E239Eull, 0x1A7E13A609ull, 0x1A12E589D0ull, 0x1A26D80C47ull,
    0x1AD78D82EFull, 0x1AE3B00778ull, 0x1A8F4628A1ull, 0x1ABB7BAD36ull,
    0x134EA3B8ECull, 0x137A9E3D7Bull, 0x13166812A2ull, 0x1322559735ull,
    0x13D300199Dull, 0x13E73D9C0Aull, 0x138BCBB3D3ull, 0x13BFF63644ull,
    0x1442F705EFull, 0x1476CA8078ull, 0x141A3CAFA1ull, 0x142E012A36ull,
    0x14DF54A49Eull, 0x14EB692109ull, 0x14879F0ED0ull, 0x14B3A28B47ull,
    0x1D467A9E9Dull, 0x1D72471B0Aull, 0x1D1EB134D3ull, 0x1D2A8CB144ull,
    0x1DDBD93FECull, 0x1DEFE4BA7Bull, 0x1D831295A2ull, 0x1DB72F1035ull,
    0x164AF0F210ull, 0x167ECD7787ull, 0x16123B585Eull, 0x162606DDC9ull,
    0x16D7535361ull, 0x16E36ED6F6ull, 0x168F98F92Full, 0x16BBA57CB8ull,
    0x1F4E7D6962ull, 0x1F7A40ECF5ull, 0x1F16B6C32Cull, 0x1F228B46BBull,
    0x1FD3DEC813ull, 0x1FE7E34D84ull, 0x1F8B15625Dull, 0x1FBF28E7CAull,
    0x084D62C339ull, 0x08795F46AEull, 0x0815A96977ull, 0x082194ECE0ull,
    0x08D0C16248ull, 0x08E4FCE7DFull, 0x08880AC806ull, 0x08BC374D91ull,
    0x0149EF584Bull, 0x017DD2DDDCull, 0x011124F205ull, 0x0125197792ull,
    0x01D44CF93Aull, 0x01E0717CADull, 0x018C875374ull, 0x01B8BAD6E3ull,
    0x0A456534C6ull, 0x0A7158B151ull, 0x0A1DAE9E88ull, 0x0A29931B1Full,
    0x0AD8C695B7ull, 0x0AECFB1020ull, 0x0A800D3FF9ull, 0x0AB430BA6Eull,
    0x0341E8AFB4ull, 0x0375D52A23ull, 0x03192305FAull, 0x032D1E806Dull,
    0x03DC4B0EC5ull, 0x03E8768B52ull, 0x038480A48Bull, 0x03B0BD211Cull,
    0x044DBC12B7ull, 0x0479819720ull, 0x041577B8F9ull, 0x04214A3D6Eull,
    0x04D01FB3C6ull, 0x04E4223651ull, 0x0488D41988ull, 0x04BCE99C1Full,
    0x0D493189C5ull, 0x0D7D0C0C52ull, 0x0D11FA238Bull, 0x0D25C7A61Cull,
    0x0DD49228B4ull, 0x0DE0AFAD23ull, 0x0D8C5982FAull, 0x0DB864076Dull,
    0x0645BBE548ull, 0x06718660DFull, 0x061D704F06ull, 0x06294DCA91ull,
    0x06D8184439ull, 0x06EC25C1AEull, 0x0680D3EE77ull, 0x06B4EE6BE0ull,
    0x0F41367E3Aull, 0x0F750BFBADull, 0x0F19FDD474ull, 0x0F2DC051E3ull,
    0x0FDC95DF4Bull, 0x0FE8A85ADCull, 0x0F845E7505ull, 0x0FB063F092ull,
};
// OP25 lib/p25p2_vf.cc interleave_vcw: the voice frame's bit i (dibit i / 2, first bit of the dibit for even i) is bit j of code word k
const uint8_t kVf[72][2] = {
    {0, 23}, {0,  5}, {1, 10}, {2,  3}, {0, 22}, {0,  4}, {1,  9}, {2,  2},
    {0, 21}, {0,  3}, {1,  8}, {2,  1}, {0, 20}, {0,  2}, {1,  7}, {2,  0},
    {0, 19}, {0,  1}, {1,  6}, {3, 13}, {0, 18}, {0,  0}, {1,  5}, {3, 12},
    {0, 17}, {1, 22}, {1,  4}, {3, 11}, {0, 16}, {1, 21}, {1,  3}, {3, 10},
    {0, 15}, {1, 20}, {1,  2}, {3,  9}, {0, 14}, {1, 19}, {1,  1}, {3,  8},
    {0, 13}, {1, 18}, {1,  0}, {3,  7}, {0, 12}, {1, 17}, {2, 10}, {3,  6},
    {0, 11}, {1, 16}, {2,  9}, {3,  5}, {0, 10}, {1, 15}, {2,  8}, {3,  4},
    {0,  9}, {1, 14}, {2,  7}, {3,  3}, {0,  8}, {1, 13}, {2,  6}, {3,  2},
    {0,  7}, {1, 12}, {2,  5}, {3,  1}, {0,  6}, {1, 11}, {2,  4}, {3,  0},
};
} // namespace

uint8_t duidEncode(int d) { return kDuidCw[d & 15]; }

int duidDecode(uint8_t v, int* errs) {
    const int d = kDuidLookup[v];
    if (errs) *errs = d < 0 ? 8 : __builtin_popcount((unsigned)(kDuidCw[d] ^ v));
    return d;
}

uint64_t ischEncode(int info) { return kIsch[info & 127]; }

int ischDecode(uint64_t cw, int* dist) {   // OP25 p25p2_isch.cc isch_lookup: nearest codeword, at most 7 bits off
    cw &= 0xFFFFFFFFFFull;
    int best = -1, bd = 41;
    for (int i = 0; i < 128; i++) {
        const int d = __builtin_popcountll(kIsch[i] ^ cw);
        if (d < bd) { bd = d; best = i; }
    }
    const int ds = __builtin_popcountll(kSisch ^ cw);
    if (ds < bd) { bd = ds; best = -2; }
    if (dist) *dist = bd;
    return bd <= 7 ? best : -1;
}

uint16_t crc12(const uint8_t* bits, int n) {   // OP25 p25p2_tdma.cc crc12: poly {1,1,0,0,0,1,0,0,1,0,1,1,1}, remainder ^ 0xFFF
    uint16_t r = 0;
    for (int i = 0; i < n + 12; i++) {
        const int in = i < n ? bits[i] & 1 : 0;
        const int top = (r >> 11) & 1;
        r = (uint16_t)(((r << 1) | in) & 0xFFF);
        if (top) r ^= 0x897;
    }
    return (uint16_t)(r ^ 0xFFF);
}

// ------------------------------------------------------------------------------------------------ Reed-Solomon (63,35), errors and erasures
// GF(64) with x^6 + x + 1, generator roots alpha^1 .. alpha^28: the code OP25 decodes with ezpwd::RS<63,35> (POLY 0x43, FCR 1, PRIM 1).
// The decoder is the textbook Berlekamp-Massey with the erasure locator as its start, Chien search and Forney's formula.

namespace {
struct Gf {
    uint8_t exp[128], log[64];
    Gf() {
        int x = 1;
        for (int i = 0; i < 63; i++) { exp[i] = (uint8_t)x; log[x] = (uint8_t)i; x <<= 1; if (x & 64) x ^= 0x43; }
        for (int i = 63; i < 128; i++) exp[i] = exp[i - 63];
        log[0] = 0;
    }
    uint8_t mul(int a, int b) const { return a && b ? exp[log[a] + log[b]] : 0; }
    uint8_t inv(int a) const { return exp[63 - log[a]]; }
    uint8_t pw(int e) const { e %= 63; if (e < 0) e += 63; return exp[e]; }
};
const Gf& gf() { static const Gf g; return g; }
constexpr int kN = 63, kRoots = 28;
}

void rs63Encode(uint8_t sym[63]) { p25::rsEncode(sym, 63, 35); }

bool rs63Decode(uint8_t c[63], const std::vector<int>& eras, int* corrected) {
    const Gf& g = gf();
    if (corrected) *corrected = 0;
    uint8_t s[kRoots];
    bool any = false;
    for (int j = 0; j < kRoots; j++) {   // s[j] = r(alpha^(j+1)), c[0] the coefficient of x^62
        int v = 0;
        for (int t = 0; t < kN; t++) if (c[t] & 63) v ^= g.mul(c[t] & 63, g.pw((j + 1) * (kN - 1 - t)));
        s[j] = (uint8_t)v;
        any |= v != 0;
    }
    if (!any) return true;
    const int ne = (int)eras.size();
    if (ne > kRoots) return false;
    uint8_t lam[kRoots + 1] = {1}, b[kRoots + 1], t[kRoots + 1];
    for (int i = 0; i < ne; i++) {   // the erasure locator: product of (1 + X x), X = alpha^(62 - position)
        const int X = g.pw(kN - 1 - eras[(size_t)i]);
        for (int j = i + 1; j > 0; j--) lam[j] ^= g.mul(X, lam[j - 1]);
    }
    memcpy(b, lam, sizeof b);
    int el = ne;
    for (int r = ne + 1; r <= kRoots; r++) {
        int d = 0;
        for (int i = 0; i < r; i++) d ^= g.mul(lam[i], s[r - i - 1]);
        if (d == 0) { memmove(b + 1, b, kRoots); b[0] = 0; continue; }
        t[0] = lam[0];
        for (int i = 0; i < kRoots; i++) t[i + 1] = lam[i + 1] ^ g.mul(d, b[i]);
        if (2 * el <= r + ne - 1) {
            el = r + ne - el;
            const int di = g.inv(d);
            for (int i = 0; i <= kRoots; i++) b[i] = g.mul(lam[i], di);
        } else { memmove(b + 1, b, kRoots); b[0] = 0; }
        memcpy(lam, t, sizeof lam);
    }
    int deg = 0;
    for (int i = 0; i <= kRoots; i++) if (lam[i]) deg = i;
    if (deg == 0 || 2 * (deg - ne) + ne > kRoots) return false;
    // omega = s(x) lambda(x) mod x^28
    uint8_t om[kRoots] = {};
    for (int i = 0; i < kRoots; i++) for (int j = 0; j <= i && j <= deg; j++) om[i] ^= g.mul(lam[j], s[i - j]);
    int found = 0, pos[kRoots];
    uint8_t val[kRoots];
    for (int tpos = 0; tpos < kN; tpos++) {   // Chien: X = alpha^(62 - tpos) is a root's inverse
        const int Xi = g.pw(-(kN - 1 - tpos));
        int v = 0;
        for (int i = 0; i <= deg; i++) v ^= g.mul(lam[i], g.pw(i * g.log[Xi]));
        if (v) continue;
        if (found == kRoots) return false;
        int num = 0, den = 0;
        for (int i = 0; i < kRoots; i++) num ^= g.mul(om[i], g.pw(i * g.log[Xi]));
        for (int i = 1; i <= deg; i += 2) den ^= g.mul(lam[i], g.pw((i - 1) * g.log[Xi]));   // lambda'(Xi) (odd terms)
        if (!den) return false;
        pos[found] = tpos; val[found] = g.mul(num, g.inv(den));
        found++;
    }
    if (found != deg) return false;
    for (int i = 0; i < found; i++) c[pos[i]] ^= val[i];
    if (corrected) for (int i = 0; i < found; i++) *corrected += val[i] != 0;
    return true;
}

// ------------------------------------------------------------------------------------------------ scrambler (SDRTrunk LinearFeedbackShiftRegister.java)

uint64_t scramblerSeed(int wacn, int sysId, int nac) {
    const uint64_t s = ((uint64_t)(wacn & 0xFFFFF) << 24) | ((uint64_t)(sysId & 0xFFF) << 12) | (uint64_t)(nac & 0xFFF);
    return s ? s : 0xFFFFFFFFFFFull;
}

void Scrambler::seed(int wacn, int sysId, int nac) {
    uint64_t r = scramblerSeed(wacn, sysId, nac);
    for (uint8_t& bit : seq_) {
        const uint64_t out = (r >> 43) & 1;
        const uint64_t fb = out ^ ((r >> 33) & 1) ^ ((r >> 19) & 1) ^ ((r >> 14) & 1) ^ ((r >> 8) & 1) ^ ((r >> 3) & 1);
        r = ((r << 1) & 0xFFFFFFFFFFFull) | fb;
        bit = (uint8_t)out;
    }
}

// ------------------------------------------------------------------------------------------------ MAC PDUs (OP25 p25p2_tdma.cc handle_mac_*)

namespace {
void put24(std::vector<uint8_t>& v, int x) { v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
void put16(std::vector<uint8_t>& v, int x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }
int get24(const uint8_t* p) { return p[0] << 16 | p[1] << 8 | p[2]; }
int get16(const uint8_t* p) { return p[0] << 8 | p[1]; }
}

std::vector<uint8_t> macPtt(int src, int tg, int algId, int keyId, const uint8_t* mi) {
    std::vector<uint8_t> v;
    v.push_back((uint8_t)(kMacPtt << 5));
    for (int i = 0; i < 9; i++) v.push_back(mi ? mi[i] : 0);   // bytes 1..9 MI
    v.push_back((uint8_t)algId);                                // 10 ALGID
    put16(v, keyId);                                            // 11..12 key ID
    put24(v, src);                                              // 13..15 source
    put16(v, tg);                                               // 16..17 group
    return v;
}

std::vector<uint8_t> macEndPtt(int src, int tg, int cc) {
    std::vector<uint8_t> v((size_t)18, 0);
    v[0] = (uint8_t)(kMacEndPtt << 5);
    v[1] = (uint8_t)((cc >> 8) & 0x0F); v[2] = (uint8_t)cc;   // colour code: byte 1 low nibble, byte 2
    v[13] = (uint8_t)(src >> 16); v[14] = (uint8_t)(src >> 8); v[15] = (uint8_t)src;
    v[16] = (uint8_t)(tg >> 8); v[17] = (uint8_t)tg;
    return v;
}

std::vector<uint8_t> macWithUser(int opcode, int tg, int src, int svc, bool group) {
    std::vector<uint8_t> v;
    v.push_back((uint8_t)((opcode & 7) << 5));
    v.push_back(group ? 0x01 : 0x02);   // group / unit to unit voice channel user, abbreviated (lengths 7 and 8 in OP25's mac_msg_len)
    v.push_back((uint8_t)svc);
    if (group) put16(v, tg); else put24(v, tg);
    put24(v, src);
    return v;
}

std::vector<uint8_t> macIdle() { return {(uint8_t)(kMacIdle << 5), 0x00}; }

// MAC message lengths in bytes by opcode (b1b2 << 6 | mco), 0 unknown: OP25 (GPL-3) lib/p25p2_tdma.cc mac_msg_len
const uint8_t kMacMsgLen[256] = {
     0,  7,  8,  7,  0, 16,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0, 14, 15,  0,  0, 15,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     5,  7,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     9,  7,  9,  0,  9,  8,  9,  0, 10, 10,  9,  0, 10,  0,  0,  0,
     0,  0,  0,  0,  9,  7,  0,  0, 10,  0,  7,  0, 10,  8, 14,  7,
     9,  9,  0,  0,  9,  0,  0,  9, 10,  0,  7, 10, 10,  7,  0,  9,
     9, 29,  9,  9,  9,  9, 10, 13,  9,  9,  9, 11,  9,  9,  0,  0,
     8,  0,  0,  7, 11,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    16,  0,  0, 11, 13, 11, 11, 11, 10,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    11,  0,  0,  8, 15, 12, 15, 32, 12, 12,  0, 27, 14, 29, 29, 32,
     0,  0,  0,  0,  0,  0,  9,  0, 14, 29, 11, 27, 14,  0, 40, 11,
    28,  0,  0, 14, 17, 14,  0,  0, 16,  8, 11,  0, 13, 19,  0,  0,
     0,  0, 16, 14,  0,  0, 12,  0, 22,  0, 11, 13, 11,  0, 15,  0,
};

bool macParse(const uint8_t* b, int n, MacPdu& m) {
    m = MacPdu();
    if (n < 1) return false;
    m.opcode = (b[0] >> 5) & 7;
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
        if (n < 18) return false;
        m.colorCode = ((b[1] & 0x0F) << 8) | b[2];
        m.src = get24(b + 13);
        m.tg = get16(b + 16);
        return true;
    case kMacActive: case kMacIdle: case kMacHangtime:
        // the PDU carries a list of MAC messages: walk it as OP25's decode_mac_msg does, taking the voice channel user message wherever it is
        for (int p = 1; p < n;) {
            const int op = b[p], rem = n - p;
            int len = 0;
            switch (op) {
            case 0x00: len = rem; break;                                            // null information: the rest
            case 0x08: len = p + 1 < n ? b[p + 1] & 0x3F : 0; break;                // null avoid zero bias
            case 0x11: len = p + 1 < n ? ((b[p + 1] & 3) + 1) * 2 + 2 : 0; break;   // indirect group paging
            case 0x12: len = p + 1 < n ? ((b[p + 1] & 3) + 1) * 3 + 2 : 0; break;   // individual paging with priority
            default:
                if ((op >> 6) == 2 && p + 2 < n) len = b[p + 2] & 0x3F;              // manufacturer specific: its own length
                if (len == 0) len = kMacMsgLen[op];
            }
            if (op != 0x00 && op != 0x08 && m.msgOpcode < 0) m.msgOpcode = op;
            if (op == 0x01 && rem >= 7 && m.tg < 0) { m.svcOpts = b[p + 1]; m.tg = get16(b + p + 2); m.src = get24(b + p + 4); m.group = true; }
            else if (op == 0x02 && rem >= 8 && m.tg < 0) { m.svcOpts = b[p + 1]; m.tg = get24(b + p + 2); m.src = get24(b + p + 5); m.group = false; }
            if (len <= 0) break;
            p += len;
        }
        return true;
    default: return true;
    }
}

// ------------------------------------------------------------------------------------------------ voice frames (OP25 p25p2_vf.cc)

void voiceDeinterleave(const uint8_t dib[36], char fr[4][24]) {
    memset(fr, 0, 4 * 24);
    for (int i = 0; i < 72; i++) {
        const int bit = (dib[i / 2] >> (i & 1 ? 0 : 1)) & 1;
        fr[kVf[i][0]][kVf[i][1]] = (char)bit;
    }
}

void voiceInterleave(const uint32_t c[4], uint8_t dib[36]) {
    memset(dib, 0, 36);
    for (int i = 0; i < 72; i++) {
        const int bit = (int)((c[kVf[i][0]] >> kVf[i][1]) & 1);
        dib[i / 2] = (uint8_t)(dib[i / 2] | bit << (i & 1 ? 0 : 1));
    }
}

void voiceFr(const uint32_t c[4], char fr[4][24]) {
    static const int len[4] = {24, 23, 11, 14};
    memset(fr, 0, 4 * 24);
    for (int k = 0; k < 4; k++) for (int j = 0; j < len[k]; j++) fr[k][j] = (char)((c[k] >> j) & 1);
}

// ------------------------------------------------------------------------------------------------ bursts (OP25 p25p2_tdma.cc handle_packet / handle_acch_frame)

namespace {
// the timeslot dibits that carry FACCH and SACCH (OP25: burst dibits 11.. counted from 10 dibits before the timeslot)
struct Span { int at, n; };
const Span kFacchSpans[4] = {{1, 36}, {38, 31}, {90, 32}, {123, 36}};   // 135 dibits = 270 bits = 45 hexbits
const Span kSacchSpans[3] = {{1, 36}, {38, 84}, {123, 36}};             // 156 dibits = 312 bits = 52 hexbits
template <size_t N> void gather(const uint8_t* dib, const Span (&sp)[N], uint8_t* bits) {
    int k = 0;
    for (const Span& s : sp) for (int i = 0; i < s.n; i++) { bits[k++] = (dib[s.at + i] >> 1) & 1; bits[k++] = dib[s.at + i] & 1; }
}
template <size_t N> void scatter(uint8_t* dib, const Span (&sp)[N], const uint8_t* bits) {
    int k = 0;
    for (const Span& s : sp) for (int i = 0; i < s.n; i++) { dib[s.at + i] = (uint8_t)(bits[k] << 1 | bits[k + 1]); k += 2; }
}
// The MAC PDU in a (63,35) codeword: FACCH puts its 26 data hexbits at positions 9..34 and sends 9..53 (54..62 punctured), SACCH its 30 at
// 5..34 and sends 5..56 (57..62 punctured); the positions before are shortened (zero)
void macEncode(const std::vector<uint8_t>& mac, bool fast, uint8_t* bits) {
    const int first = fast ? 9 : 5, macBits = fast ? 144 : 168, sent = fast ? 45 : 52;
    uint8_t info[180] = {};
    for (int i = 0; i < macBits; i++) { const size_t by = (size_t)i / 8; info[i] = by < mac.size() ? (uint8_t)((mac[by] >> (7 - i % 8)) & 1) : 0; }
    const uint16_t c = crc12(info, macBits);
    for (int i = 0; i < 12; i++) info[macBits + i] = (uint8_t)((c >> (11 - i)) & 1);
    uint8_t cw[63] = {};
    for (int h = 0; h < 35 - first; h++) { int v = 0; for (int b = 0; b < 6; b++) v = v << 1 | info[6 * h + b]; cw[first + h] = (uint8_t)v; }
    rs63Encode(cw);
    for (int h = 0; h < sent; h++) for (int b = 0; b < 6; b++) bits[6 * h + b] = (uint8_t)((cw[first + h] >> (5 - b)) & 1);
}
bool macDecode(const uint8_t* bits, bool fast, MacPdu& m, int* corrected) {
    const int first = fast ? 9 : 5, macBits = fast ? 144 : 168, sent = fast ? 45 : 52;
    uint8_t cw[63] = {};
    for (int h = 0; h < sent; h++) { int v = 0; for (int b = 0; b < 6; b++) v = v << 1 | bits[6 * h + b]; cw[first + h] = (uint8_t)v; }
    std::vector<int> eras;
    for (int p = first + sent; p < 63; p++) eras.push_back(p);
    if (!rs63Decode(cw, eras, corrected)) return false;
    for (int p = 0; p < first; p++) if (cw[p]) return false;   // a shortened position came out non-zero: miscorrected
    uint8_t info[180];
    for (int h = 0; h < 35 - first; h++) for (int b = 0; b < 6; b++) info[6 * h + b] = (uint8_t)((cw[first + h] >> (5 - b)) & 1);
    uint16_t c = 0;
    for (int i = 0; i < 12; i++) c = (uint16_t)(c << 1 | info[macBits + i]);
    if (crc12(info, macBits) != c) return false;
    uint8_t by[21] = {};
    for (int i = 0; i < macBits; i++) by[i / 8] = (uint8_t)(by[i / 8] | info[i] << (7 - i % 8));
    return macParse(by, macBits / 8, m);
}
void hexToDibits(const uint8_t* hex, int n, uint8_t* dib, int skipAt = -1) {   // 3 dibits per hexbit (skipping one dibit position)
    int d = 0;
    for (int i = 0; i < n; i++) for (int k = 0; k < 3; k++) { if (d == skipAt) d++; dib[d++] = (uint8_t)((hex[i] >> (4 - 2 * k)) & 3); }
}
void dibitsToHex(const uint8_t* dib, int n, uint8_t* hex, int skipAt = -1) {
    int d = 0;
    for (int i = 0; i < n; i++) { int v = 0; for (int k = 0; k < 3; k++) { if (d == skipAt) d++; v = v << 2 | (dib[d++] & 3); } hex[i] = (uint8_t)v; }
}
} // namespace

void burstBuild(int duid, const uint8_t (*voice)[36], const uint8_t* ess, const std::vector<uint8_t>& mac, const uint8_t* scr, uint8_t dib[kBurstDibits]) {
    memset(dib, 0, kBurstDibits);
    if (duid == k4V || duid == k2V) {
        const int nv = duid == k4V ? 4 : 2;
        for (int f = 0; f < nv; f++) memcpy(dib + kVoicePos[f], voice[f], 36);
        if (ess) {
            if (duid == k4V) hexToDibits(ess, 4, dib + kEssBPos);
            else hexToDibits(ess, 28, dib + kEssAPos, kDuidPos[2] - kEssAPos);
        }
    } else if (duid == kFacch || duid == kFacchClear) {
        uint8_t bits[270];
        macEncode(mac, true, bits);
        scatter(dib, kFacchSpans, bits);
    } else if (duid == kSacch || duid == kSacchClear) {
        uint8_t bits[312];
        macEncode(mac, false, bits);
        scatter(dib, kSacchSpans, bits);
    }
    if (scr && duidScrambled(duid)) for (int i = 0; i < kBurstDibits; i++) dib[i] ^= (uint8_t)(scr[2 * i] << 1 | scr[2 * i + 1]);
    const uint8_t dv = duidEncode(duid);
    for (int j = 0; j < 4; j++) dib[kDuidPos[j]] = (uint8_t)((dv >> (6 - 2 * j)) & 3);   // the DUID is never scrambled
}

void burstParse(const uint8_t raw[kBurstDibits], const uint8_t* scr, Burst& b) {
    b = Burst();
    uint8_t dv = 0;
    for (int j = 0; j < 4; j++) dv = (uint8_t)(dv << 2 | (raw[kDuidPos[j]] & 3));
    b.duid = duidDecode(dv, &b.duidErrs);
    if (b.duid < 0) return;
    uint8_t dib[kBurstDibits];
    for (int i = 0; i < kBurstDibits; i++) dib[i] = (uint8_t)((raw[i] ^ (scr && duidScrambled(b.duid) ? (scr[2 * i] << 1 | scr[2 * i + 1]) : 0)) & 3);
    if (b.duid == k4V || b.duid == k2V) {
        b.nVoice = b.duid == k4V ? 4 : 2;
        for (int f = 0; f < b.nVoice; f++) memcpy(b.voice[f], dib + kVoicePos[f], 36);
        if (b.duid == k4V) dibitsToHex(dib + kEssBPos, 4, b.ess);
        else dibitsToHex(dib + kEssAPos, 28, b.ess, kDuidPos[2] - kEssAPos);
    } else if (b.duid == kFacch || b.duid == kFacchClear) {
        uint8_t bits[270];
        gather(dib, kFacchSpans, bits);
        b.macPresent = true;
        b.macOk = macDecode(bits, true, b.mac, &b.rsCorrected);
    } else if (b.duid == kSacch || b.duid == kSacchClear) {
        uint8_t bits[312];
        gather(dib, kSacchSpans, bits);
        b.macPresent = true;
        b.macOk = macDecode(bits, false, b.mac, &b.rsCorrected);
    }
}

void essBuild(int algId, int keyId, const uint8_t mi[9], uint8_t essB[16], uint8_t essA[28]) {
    uint8_t hex[44] = {};
    hex[0] = (uint8_t)(algId >> 2 & 63);   // OP25 handle_4V2V_ess: algid = B0 << 2 | B1 >> 4; key = (B1 & 15) << 12 | B2 << 6 | B3; MI from B4..B15
    hex[1] = (uint8_t)((algId & 3) << 4 | (keyId >> 12 & 15));
    hex[2] = (uint8_t)(keyId >> 6 & 63);
    hex[3] = (uint8_t)(keyId & 63);
    for (int j = 0, i = 0; i < 9; i += 3, j += 4) {
        const uint8_t a = mi ? mi[i] : 0, b = mi ? mi[i + 1] : 0, c = mi ? mi[i + 2] : 0;
        hex[4 + j] = (uint8_t)(a >> 2);
        hex[5 + j] = (uint8_t)((a & 3) << 4 | b >> 4);
        hex[6 + j] = (uint8_t)((b & 15) << 2 | c >> 6);
        hex[7 + j] = (uint8_t)(c & 63);
    }
    p25::rsEncode(hex, 44, 16);   // (63,35) shortened to 44: the ESS-B hexbits are the data, ESS-A the 28 parity
    memcpy(essB, hex, 16);
    memcpy(essA, hex + 16, 28);
}

bool essParse(const uint8_t essB[16], const uint8_t essA[28], int& algId, int& keyId, uint8_t mi[9]) {
    uint8_t hex[44];
    memcpy(hex, essB, 16);
    memcpy(hex + 16, essA, 28);
    if (!p25::rsDecode(hex, 44, 16)) return false;
    algId = hex[0] << 2 | hex[1] >> 4;
    keyId = (hex[1] & 15) << 12 | hex[2] << 6 | hex[3];
    if (mi) for (int j = 0, i = 0; i < 9; j += 4) {
        mi[i++] = (uint8_t)(hex[j + 4] << 2 | hex[j + 5] >> 4);
        mi[i++] = (uint8_t)((hex[j + 5] & 15) << 4 | hex[j + 6] >> 2);
        mi[i++] = (uint8_t)((hex[j + 6] & 3) << 6 | hex[j + 7]);
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ frame layer

void Framer::reset() {
    reg_ = 0; locked_ = false; rev_ = false; pos_ = 0; slot_ = -1; misses_ = 0;
    essBn_[0] = essBn_[1] = 0;
    out_.clear();
}

void Framer::push(int d) {
    reg_ = ((reg_ << 2) | (uint64_t)(d & 3)) & 0xFFFFFFFFFFull;
    // the S-ISCH, or the S-ISCH with every dibit's first bit flipped (an inverted spectrum: OP25 P25P2_FRAME_SYNC_REV_P)
    const int dn = __builtin_popcountll(reg_ ^ kSisch), di = __builtin_popcountll(reg_ ^ kSisch ^ 0xAAAAAAAAAAull);
    if (!locked_) {
        if (dn <= 4 || di <= 4) {   // an S-ISCH just ended: its timeslot follows (which one, the next I-ISCH says)
            locked_ = true; rev_ = di < dn; pos_ = kIschDibits; misses_ = 0; slot_ = -1;
            essBn_[0] = essBn_[1] = 0;
            for (int i = 0; i < kIschDibits; i++) buf_[i] = (uint8_t)((kSisch >> (38 - 2 * i)) & 3);
        }
        return;
    }
    buf_[pos_++] = (uint8_t)((d ^ (rev_ ? 2 : 0)) & 3);
    if (pos_ == kIschDibits) {   // this frame's ISCH is in
        uint64_t cw = 0;
        for (int i = 0; i < kIschDibits; i++) cw = cw << 2 | buf_[i];
        int dist = 0;
        const int info = ischDecode(cw, &dist);
        const int expect = slot_ < 0 ? -1 : slot_;
        if (info >= 0) {
            const int s = ischSlot(info);
            if (s < kSlotsPerSuperframe && !ischIsSync(s)) {
                if (slot_ < 0) { Event e; e.kind = Event::Lock; out_.push_back(e); }
                slot_ = s; misses_ = 0;
            } else misses_++;
        } else if (info == -2 && (expect < 0 || ischIsSync(expect))) misses_ = 0;
        else misses_++;
        if (misses_ >= 6) {
            locked_ = false; pos_ = 0; slot_ = -1;
            Event e; e.kind = Event::Lost; out_.push_back(e);
            return;
        }
    }
    if (pos_ == kSlotDibits) {
        if (slot_ >= 0) frameDone();
        pos_ = 0;
        if (slot_ >= 0) { slot_ = (slot_ + 1) % kSlotsPerSuperframe; if (slot_ == 0) sf_++; }
    }
}

void Framer::frameDone() {
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
            if (essBn_[c] < 4) memcpy(essB_[c] + 4 * essBn_[c]++, b.ess, 4);
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
        if (!c.active) { c = Call(); c.active = true; c.lateEntry = true; c.frames = (uint32_t)e.nVoice; return true; }
        c.frames += (uint32_t)e.nVoice;
        return false;
    case Event::Lost:
        for (Call& x : c_) if (x.active) x.active = false;
        return true;
    default: return false;
    }
}

// ------------------------------------------------------------------------------------------------ generator

namespace {
uint64_t splitmix(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
}

Generator::Generator(int wacn, int sysId, int nac) : nac_(nac) { scr_.seed(wacn, sysId, nac); }

void Generator::voiceWords(int chan, uint32_t n, uint32_t c[4]) {
    uint64_t s = 0xA5A5ull * (uint64_t)(chan + 1) + (uint64_t)n * 0x10001ull;
    const uint64_t a = splitmix(s), b = splitmix(s);
    c[0] = (uint32_t)p25::golay24Encode((int)(a & 0xFFF));            // c0: Golay (24,12) of 12 data bits
    c[1] = (uint32_t)((a >> 12) & 0x7FFFFF);                          // c1: 23 bits (its Golay and the scrambling are the vocoder's business)
    c[2] = (uint32_t)(b & 0x7FF);
    c[3] = (uint32_t)((b >> 11) & 0x3FFF);
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
    uint8_t essB[2][16] = {}, essA[2][28] = {};
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
        const uint64_t isch = ischIsSync(k) ? kSisch : ischEncode(ischInfo(k, 0, s & 3));
        for (int i = 0; i < kIschDibits; i++) out.push_back((uint8_t)((isch >> (38 - 2 * i)) & 3));
        uint8_t dib[kBurstDibits];
        const GenCall* g = call[c];
        std::vector<uint8_t> mac;
        int duid = kFacch;
        uint8_t v[4][36] = {};
        const uint8_t* ess = nullptr;
        if (sacchSlot(k)) {
            duid = kSacch;
            mac = g && phase[c] <= 1 ? macWithUser(kMacActive, g->tg, g->src, g->algId != kAlgClear ? 0x40 : 0) : macIdle();
        } else if (g && phase[c] == 1) {
            const int j = nth[c]++;
            duid = j < 4 ? k4V : k2V;
            const int nv = j < 4 ? 4 : 2;
            for (int f = 0; f < nv; f++) { uint32_t w[4]; voiceWords(c, vn_[c]++, w); voiceInterleave(w, v[f]); }
            ess = j < 4 ? essB[c] + 4 * j : essA[c];
        } else if (g && phase[c] == 0) {
            mac = nth[c]++ == 4 ? macPtt(g->src, g->tg, g->algId, g->keyId) : macIdle();
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
