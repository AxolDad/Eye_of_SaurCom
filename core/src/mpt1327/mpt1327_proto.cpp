// MPT 1327 codewords: check bits and the address codeword fields of the control channel messages.
#include "dect2/mpt1327/mpt1327_proto.h"
#include <cstdio>
#include <cstring>

namespace dect2::mpt1327 {

namespace {

// Generator polynomial X15 + X14 + X13 + X11 + X4 + X2 + 1 without the X15 term (section 3.2.3)
constexpr uint32_t kPoly = 0x6815;

uint32_t crc15(uint64_t info48) {
    // codeword bits 1 .. 48 are the coefficients of X62 .. X15: divide info * X15 by the generator
    uint32_t reg = 0;
    for (int i = 47; i >= 0; i--) {
        const uint32_t in = (uint32_t)((info48 >> i) & 1);
        const uint32_t fb = ((reg >> 14) & 1) ^ in;
        reg = (reg << 1) & 0x7FFF;
        if (fb) reg ^= kPoly;
    }
    return reg;
}

int parity64(uint64_t v) {
    v ^= v >> 32; v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (int)(v & 1);
}

const char* const kAloha[8] = {"ALH", "ALHS", "ALHD", "ALHE", "ALHR", "ALHX", "ALHF", "reserved"};
const char* const kAlohaText[8] = {"any single codeword message invited", "invited, except RQD", "invited, except RQS", "emergency requests only",
                                   "registration or emergency requests", "invited, except RQR", "fall-back mode", "reserved"};
const char* const kAck[8] = {"ACK", "ACKI", "ACKQ", "ACKX", "ACKV", "ACKE", "ACKT", "ACKB"};
const char* const kAckText[8] = {"acknowledgement", "intermediate acknowledgement", "call queued", "message rejected", "called unit unavailable",
                                 "emergency call acknowledged", "try on the given address", "call-back or negative acknowledgement"};
const char* const kAhoy[8] = {"AHY", "spare", "AHYX", "reserved", "reserved", "AHYP", "AHYQ", "AHYC"};
const char* const kAhoyText[8] = {"availability check", "spare (customisation)", "cancel alert / waiting state", "reserved", "reserved",
                                  "called unit presence check", "status message", "short data invitation"};
const char* const kMisc[8] = {"MARK", "MAINT", "CLEAR", "MOVE", "BCAST", "reserved", "reserved", "reserved"};
const char* const kMiscText[8] = {"control channel marker", "call maintenance", "clear down from allocated channel", "move to control channel",
                                  "system parameter broadcast", "reserved", "reserved", "reserved"};

}  // namespace

uint16_t checkBits(uint64_t info48) {
    const uint32_t c = crc15(info48) ^ 1u;                       // the final check bit of the (63,48) code is inverted
    const uint64_t w63 = (info48 << 15) | c;
    return (uint16_t)((c << 1) | (uint32_t)parity64(w63));       // even parity over the whole 64-bit codeword
}

uint64_t encode(uint64_t info48) {
    info48 &= 0xFFFFFFFFFFFFull;
    return (info48 << 16) | checkBits(info48);
}

bool valid(uint64_t cw) { return checkBits(cw >> 16) == (uint16_t)(cw & 0xFFFF); }

uint32_t field(uint64_t cw, int firstBit, int count) {
    const int shift = 64 - (firstBit - 1) - count;
    return (uint32_t)((cw >> shift) & ((1ull << count) - 1));
}

Ident identType(int i) {
    switch (i) {
    case 0: return Ident::Dummy;
    case 8101: return Ident::PstnGateway;
    case 8102: return Ident::PabxGateway;
    case 8103: return Ident::DataGateway;
    case 8185: return Ident::Regi;
    case 8186: return Ident::Inci;
    case 8187: return Ident::Diverti;
    case 8188: return Ident::Sdmi;
    case 8189: return Ident::Ipfixi;
    case 8190: return Ident::Tsci;
    case 8191: return Ident::Alli;
    default: break;
    }
    if (i >= 1 && i <= 8100) return Ident::User;
    if (i >= 8121 && i <= 8135) return Ident::PstnShort;
    if (i >= 8181 && i <= 8184) return Ident::Reserved;
    return Ident::Spare;   // 8104 - 8120 (reserved) and 8136 - 8180 (customisation)
}

const char* identName(int i) {
    static char buf[16];
    switch (identType(i)) {
    case Ident::User: return nullptr;
    case Ident::Dummy: return "DUMMYI";
    case Ident::PstnGateway: return "PSTNGI";
    case Ident::PabxGateway: return "PABXI";
    case Ident::DataGateway: return "DNI";
    case Ident::PstnShort: snprintf(buf, sizeof buf, "PSTNSI%d", i - 8120); return buf;
    case Ident::Regi: return "REGI";
    case Ident::Inci: return "INCI";
    case Ident::Diverti: return "DIVERTI";
    case Ident::Sdmi: return "SDMI";
    case Ident::Ipfixi: return "IPFIXI";
    case Ident::Tsci: return "TSCI";
    case Ident::Alli: return "ALLI";
    case Ident::Reserved: return "reserved";
    default: return "spare";
    }
}

const char* operText(int oper) {
    static const char* const t[8] = {"pressel on", "pressel off", "periodic message within an item", "disconnect: end of channel use",
                                     "spare (customisation)", "reserved", "clear down units not in this call", "disable user transmission"};
    return oper >= 0 && oper < 8 ? t[oper] : "";
}

CallKind gtcKind(const Message& m) {
    const Ident a = identType(m.called.ident), b = identType(m.caller.ident);
    auto phone = [](Ident x) { return x == Ident::PstnGateway || x == Ident::PabxGateway || x == Ident::PstnShort; };
    if (a == Ident::Alli) return CallKind::AllCall;
    if (phone(a)) return CallKind::ToPhone;
    if (phone(b)) return CallKind::FromPhone;
    if (a == Ident::Ipfixi || b == Ident::Ipfixi) return CallKind::Interprefix;
    if (b == Ident::Inci) return CallKind::Include;
    if (a == Ident::Dummy || a == Ident::Tsci || b == Ident::Dummy || b == Ident::Tsci) return CallKind::Intersite;
    return CallKind::Normal;
}

std::string addressText(const Address& a) {
    char b[40];
    if (a.ident < 0) return "-";
    if (const char* n = identName(a.ident)) return n;   // a special ident: no prefix
    if (a.pfix < 0) { snprintf(b, sizeof b, "ident %d", a.ident); return b; }
    snprintf(b, sizeof b, "%d/%d", a.pfix, a.ident);
    return b;
}

bool decodeCcsc(uint64_t cw, int& sys) {
    if (field(cw, 1, 1) != 0 || ((uint16_t)(cw & 0xFFFF) != kSync && (uint16_t)(cw & 0xFFFF) != kSyncFrench)) return false;
    sys = (int)field(cw, 2, 15);
    return true;
}

bool decodeAddress(uint64_t cw, Message& m) {
    m = Message();
    m.cw = cw;
    if (field(cw, 1, 1) != 1) return false;
    char b[160];
    const int pfix = (int)field(cw, 2, 7), ident1 = (int)field(cw, 9, 13);
    if (field(cw, 22, 1) == 0) {   // the go-to-channel structure (section 5.4): bit 22 is 0
        m.type = "GTC";
        m.called = {pfix, ident1};
        m.channel = (int)field(cw, 24, 10);
        const int ident2 = (int)field(cw, 34, 13);
        m.caller = {userIdent(ident2) ? pfix : -1, ident2};   // a common-prefix call: the caller has the same prefix (SDRTrunk createFrom)
        m.dataCall = field(cw, 23, 1) != 0;
        static const char* const kind[7] = {"", ", system-wide call", ", call to a telephone", ", call from a telephone", ", interprefix call",
                                            ", include call", ", intersite call"};
        snprintf(b, sizeof b, "go to channel %d, %s%s", m.channel, field(cw, 23, 1) ? "data (audio muted)" : "speech", kind[(int)gtcKind(m)]);
        m.detail = b;
        return true;
    }
    const uint32_t cat = field(cw, 23, 3);
    if (cat == 0) {
        const uint32_t type = field(cw, 26, 2), func = field(cw, 28, 3);
        if (type == 3) {   // miscellaneous: bits 2 .. 21 are not an address for MARK / BCAST
            m.type = kMisc[func];
            m.detail = kMiscText[func];
            if (func == 4) {   // BCAST: SYSDEF in bits 2 .. 6, SYS in bits 7 .. 21 (section 5.5.4.5)
                const uint32_t sysdef = field(cw, 2, 5);
                m.sys = (int)field(cw, 7, 15);
                if (sysdef == 0 || sysdef == 1 || sysdef == 4) {
                    m.channel = (int)field(cw, 31, 10);
                    snprintf(b, sizeof b, "%s, system %d, control channel %d", sysdef == 0 ? "control channel announced" : sysdef == 1 ? "control channel withdrawn" : "adjacent site control channel", m.sys, m.channel);
                    if (sysdef == 4) { const int adj = (int)field(cw, 45, 4); if (adj) snprintf(b + strlen(b), sizeof b - strlen(b), ", site %d", adj); }
                    m.detail = b;
                } else if (sysdef == 2) m.detail = "call maintenance parameters";
                else if (sysdef == 3) m.detail = "registration parameters";
                else if (sysdef == 5) m.detail = "vote now advice";
            } else if (func == 1) {   // MAINT (5.5.4.2): PFIX/IDENT1, CHAN in bits 31 .. 40, OPER in bits 41 .. 43
                m.called = {pfix, ident1};
                m.channel = (int)field(cw, 31, 10);
                m.oper = (int)field(cw, 41, 3);
                snprintf(b, sizeof b, "call maintenance on channel %d: %s", m.channel, operText(m.oper));
                m.detail = b;
            } else if (func == 2) {   // CLEAR (5.5.4.3): no address; CHAN in bits 2 .. 11, CONT in bits 12 .. 21
                m.channel = (int)field(cw, 2, 10);
                m.cont = (int)field(cw, 12, 10);
                snprintf(b, sizeof b, "clear down from channel %d, back to control channel %d", m.channel, m.cont);
                m.detail = b;
            } else if (func != 0) {
                m.called = {pfix, ident1};   // MOVE: PFIX/IDENT1
            }
            return true;
        }
        m.called = {pfix, ident1};
        if (type == 0) {
            m.type = kAloha[func];
            snprintf(b, sizeof b, "%s, control channel (low 4 bits) %u", kAlohaText[func], (unsigned)field(cw, 31, 4));
            m.detail = b;
        } else if (type == 1) {
            m.type = kAck[func];
            m.caller = {-1, (int)field(cw, 31, 13)};
            m.detail = kAckText[func];
        } else {
            m.type = kAhoy[func];
            m.detail = kAhoyText[func];
        }
        return true;
    }
    m.called = {pfix, ident1};
    if (cat == 1) {
        m.type = field(cw, 26, 1) ? "HEAD" : "SAM";
        m.detail = field(cw, 26, 1) ? "short data message header" : "single address message";
    } else if (cat == 2) {
        m.type = "STDDATA";
        m.detail = "standard data call set-up";
    } else if (cat == 5) {
        m.type = "STDACK";
        m.detail = "standard data acknowledgement or selective acknowledgement";
    } else {
        snprintf(b, sizeof b, "category %u (reserved or spare)", (unsigned)cat);
        m.type = "RSVD";
        m.detail = b;
    }
    return true;
}

}  // namespace dect2::mpt1327
