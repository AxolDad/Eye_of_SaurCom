// P25 Phase 1 protocol pieces on top of the Rust crate rust/saurcom_p25: the functions of p25_proto.h
// (p25_proto.cpp keeps the frame sync table `kSync`).
#include "dect2/p25/p25_proto.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/p25.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace p25 {

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[3];
        saurcom_p25_struct_sizes(s);
        dect2::rsAbiCheck("saurcom_p25", s, {sizeof(SaurcomP25Tsbk), sizeof(SaurcomP25Lc), sizeof(SaurcomP25Crypto)});
    }
} abiCheck;
inline const char* cs(const uint8_t* p) { return reinterpret_cast<const char*>(p); }
SaurcomP25Crypto toC(const Crypto& c) {
    SaurcomP25Crypto o;
    memset(&o, 0, sizeof o);
    memcpy(o.mi, c.mi, 9);
    o.alg_id = c.algId; o.key_id = c.keyId; o.mfid = c.mfid; o.tg = c.tg;
    return o;
}
void fromC(const SaurcomP25Crypto& o, Crypto& c) {
    memcpy(c.mi, o.mi, 9);
    c.algId = o.alg_id; c.keyId = o.key_id; c.mfid = o.mfid; c.tg = o.tg;
}
} // namespace

const char* duidName(int d) { return cs(saurcom_p25_duid_name(d)); }
int frameLen(int d) { return saurcom_p25_frame_len(d); }

void nidEncode(int nac, int duid, uint8_t bits[64]) { saurcom_p25_nid_encode(nac, duid, bits); }
bool nidDecode(const uint8_t bits[64], int& nac, int& duid, int* errs) {
    int32_t n = 0, d = 0, e = 0;
    const bool ok = saurcom_p25_nid_decode(bits, &n, &d, &e);
    if (errs) *errs = e;
    if (ok) { nac = n; duid = d; }
    return ok;
}

void rsEncode(uint8_t* sym, int n, int k) { saurcom_p25_rs_encode(sym, n, k); }
bool rsDecode(uint8_t* sym, int n, int k, int* errs) {
    int32_t e = 0;
    const bool ok = saurcom_p25_rs_decode(sym, n, k, &e);
    if (errs) *errs = e;
    return ok;
}

uint16_t hamming10Encode(int hex) { return saurcom_p25_hamming10_encode(hex); }
int hamming10Decode(uint16_t cw, int* errs) { int32_t e = 0; const int h = saurcom_p25_hamming10_decode(cw, &e); if (errs) *errs = e; return h; }
uint32_t golay24Encode(int d12) { return saurcom_p25_golay24_encode(d12); }
int golay24Decode(uint32_t cw, int* errs) { int32_t e = 0; const int d = saurcom_p25_golay24_decode(cw, &e); if (errs) *errs = e; return d; }
uint32_t golay18Encode(int hex) { return saurcom_p25_golay18_encode(hex); }
int golay18Decode(uint32_t cw, int* errs) { int32_t e = 0; const int h = saurcom_p25_golay18_decode(cw, &e); if (errs) *errs = e; return h; }

uint16_t crcCcitt(const uint8_t* bits, int n) { return saurcom_p25_crc_ccitt(bits, n < 0 ? 0 : (size_t)n); }

void trellisEncode(const uint8_t bits[96], uint8_t dibits[98]) { saurcom_p25_trellis_encode(bits, dibits); }
bool trellisDecode(const float lv[98], uint8_t bits[96], float* metric) {
    float m = 0;
    const bool ok = saurcom_p25_trellis_decode(lv, bits, &m);
    if (metric) *metric = m;
    return ok;
}

void tsbkBits(const Tsbk& t, uint8_t b[96]) {
    SaurcomP25Tsbk c;
    memset(&c, 0, sizeof c);
    c.last = t.last; c.prot = t.prot; c.opcode = t.opcode; c.mfid = t.mfid; c.args = t.args;
    saurcom_p25_tsbk_bits(&c, b);
}
bool tsbkParse(const uint8_t b[96], Tsbk& t) {
    SaurcomP25Tsbk c;
    const bool ok = saurcom_p25_tsbk_parse(b, &c);
    t.last = c.last != 0; t.prot = c.prot != 0; t.opcode = c.opcode; t.mfid = c.mfid; t.args = c.args;
    return ok;
}
const char* opcodeName(int op) { return cs(saurcom_p25_opcode_name(op)); }

void lcHexbits(const Lc& l, uint8_t hex[12]) {
    SaurcomP25Lc c;
    memset(&c, 0, sizeof c);
    c.lco = l.lco; c.mfid = l.mfid; c.prot = l.prot; c.svc_opts = l.svcOpts; c.tg = l.tg; c.src = l.src; c.dst = l.dst; c.raw = l.raw;
    saurcom_p25_lc_hexbits(&c, hex);
}
bool lcParse(const uint8_t hex[12], Lc& l) {
    SaurcomP25Lc c;
    saurcom_p25_lc_parse(hex, &c);
    l = Lc();
    l.lco = c.lco; l.mfid = c.mfid; l.prot = c.prot != 0; l.svcOpts = c.svc_opts; l.tg = c.tg; l.src = c.src; l.dst = c.dst; l.raw = c.raw;
    return true;
}

const char* algName(int a) { return cs(saurcom_p25_alg_name(a)); }
void esHexbits(const Crypto& c, uint8_t hex[16]) { const SaurcomP25Crypto o = toC(c); saurcom_p25_es_hexbits(&o, hex); }
void esParse(const uint8_t hex[16], Crypto& c) { SaurcomP25Crypto o = toC(c); saurcom_p25_es_parse(hex, &o); fromC(o, c); }
void hduHexbits(const Crypto& c, uint8_t hex[20]) { const SaurcomP25Crypto o = toC(c); saurcom_p25_hdu_hexbits(&o, hex); }
void hduParse(const uint8_t hex[20], Crypto& c) { SaurcomP25Crypto o = toC(c); saurcom_p25_hdu_parse(hex, &o); fromC(o, c); }

void imbeDeinterleave(const uint8_t dibits[72], char fr[8][23]) { saurcom_p25_imbe_deinterleave(dibits, (uint8_t*)fr); }
void imbeInterleave(const char fr[8][23], uint8_t dibits[72]) { saurcom_p25_imbe_interleave((const uint8_t*)fr, dibits); }
void imbeFlatten(const char fr[8][23], uint8_t bits[144]) { saurcom_p25_imbe_flatten((const uint8_t*)fr, bits); }
void imbeUnflatten(const uint8_t bits[144], char fr[8][23]) { saurcom_p25_imbe_unflatten(bits, (uint8_t*)fr); }

std::string channelText(int ch) {
    uint8_t b[24];
    const size_t n = saurcom_p25_channel_text(ch, b, sizeof b);
    return std::string((const char*)b, n);
}

} // namespace p25
} // namespace dect2
