// NXDN common air interface on top of the Rust crate rust/zorcom_nxdn: the functions of nxdn_proto.h
// (nxdn_proto.cpp keeps the frame sync word table).
#include "dect2/nxdn/nxdn_proto.h"
#include "dect2/common/rs_abi.h"
#include "dect2/rs/nxdn.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace nxdn {

namespace {
struct AbiCheck {
    AbiCheck() { dect2::rsAbiCheck("zorcom_nxdn", {zorcom_nxdn_struct_size()}, {sizeof(ZorcomNxdnMsg)}); }
} abiCheck;
inline size_t sz(int n) { return n < 0 ? 0 : (size_t)n; }
} // namespace

void pn9(uint8_t* out, int n) { zorcom_nxdn_pn9(out, sz(n)); }

uint8_t lichEncode(const Lich& l) { return zorcom_nxdn_lich_encode(l.rfct, l.fct, l.opt, l.dir); }
bool lichDecode(uint8_t x, Lich& l) {
    int32_t o[4];
    if (!zorcom_nxdn_lich_decode(x, o)) return false;
    l.rfct = o[0]; l.fct = o[1]; l.opt = o[2]; l.dir = o[3];
    return true;
}

uint32_t crc6(const uint8_t* b, int n) { return zorcom_nxdn_crc6(b, sz(n)); }
uint32_t crc12(const uint8_t* b, int n) { return zorcom_nxdn_crc12(b, sz(n)); }
uint32_t crc16(const uint8_t* b, int n) { return zorcom_nxdn_crc16(b, sz(n)); }

int infoBits(Ch c) { return (int)zorcom_nxdn_info_bits((int)c); }
int airBits(Ch c) { return (int)zorcom_nxdn_air_bits((int)c); }

void encode(Ch c, const uint8_t* info, uint8_t* air) { zorcom_nxdn_encode((int)c, info, sz(infoBits(c)), air, sz(airBits(c))); }

bool decode(Ch c, const float* soft, uint8_t* info, int* errs) {
    int32_t e = 0;
    const int r = zorcom_nxdn_decode((int)c, soft, sz(airBits(c)), info, sz(infoBits(c)), &e);
    if (r == 1 && errs) *errs = e;
    return r == 1;
}

int buildMsg(const Msg& m, uint8_t* bits, int maxBits) {
    ZorcomNxdnMsg c;
    memset(&c, 0, sizeof c);
    c.type = m.type; c.call_type = m.callType; c.option = m.option; c.src = m.src; c.dst = m.dst; c.cipher = m.cipher; c.key_id = m.keyId;
    c.status = m.status; c.block = m.block; c.blocks = m.blocks; c.location = m.location; c.service = m.service; c.channel = m.channel;
    c.cc_option = m.ccOption; c.timer = m.timer;
    c.text_len = (uint32_t)(m.text.size() < 8 ? m.text.size() : 8);
    memcpy(c.text, m.text.data(), c.text_len);
    return (int)zorcom_nxdn_build_msg(&c, bits, sz(maxBits));
}

bool parseMsg(const uint8_t* bits, int nBits, Msg& m) {
    ZorcomNxdnMsg c;
    const bool ok = zorcom_nxdn_parse_msg(bits, sz(nBits), &c);
    m = Msg();
    m.type = c.type; m.callType = c.call_type; m.option = c.option; m.src = c.src; m.dst = c.dst; m.cipher = c.cipher; m.keyId = c.key_id;
    m.status = c.status; m.block = c.block; m.blocks = c.blocks; m.location = c.location; m.service = c.service; m.channel = c.channel;
    m.ccOption = c.cc_option; m.timer = c.timer;
    m.text.assign((const char*)c.text, c.text_len);
    return ok;
}

const char* msgName(int type) { return (const char*)zorcom_nxdn_msg_name(type); }
const char* callTypeName(int t) { return (const char*)zorcom_nxdn_call_type_name(t); }

void ambeFrame(const uint8_t bits[72], char fr[4][24]) { zorcom_nxdn_ambe_frame(bits, 72, (uint8_t*)fr, 96); }

} // namespace nxdn
} // namespace dect2
