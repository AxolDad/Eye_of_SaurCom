// EDACS control channel (outbound): frame, error handling and messages. Receive only.
//
// The message sets are taken from DSD-FME's EDACS parser (lwvmobile/dsd-fme, src/edacs-fme.c, its standard parsing rewritten after
// TIA/EIA TSB-69.3 and the reverse-engineered extended addressing (EA) messages; constants from include/dsd.h). Those parts are under
// DSD-FME's ISC licence ("all other code" in its COPYRIGHT; notice in LICENSES/ISC-DSD.txt). This file is a rewrite in our own code of the
// field layouts and constants, not a copy. DSD-FME's src/edacs-bch3.c (Morelos-Zaragoza's BCH code, non-commercial licence) and the
// ledacs portions were consulted for the code parameters only and nothing of them is used: the BCH encoder and decoder here are an
// independent textbook implementation (Lin and Costello, cyclotomic-coset generator, syndrome table). OP25 and SDRTrunk have no EDACS
// decoder.
//  - 9600 bit/s GFSK (EDACS narrowband: 4800 bit/s); a 288-bit frame = the 48-bit sync (dsd.h EDACS_SYNC) and two messages.
//  - A message is 28 data bits and 12 BCH check bits (40 bits), sent three times with the middle copy inverted: voted bit by bit, then
//    checked with BCH(40,28): the shortened (63,51) code over GF(2^6) from x^6 + x + 1, t = 2, data in the high 28 bits, the remainder in
//    the low 12 (the parameters DSD-FME uses). DSD-FME only checks it; this decoder also corrects up to two bits.
//  - The two messages of a frame form one command (msg_1, msg_2). Standard and networked systems use the TSB-69.3 set (MT-A / MT-B / MT-D),
//    EA systems the MT1 / MT2 set; which one a system uses is a setting, as in DSD-FME (its auto detection was removed as unreliable).
//    Some systems XOR the command bits with an ESK (0xA0 << 20).
//  - LCN 1..25 are channels; 26..31 in a channel field are status values (28 convert to callee, 29 queued, 30 busy, 31 denied).
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::edacs {

constexpr double kBaud = 9600.0, kBaudNarrow = 4800.0;
constexpr int kSyncBits = 48, kMsgBits = 40, kFrameBits = 288;
constexpr uint64_t kSync = 0x555557125555ull;

// ------------------------------------------------------------------------------------------------ BCH (40,28)
uint32_t bchGenerator();                             // the BCH(40,28) generator x^12 + x^10 + x^8 + x^5 + x^4 + x^3 + 1 (0x1539, bit 12 the top)
uint32_t bchCheck(uint32_t data28);                  // the 12 check bits
uint64_t bchEncode(uint32_t data28);                 // data << 12 | check
bool bchDecode(uint64_t& word40, int* corrected = nullptr);   // corrects up to two bit errors in place; false beyond

// ------------------------------------------------------------------------------------------------ frame
std::vector<uint8_t> encodeFrame(uint32_t msgA, uint32_t msgB);   // the sync, then each message three times, the middle copy inverted
uint64_t vote(const uint8_t* bits120, int& disagreements);

struct Decoded {
    uint32_t data = 0;           // 28 bits
    bool ok = false;             // the BCH check passed (after at most two corrections)
    bool bchOk = false;
    int corrected = 0, disagree = 0;
};
Decoded decodeMessage(const uint8_t* bits120);

// ------------------------------------------------------------------------------------------------ messages (dsd-fme edacs-fme.c)
enum class Mode { Standard, Extended };   // Standard: standard and networked (TSB-69.3); Extended: EA
constexpr uint32_t kEsk = 0xA0;            // the ESK value dsd-fme knows (-fH / -fE)

struct Message {
    enum Kind { Unknown, GroupVoice, IndivVoice, AllCall, DataCall, Interconnect, ChannelUpdate, UnkeyDrop, SiteId, SystemInfo, TestCall, Patch } kind = Unknown;
    int lcn = -1;                // the working channel (1..25; 26..31 status values, see lcnStatus)
    int group = -1;              // standard: 11-bit AFS; EA: 16-bit group
    int target = -1;             // individual calls: the called radio (standard 14-bit LID, EA 20-bit)
    int source = -1;             // the calling radio, when the message has it
    bool digital = false;        // ProVoice
    bool emergency = false, update = false, individual = false, drop = false;
    int sysId = -1, site = -1, area = -1, ccLcn = -1;
    uint32_t m1 = 0, m2 = 0;     // the two 28-bit messages (after the ESK)
};
Message parse(uint32_t msg1, uint32_t msg2, Mode mode, uint32_t esk = 0);
// the two messages that carry m (the builders the tests use; the field layout is parse's)
void build(const Message& m, Mode mode, uint32_t& msg1, uint32_t& msg2, uint32_t esk = 0);
std::string describe(const Message& m, Mode mode);
const char* lcnStatus(int lcn);   // "" for a channel

struct AfsSplit { int agencyBits = 4, fleetBits = 4, subfleetBits = 3; };   // dsd-fme's default 4:4:3
std::string afsText(int group, const AfsSplit& s = AfsSplit());             // "AA-FFS", e.g. "05-045"

// ------------------------------------------------------------------------------------------------ test signals
std::vector<cf32> modulate(const std::vector<uint8_t>& bits, double fs, double offsetHz, double devHz, float amp, bool invert = false, double baud = kBaud);
// A working (voice) channel: FM of a voice tone with the subaudible low-speed data under it (150 bit/s), then the end of transmission
// (dsd-fme DOTTING_SEQUENCE: dotting) for dropBits bits and the carrier off. provoice: 9600 bit/s random data instead of the voice tone.
std::vector<cf32> workingChannel(double fs, double offsetHz, double voiceSec, double toneHz, int dropBits, float amp, bool provoice = false, uint32_t seed = 1);

} // namespace dect2::edacs
