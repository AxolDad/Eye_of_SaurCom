// MPT 1327 control channel codewords, written from the public signalling standard (MPT 1327, 1997 issue, sections 3 and 5):
// 64-bit codewords (48 information bits + 15 check bits of a (63,48) cyclic code + 1 parity bit), the codeword
// synchronisation sequences and the address codeword layouts used on a control channel. Receive and decode only.
// Cross-checked against SDRTrunk (GPL-3, Dennis Sheirer), module/decode/mpt1327/MPT1327Message.java (field positions, message type
// numbers, IdentType) and identifier/MPT1327Talkgroup.java (special idents carry no prefix), and bits/SyncPattern.java for the French
// variant's SYNC / SYNT. MAINT's OPER field and the GTC call kinds are from the standard's sections 4 (Table 4-1), 5.4 and 5.5.4.2.
#pragma once
#include <cstdint>
#include <string>

namespace dect2::mpt1327 {

constexpr uint16_t kSync = 0xC4D7;   // control channel codeword synchronisation sequence SYNC, bit 1 first (section 3.2.1.1)
constexpr uint16_t kSynt = 0x3B28;   // traffic channel sequence SYNT (section 3.2.1.2): the codewords on a traffic channel (CLEAR, MAINT) follow it
constexpr uint16_t kSyncFrench = 0xB433;   // the French variant's SYNC (SDRTrunk SyncPattern MPT1327_CONTROL_FRENCH)
constexpr uint16_t kSyntFrench = 0x4BCC;   // and its SYNT (MPT1327_TRAFFIC_FRENCH): again the complement

// A codeword is held in a uint64_t with codeword bit 1 (the first one transmitted) as bit 63 and bit 64 as bit 0.
uint64_t encode(uint64_t info48);        // info48: codeword bits 1 .. 48 (bit 1 in bit 47); returns the 64-bit codeword with its check bits
bool valid(uint64_t cw);                 // the check bits and the parity agree with the information bits
uint16_t checkBits(uint64_t info48);     // codeword bits 49 .. 64

// Table 4-1 of the standard (SDRTrunk MPT1327Message.IdentType): idents 1 - 8100 are units or groups (the standard does not say which:
// that is the system's fleet plan); the rest are special idents, which have no prefix.
enum class Ident { Dummy, User, PstnGateway, PabxGateway, DataGateway, PstnShort, Spare, Reserved, Regi, Inci, Diverti, Sdmi, Ipfixi, Tsci, Alli };
Ident identType(int ident);
const char* identName(int ident);        // "ALLI", "PSTNGI" ... for a special ident, nullptr for a user ident
inline bool userIdent(int ident) { return ident >= 1 && ident <= 8100; }

struct Address {
    int pfix = -1;     // 7-bit prefix, -1 when the message carries no prefix
    int ident = -1;    // 13-bit ident, -1 when absent
};
std::string addressText(const Address& a);   // "prefix/ident", "ident N" without a prefix, "-" when absent

struct Message {
    std::string type;       // mnemonic: GTC, ALH, ACK, AHY, BCAST, ... (empty: not an address codeword)
    std::string detail;     // the function / parameters in words
    Address called;         // PFIX/IDENT1
    Address caller;         // IDENT2 (the standard gives it without a prefix of its own), pfix -1
    int channel = -1;       // channel number (GTC: the allocated channel; BCAST: the control channel announced), -1 when the message has none
    int sys = -1;           // system identity (BCAST), -1 when the message has none
    bool dataCall = false;  // GTC: a data call (the audio is muted), not speech
    int oper = -1;          // MAINT: OPER (0 pressel on, 1 pressel off, 2 periodic, 3 disconnect, 4 spare, 5 reserved, 6 clear down others, 7 disable)
    int cont = -1;          // CLEAR: the control channel to go back to (0: system-dependent)
    int sysdef = -1;        // BCAST: SYSDEF (0 announce control channel, 1 withdraw, 2 / 3 parameters, 4 adjacent site, 5 vote now)
    int chan4 = -1;         // ALH: the low 4 bits of this control channel's own number (section 5.5.1)
    uint64_t cw = 0;
};
// MAINT's OPER: 3 (disconnect, end of channel use) ends the call on the channel; 6 / 7 are the TSC's selective clear down and disable
constexpr int kOperDisconnect = 3;
const char* operText(int oper);

// What a GTC sets up, from its idents (section 5.4)
enum class CallKind { Normal, AllCall, ToPhone, FromPhone, Interprefix, Include, Intersite };
CallKind gtcKind(const Message& gtc);

// An address as one number for the talkgroup list: prefix * 8192 + ident (the 20-bit MPT 1327 address) for a user ident; a special ident
// alone, without its (meaningless) prefix, as SDRTrunk's MPT1327Talkgroup does
inline int addressNumber(const Address& a) { return a.ident < 0 ? -1 : userIdent(a.ident) ? (a.pfix < 0 ? 0 : a.pfix) * 8192 + a.ident : a.ident; }

uint32_t field(uint64_t cw, int firstBit, int count);   // codeword bits firstBit .. firstBit+count-1 (1-based) as a number, first bit most significant
bool decodeAddress(uint64_t cw, Message& m);             // false when bit 1 is 0 (a data codeword); the check bits are not tested here
bool decodeCcsc(uint64_t cw, int& sys);                  // the control channel system codeword: bit 1 = 0, SYS in bits 2 .. 16, final 16 bits = SYNC
                                                         // (or the French SYNC)

}  // namespace dect2::mpt1327
