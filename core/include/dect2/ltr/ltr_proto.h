// LTR and Passport word formats and the test-signal generator.
//  - LTR word (standard and LTR-Net share it): 40 bits, MSB first: 9-bit sync 0x158, AREA 1, GO-TO 5, HOME 5, GROUP 8, FREE 5, then a
//    7-bit checksum. Field positions, the checksum table and the message types (idle when go-to == free and group 255, call end on go-to
//    31) are from SDRTrunk (GPL-3, Dennis Sheirer): module/decode/ltrstandard/LTRStandardMessageProcessor.java, message/LTRMessage.java,
//    edac/CRCLTR.java. The inbound status word (ISW, sent by mobiles) is the whole word inverted with a normal or inverted checksum.
//  - Passport word: 68 bits: sync 0x158, DCC 2, LCN 11, SITE 7, GROUP 16 (RADIO ID 23 = SITE + GROUP), TYPE 4, FREE 11, checksum 8
//    (bits 9 - 59 XORed through a table; a residual of 0 passes). Fields, table, message types and band plan from SDRTrunk:
//    module/decode/passport/PassportMessage.java, PassportBand.java, edac/CRCPassport.java.
//  - 300 bit/s subaudible NRZ below 150 Hz; talkgroup text A-HH-GGG, ids 240 - 247 tokens, 253 busy, 254 callsign ID, 255 idle.
// Polarity of the FM data is not known in advance: both are accepted.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::ltr {

constexpr double kBaud = 300.0;
constexpr uint32_t kSync = 0x158;   // 9 bits
constexpr int kWordBits = 40;

struct Word {
    int area = 0, gotoCh = 0, home = 1, id = 255, freeCh = 31, chk = 0;
};

int checksum(const Word& w);                                           // the 7-bit checksum of the 24 information bits (SDRTrunk CRCLTR)
std::vector<uint8_t> encodeWord(const Word& w, bool isw = false);       // 40 bits, MSB first, with the real checksum; isw: inverted (mobile)
bool parseWord(const uint8_t* bits40, Word& w);                        // false when the sync is not 0x158; chk as received
// 0 the checksum fails, 1 it matches, 2 it matches inverted (allowed in an inbound word: SDRTrunk accepts both there)
int checkWord(const Word& w);
bool plausible(const Word& w);                                         // home 1-20, go-to 0-20 or 29-31, free 0-20 or 31
std::string talkgroup(const Word& w);                                  // "A-HH-GGG"
std::string kindOf(int id);                                            // "idle", "callsign ID", "busy", "token" or ""
// A talkgroup as one number for the talkgroup list: area * 100000 + home * 1000 + id (0-05-123 -> 5123, 1-05-123 -> 105123)
inline int tgNumber(int area, int home, int id) { return area * 100000 + home * 1000 + id; }
std::string tgText(int number);                                        // the number back as "A-HH-GGG"
constexpr int kTurnOff = 31;                                           // go-to 31: the turn-off word sent as a transmission ends
inline bool isCallId(int id) { return id >= 1 && id <= 239; }          // ids 240 and up are tokens, busy, callsign and idle

// ---- Passport (68-bit words on the same 300 bit/s subaudible channel)
constexpr int kPassportBits = 68;
struct PassportWord {
    int dcc = 0, lcn = 0, site = 0, group = 0, type = 0, freeCh = 0, chk = 0;
    int radioId() const { return (site << 16) | group; }   // bits 22 - 44
    int siteBand() const { return group & 0xF; }            // bits 41 - 44, in an idle word
    int neighborBand() const { return (group >> 8) & 0xF; } // bits 33 - 36, in an idle word
};
enum class PassportKind { Unknown, CallStart, TgAssign, Idle, CallEnd, Page, RadioId, Data, Register };
int passportChecksum(const PassportWord& p);                           // the 8-bit checksum of bits 9 - 59
std::vector<uint8_t> encodePassport(const PassportWord& p);            // 68 bits with the real checksum
bool parsePassport(const uint8_t* bits68, PassportWord& p);            // false when the sync is not 0x158; chk as received
bool passportOk(const PassportWord& p);                                 // the residual is 0, or an error in the last two check bits (SDRTrunk)
PassportKind passportKind(const PassportWord& p);
const char* passportKindName(PassportKind k);
double passportBandBaseHz(int band);                                    // SDRTrunk PassportBand: 0 when the band has no base
inline double passportFreqHz(int band, int lcn) { const double b = passportBandBaseHz(band); return b > 0 && lcn > 0 && lcn < 1792 ? b + lcn * 12500.0 : 0; }
const char* passportBandName(int band);

// Test signal: FM at fs with the subaudible data (bit 1 = higher frequency) of the bits, optionally with a voice-band tone and noise.
// devHz: deviation of the data; voiceHz/voiceDevHz: a tone riding on top (0 = none); the carrier sits offsetHz from 0 Hz.
std::vector<cf32> modulate(const std::vector<uint8_t>& bits, double fs, double offsetHz, double devHz, float amp, double voiceHz = 0, double voiceDevHz = 0, bool invert = false);
// A repeater's transmission for a call: the words for (area, home, id) on repeater `rep` repeated with the voice tone, then `turnOff` turn-off
// words (go-to 31) and the carrier off
std::vector<cf32> transmission(double fs, double offsetHz, int area, int rep, int home, int id, double voiceSec, double toneHz, int turnOff, float amp);

} // namespace dect2::ltr
