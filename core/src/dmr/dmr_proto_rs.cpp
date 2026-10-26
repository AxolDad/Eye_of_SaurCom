// DMR burst layout and message formats on top of the Rust crate rust/zorcom_dmr: the functions of dmr_proto.h;
// dmr_proto.cpp keeps the sync word table, the three burst slicers (vector copies), be24 / putBe24 and the talker alias PDU builder of the test signal.
// Bit vectors that are shorter than the format needs are zero padded.
#include "dect2/dmr/dmr_proto.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/dmr.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace dmr {

namespace {
struct AbiCheck {
    AbiCheck() {
        size_t s[5];
        zorcom_dmr_struct_sizes(s);
        dect2::rsAbiCheck("zorcom_dmr", s, {sizeof(ZorcomDmrFullLc), sizeof(ZorcomDmrCsbk), sizeof(ZorcomDmrDataHeader), sizeof(ZorcomDmrDataBlock), sizeof(ZorcomDmrTa)});
    }
} abiCheck;

template <size_t N> void take(const Bits& b, uint8_t (&a)[N]) {
    memset(a, 0, N);
    memcpy(a, b.data(), std::min(b.size(), N));
}
inline const char* cs(const uint8_t* p) { return reinterpret_cast<const char*>(p); }

ZorcomDmrFullLc toC(const FullLc& l) {
    ZorcomDmrFullLc c;
    memset(&c, 0, sizeof c);
    c.pf = l.pf; c.flco = l.flco; c.fid = l.fid; c.svc = l.svc; c.dst = l.dst; c.src = l.src;
    memcpy(c.raw, l.raw, 9);
    return c;
}
ZorcomDmrDataHeader toC(const DataHeader& h) {
    ZorcomDmrDataHeader c;
    memset(&c, 0, sizeof c);
    c.dpf = h.dpf; c.sap = h.sap; c.group = h.group; c.resp = h.resp; c.dst = h.dst; c.src = h.src; c.blocks = h.blocks; c.pad = h.pad; c.fmf = h.fmf;
    c.fsn = h.fsn; c.ns = h.ns; c.dd = h.dd; c.sp = h.sp; c.dp = h.dp; c.resync = h.resync; c.sarq = h.sarq; c.status = h.status;
    return c;
}
ZorcomDmrTa toC(const TalkerAlias& t) {
    ZorcomDmrTa c;
    memset(&c, 0, sizeof c);
    c.format = t.format; c.length = t.length; c.have = t.have;
    memcpy(c.data, t.data, 28);
    return c;
}
} // namespace

const char* syncName(int s) { return cs(zorcom_dmr_sync_name(s)); }
int syncDirectSlot(int s) { return zorcom_dmr_sync_direct_slot(s); }
int syncOf(int family, bool voice) { return zorcom_dmr_sync_of(family, voice); }
void syncSymbols(int s, int8_t out[24]) { zorcom_dmr_sync_symbols(s, out); }

int matchSync(const Bits& centre48, int maxDiff, int* diff) {
    uint8_t c[48];
    take(centre48, c);
    int32_t d = 0;
    const int r = zorcom_dmr_match_sync(c, maxDiff, &d);
    if (diff) *diff = d;
    return r;
}

const char* dataTypeName(int dt) { return cs(zorcom_dmr_data_type_name(dt)); }
const char* dataTypeShort(int dt) { return cs(zorcom_dmr_data_type_short(dt)); }

Bits makeDataBurst(int cc, int dt, const Bits& p, int sync) {
    uint8_t pay[196], out[264];
    take(p, pay);
    if (!zorcom_dmr_make_data_burst(cc, dt, pay, sync, out)) return Bits();
    return Bits(out, out + 264);
}

Bits makeVoiceBurst(const Bits& vs, int sync, int cc, int pi, int lcss, const Bits& emb32) {
    uint8_t v[216], e[32], out[264];
    take(vs, v);
    take(emb32, e);
    if (!zorcom_dmr_make_voice_burst(v, sync, cc, pi, lcss, e, out)) return Bits();
    return Bits(out, out + 264);
}

SlotTypeInfo burstSlotType(const Bits& burst) {
    SlotTypeInfo r;
    int32_t cc = 0, dt = 0, e = 0;
    if (!zorcom_dmr_burst_slot_type(burst.data(), burst.size(), &cc, &dt, &e)) return r;
    r.ok = true; r.errors = e; r.cc = cc; r.dt = dt;
    return r;
}

EmbInfo burstEmb(const Bits& burst) {
    EmbInfo r;
    uint8_t e32[32];
    int32_t cc = 0, pi = 0, lcss = 0, e = 0;
    const bool ok = zorcom_dmr_burst_emb(burst.data(), burst.size(), e32, &cc, &pi, &lcss, &e);
    r.emb32.assign(e32, e32 + 32);
    if (!ok) return r;
    r.ok = true; r.errors = e; r.cc = cc; r.pi = pi; r.lcss = lcss;
    return r;
}

void bitsToSymbols(const Bits& bits, std::vector<int8_t>& sym) {
    sym.resize(bits.size() / 2);
    if (!sym.empty()) zorcom_dmr_bits_to_symbols(bits.data(), bits.size(), sym.data(), sym.size());
}

void packFullLc(const FullLc& lc, uint8_t out[9]) { const ZorcomDmrFullLc c = toC(lc); zorcom_dmr_pack_full_lc(&c, out); }
void parseFullLc(const uint8_t in[9], FullLc& lc) {
    ZorcomDmrFullLc c;
    zorcom_dmr_parse_full_lc(in, &c);
    memcpy(lc.raw, c.raw, 9);
    lc.pf = c.pf != 0; lc.flco = c.flco; lc.fid = c.fid; lc.svc = c.svc; lc.dst = c.dst; lc.src = c.src;
}
const char* flcoName(int f) { return cs(zorcom_dmr_flco_name(f)); }
std::string serviceOptionsText(uint8_t svc) {
    uint8_t b[64];
    const size_t n = zorcom_dmr_service_options_text(svc, b, sizeof b);
    return std::string((const char*)b, n == SIZE_MAX ? 0 : n);
}

void lcBurstInfo(const uint8_t lc[9], bool terminator, Bits& info96) {
    info96.assign(96, 0);
    zorcom_dmr_lc_burst_info(lc, terminator, info96.data());
}
int lcBurstDecode(const Bits& info96, bool terminator, uint8_t lc[9]) {
    uint8_t in[96];
    take(info96, in);
    return zorcom_dmr_lc_burst_decode(in, terminator, lc);
}

void crcBlockInfo(const uint8_t data10[10], uint16_t mask, Bits& info96) {
    info96.assign(96, 0);
    zorcom_dmr_crc_block_info(data10, mask, info96.data());
}
bool crcBlockCheck(const Bits& info96, uint16_t mask, uint8_t data10[10]) {
    uint8_t in[96];
    take(info96, in);
    return zorcom_dmr_crc_block_check(in, mask, data10);
}

void csbkInfo(const Csbk& c, Bits& info96) {
    ZorcomDmrCsbk k;
    memset(&k, 0, sizeof k);
    k.lb = c.lb; k.pf = c.pf; k.opcode = c.opcode; k.fid = c.fid;
    memcpy(k.data, c.data, 8);
    info96.assign(96, 0);
    zorcom_dmr_csbk_info(&k, info96.data());
}
bool csbkParse(const Bits& info96, Csbk& c) {
    uint8_t in[96];
    take(info96, in);
    ZorcomDmrCsbk k;
    if (!zorcom_dmr_csbk_parse(in, &k)) return false;
    c.lb = k.lb != 0; c.pf = k.pf != 0; c.opcode = k.opcode; c.fid = k.fid;
    memcpy(c.data, k.data, 8);
    return true;
}
const char* csbkName(int op, int fid) { return cs(zorcom_dmr_csbk_name(op, fid)); }

const char* dpfName(int d) { return cs(zorcom_dmr_dpf_name(d)); }
const char* sapName(int s) { return cs(zorcom_dmr_sap_name(s)); }

void packDataHeader(const DataHeader& h, uint8_t o[10]) { const ZorcomDmrDataHeader c = toC(h); zorcom_dmr_pack_data_header(&c, o); }
bool parseDataHeader(const uint8_t in[10], DataHeader& h) {
    ZorcomDmrDataHeader c;
    const bool ok = zorcom_dmr_parse_data_header(in, &c);
    h = DataHeader();
    h.dpf = c.dpf; h.sap = c.sap; h.group = c.group != 0; h.resp = c.resp != 0; h.dst = c.dst; h.src = c.src; h.blocks = c.blocks; h.pad = c.pad;
    h.fmf = c.fmf != 0; h.fsn = c.fsn; h.ns = c.ns; h.dd = c.dd; h.sp = c.sp; h.dp = c.dp; h.resync = c.resync != 0; h.sarq = c.sarq != 0; h.status = c.status;
    return ok;
}
int dataBlockBytes(int dt, bool confirmed) { return zorcom_dmr_data_block_bytes(dt, confirmed); }

void dataBlockEncode(int dt, const uint8_t* user, size_t n, bool confirmed, unsigned dbsn, Bits& payload196) {
    payload196.assign(196, 0);
    zorcom_dmr_data_block_encode(dt, user, n, confirmed, dbsn, payload196.data());
}
bool dataBlockParse(int dt, const uint8_t* oct, bool confirmed, DataBlock& out) {
    const size_t total = dt == kDtRate12 ? 12 : dt == kDtRate34 ? 18 : dt == kDtRate1 ? 24 : 0;   // the block's octets by data type
    ZorcomDmrDataBlock b;
    if (!total || !zorcom_dmr_data_block_parse(dt, oct, total, confirmed, &b)) return false;
    out.confirmed = confirmed;
    out.crcOk = b.crc_ok != 0;
    if (confirmed) out.dbsn = b.dbsn;
    out.bytes.assign(b.bytes, b.bytes + b.len);
    return true;
}

const char* ddName(int dd) { return cs(zorcom_dmr_dd_name(dd)); }
std::string textFromBytes(const uint8_t* p, size_t n, int format, bool ta) {
    std::string s(n * 2 + 96, '\0');
    const size_t r = zorcom_dmr_text_from_bytes(p, n, format, ta, (uint8_t*)&s[0], s.size());
    s.resize(r == SIZE_MAX ? 0 : r);
    return s;
}

void TalkerAlias::add(const FullLc& lc) {
    ZorcomDmrTa t = toC(*this);
    const ZorcomDmrFullLc l = toC(lc);
    zorcom_dmr_ta_add(&t, &l);
    format = t.format; length = t.length; have = t.have;
    memcpy(data, t.data, 28);
}
bool TalkerAlias::complete() const { const ZorcomDmrTa t = toC(*this); return zorcom_dmr_ta_complete(&t); }
std::string TalkerAlias::text() const {
    const ZorcomDmrTa t = toC(*this);
    uint8_t b[128];
    const size_t n = zorcom_dmr_ta_text(&t, b, sizeof b);
    return std::string((const char*)b, n == SIZE_MAX ? 0 : n);
}

} // namespace dmr
} // namespace dect2
