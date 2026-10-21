// COSPAS-SARSAT second-generation beacon message layer (see sgb_proto.h). C/S T.018 Issue 1 Rev 4, public document.
#include "dect2/sarsat/sgb_proto.h"
#include <cmath>
#include <cstdio>

namespace dect2::sarsat {

namespace {

// the PRN generator: x^23 + x^18 + 1, registers 22..0 shift towards register 0 which is the output; feedback = reg 0 xor reg 18 (checked against the
// 64 first chips of Table 2.2 in the tests)
std::vector<uint8_t> lfsr(uint32_t init, int n) {
    std::vector<uint8_t> v((size_t)n);
    uint32_t r = init;
    for (int i = 0; i < n; i++) {
        v[(size_t)i] = (uint8_t)(r & 1);
        const uint32_t fb = (r ^ (r >> 18)) & 1;
        r = (r >> 1) | (fb << 22);
    }
    return v;
}

// GF(2^8) with the primitive polynomial m1(X) = X^8 + X^4 + X^3 + X^2 + 1
struct Gf {
    int ex[512], lg[256];
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; i++) { ex[i] = x; lg[x] = i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
        for (int i = 255; i < 512; i++) ex[i] = ex[i - 255];
        lg[0] = 0;
    }
    int mul(int a, int b) const { return a && b ? ex[lg[a] + lg[b]] : 0; }
    int inv(int a) const { return ex[255 - lg[a]]; }
};
const Gf& gf() { static const Gf g; return g; }

uint64_t polyMul(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int i = 0; i < 64; i++) if ((b >> i) & 1) r ^= a << i;
    return r;
}

uint64_t field(const std::vector<uint8_t>& b, int first, int n) {   // bits first..first+n-1 (1-based), MSB first
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | (b[(size_t)(first - 1 + i)] & 1);
    return v;
}
void put(std::vector<uint8_t>& b, int first, int n, uint64_t v) {
    for (int i = 0; i < n; i++) b[(size_t)(first - 1 + i)] = (uint8_t)((v >> (n - 1 - i)) & 1);
}

// position: sign flag, whole degrees, 15 bit fraction
void encPos(std::vector<uint8_t>& b, int first, int degBits, double v, double maxDeg, bool def, bool latitude) {
    if (def) {
        put(b, first, 1, 0);
        put(b, first + 1, degBits, (1u << degBits) - 1);
        put(b, first + 1 + degBits, 15, latitude ? 0x03E0 : 0x7C1F);   // 000001111100000 / 111110000011111
        return;
    }
    const bool neg = v < 0;
    double a = std::fabs(v);
    if (a > maxDeg) a = maxDeg;
    long total = std::lround(a * 32768.0);
    long deg = total >> 15;
    long frac = total & 0x7FFF;
    put(b, first, 1, neg ? 1 : 0);
    put(b, first + 1, degBits, (uint64_t)deg);
    put(b, first + 1 + degBits, 15, (uint64_t)frac);
}
double decPos(const std::vector<uint8_t>& b, int first, int degBits) {
    const bool neg = b[(size_t)(first - 1)] != 0;
    const uint64_t deg = field(b, first + 1, degBits), frac = field(b, first + 1 + degBits, 15);
    const double v = (double)deg + (double)frac / 32768.0;
    return neg ? -v : v;
}

}  // namespace

std::vector<uint8_t> sgbPrn(bool q, bool selfTest) {
    // initial register values of Table 2.2, register 22 written leftmost
    static const char* kInit[2][2] = {{"00000000000000000000001", "00110101100000111111100"}, {"10100101100100111110000", "01111001110100100101000"}};
    // (the Q rows are the second column; the self-test rows the second line)
    const char* s = kInit[selfTest ? 1 : 0][q ? 1 : 0];
    uint32_t r = 0;
    for (int i = 0; i < 23; i++) if (s[i] == '1') r |= 1u << (22 - i);
    return lfsr(r, kSgbChips);
}

uint64_t sgbGenerator() {
    static const uint64_t m[6] = {0x11D, 0x177, 0x1F3, 0x169, 0x1BD, 0x1E7};   // m1, m3, m5, m7, m9, m11 of annex B.1
    uint64_t g = 1;
    for (uint64_t p : m) g = polyMul(g, p);
    return g;
}

uint64_t sgbBch(const std::vector<uint8_t>& info) {
    const uint64_t g = sgbGenerator(), mask = (1ull << 48) - 1;
    uint64_t reg = 0;
    for (int i = 0; i < kSgbInfoBits; i++) {   // the register then holds m(X) X^48 mod g(X)
        const uint64_t bit = info[(size_t)i] & 1;
        const uint64_t fb = ((reg >> 47) & 1) ^ bit;
        reg = (reg << 1) & mask;
        if (fb) reg ^= g & mask;
    }
    return reg;
}

int sgbCorrect(std::vector<uint8_t>& bits) {
    if (bits.size() < (size_t)kSgbMsgBits) return -1;
    const Gf& f = gf();
    int S[13] = {0};
    bool any = false;
    for (int p = 1; p <= kSgbMsgBits; p++) {
        if (!(bits[(size_t)(p - 1)] & 1)) continue;
        const int d = kSgbMsgBits - p;   // degree of this bit in the codeword polynomial
        for (int i = 1; i <= 12; i++) S[i] ^= f.ex[(i * d) % 255];
    }
    for (int i = 1; i <= 12; i++) any |= S[i] != 0;
    if (!any) return 0;
    // Berlekamp-Massey
    int C[16] = {1}, B[16] = {1};
    int L = 0, m = 1, bb = 1;
    for (int n = 0; n < 12; n++) {
        int d = S[n + 1];
        for (int i = 1; i <= L; i++) d ^= f.mul(C[i], S[n + 1 - i]);
        if (d == 0) { m++; continue; }
        int T[16];
        for (int i = 0; i < 16; i++) T[i] = C[i];
        const int coef = f.mul(d, f.inv(bb));
        for (int i = 0; i + m < 16; i++) C[i + m] ^= f.mul(coef, B[i]);
        if (2 * L <= n) { L = n + 1 - L; for (int i = 0; i < 16; i++) B[i] = T[i]; bb = d; m = 1; }
        else m++;
    }
    if (L > 6) return -1;
    // Chien search over the degrees 0..249 (a root at alpha^-d marks an error at degree d)
    int found[8], nf = 0;
    for (int d = 0; d < 255; d++) {
        int v = 0;
        for (int i = 0; i <= L; i++) if (C[i]) v ^= f.ex[(f.lg[C[i]] + ((255 - d) % 255) * i) % 255];
        if (v == 0) { if (d >= kSgbMsgBits || nf >= 7) return -1; found[nf++] = d; }
    }
    if (nf != L) return -1;
    for (int k = 0; k < nf; k++) bits[(size_t)(kSgbMsgBits - 1 - found[k])] ^= 1;
    return nf;
}

std::vector<uint8_t> sgbEncode(const SgbSpec& s) {
    std::vector<uint8_t> b(kSgbMsgBits, 0);
    put(b, 1, 16, (uint64_t)s.tac);
    put(b, 17, 14, (uint64_t)s.serial);
    put(b, 31, 10, (uint64_t)s.mid);
    put(b, 41, 1, s.homing);
    put(b, 42, 1, s.rls);
    put(b, 43, 1, s.test);
    encPos(b, 44, 7, s.lat, 90, !s.hasPos, true);
    encPos(b, 67, 8, s.lon, 180, !s.hasPos, false);
    put(b, 91, 3, (uint64_t)s.vesselType);
    put(b, 94, 44, s.vesselId);
    put(b, 138, 3, (uint64_t)s.beaconType);
    put(b, 141, 14, s.cancel ? 0 : 0x3FFF);
    put(b, 155, 4, (uint64_t)s.rotId);
    put(b, 159, 44, s.rotData);
    std::vector<uint8_t> info(b.begin(), b.begin() + kSgbInfoBits);
    put(b, 203, 48, sgbBch(info));
    return b;
}

std::string sgbHexId(const std::vector<uint8_t>& b) {
    std::vector<uint8_t> h(92, 0);
    h[0] = 1;
    for (int i = 0; i < 10; i++) h[(size_t)(1 + i)] = b[(size_t)(30 + i)];   // bits 31-40
    h[11] = 1; h[12] = 0; h[13] = 1;
    for (int i = 0; i < 30; i++) h[(size_t)(14 + i)] = b[(size_t)i];          // TAC and serial, bits 1-30
    h[44] = b[42];                                                              // bit 43, test protocol
    for (int i = 0; i < 3; i++) h[(size_t)(45 + i)] = b[(size_t)(90 + i)];    // bits 91-93
    for (int i = 0; i < 44; i++) h[(size_t)(48 + i)] = b[(size_t)(93 + i)];   // bits 94-137
    std::string out;
    for (int i = 0; i < 92; i += 4) { char c[2]; snprintf(c, sizeof c, "%X", (unsigned)((h[(size_t)i] << 3) | (h[(size_t)i + 1] << 2) | (h[(size_t)i + 2] << 1) | h[(size_t)i + 3])); out += c; }
    return out;
}

bool sgbDecode(const std::vector<uint8_t>& b, Beacon& o) {
    if (b.size() < (size_t)kSgbMsgBits) return false;
    const uint64_t spare = field(b, 141, 14);
    if (spare != 0x3FFF && spare != 0) return false;
    const int type = (int)field(b, 138, 3);
    if (type > 3) return false;
    Beacon r;
    r.valid = true;
    r.second = true;
    r.longMsg = true;
    r.tac = (int)field(b, 1, 16);
    r.serial = (int)field(b, 17, 14);
    r.mid = (int)field(b, 31, 10);
    r.country = countryOfMid(r.mid);
    r.homing = b[40] != 0;
    r.rls = b[41] != 0;
    r.testProto = r.selfTest = b[42] != 0;
    static const char* kType[] = {"ELT", "EPIRB", "PLB", "ELT(DT)"};
    r.beaconType = kType[type];
    r.protoName = spare == 0 ? "Second generation (cancellation)" : "Second generation";
    r.hexId = sgbHexId(b);
    char t[64];
    snprintf(t, sizeof t, "TAC %d, serial %d", r.tac, r.serial);
    r.idText = t;
    const uint64_t latBits = field(b, 44, 23), lonBits = field(b, 67, 24);
    const bool dflt = latBits == ((0x7Full << 15) | 0x3E0) && lonBits == ((0xFFull << 15) | 0x7C1F);   // "no position": 0 1111111 000001111100000 / 0 11111111 111110000011111
    if (!dflt) {
        r.hasPos = true;
        r.fine = true;
        r.lat = decPos(b, 44, 7);
        r.lon = decPos(b, 67, 8);
        r.posPrecisionDeg = 0.5 / 32768.0;
        if (std::fabs(r.lat) > 90.0 || std::fabs(r.lon) > 180.0) r.hasPos = false;
    }
    r.rotId = (int)field(b, 155, 4);
    r.bch2Present = false;
    r.rawHex = bitsToHex(b, 1, kSgbMsgBits);
    o = r;
    return true;
}

}  // namespace dect2::sarsat
