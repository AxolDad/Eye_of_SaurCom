#include "dect2/acars/acars_proto.h"
#include <cstdio>
#include <cstring>

namespace dect2 {

// The transmitter side of the test signal. The block layer (CRC, repair, parsing, label and text decoding) is Rust: acars_proto_rs.cpp over rust/zorcom_acars.
std::vector<uint8_t> acarsBuildFrame(const AcarsBlockSpec& s, int preKeyChars) {
    std::vector<uint8_t> out;
    std::vector<uint8_t> blk;                              // what the check sequence covers
    blk.push_back(acarsWithParity((uint8_t)s.mode));
    std::string reg = s.reg.size() > 7 ? s.reg.substr(0, 7) : s.reg;
    reg.insert(0, 7 - reg.size(), '.');
    for (char c : reg) blk.push_back(acarsWithParity((uint8_t)c));
    blk.push_back(acarsWithParity(s.ack));
    blk.push_back(acarsWithParity((uint8_t)s.label[0]));
    blk.push_back(acarsWithParity(s.label[1] == 'd' && s.label[0] == '_' ? kAcarsDel : (uint8_t)s.label[1]));
    blk.push_back(acarsWithParity((uint8_t)s.blockId));
    if (s.hasText) {
        blk.push_back(kAcarsStx);
        for (size_t i = 0; i < s.text.size() && i < 220; i++) blk.push_back(acarsWithParity((uint8_t)s.text[i]));
    }
    blk.push_back(s.lastBlock ? kAcarsEtx : kAcarsEtb);
    const uint16_t crc = acarsCrc(blk.data(), blk.size());
    if (preKeyChars > 0) {
        for (int i = 0; i < preKeyChars; i++) out.push_back(0xff);
        out.push_back(acarsWithParity('+'));
        out.push_back(acarsWithParity('*'));
    }
    out.push_back(kAcarsSyn); out.push_back(kAcarsSyn); out.push_back(kAcarsSoh);
    out.insert(out.end(), blk.begin(), blk.end());
    out.push_back((uint8_t)(crc & 0xff)); out.push_back((uint8_t)(crc >> 8));
    out.push_back(kAcarsDel);
    return out;
}

} // namespace dect2
