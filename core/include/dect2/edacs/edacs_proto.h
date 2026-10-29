// EDACS control channel (outbound): frame, error handling and messages. Receive only.
//
// From the public descriptions (US 4,905,302 and US 7,546,135 B2 describe the outbound control channel; the frame and code facts below are
// the ones those and other public descriptions agree on):
//  - 9600 bit/s GFSK (EDACS narrowband: 4800 bit/s, the same frame); a 30 ms frame of 288 bits = the 48-bit sync 0x555557125555 (16 dotting
//    bits, five more dotting bits and the 11-bit Barker word 11100010010, 16 dotting bits) and two messages.
//  - A message is 28 data bits and 12 BCH check bits (40 bits), sent three times with the middle copy inverted: a receiver votes bit by bit
//    over the three copies, then checks (and corrects) with the BCH code.
//  - The BCH code: the (63,51) primitive BCH code that corrects two errors, shortened to (40,28). Its generator is the standard one,
//    g(x) = m1(x) m3(x) = x^12 + x^10 + x^8 + x^5 + x^4 + x^3 + 1 (octal 12471) with m1 = x^6 + x + 1, m3 = x^6 + x^4 + x^2 + x + 1.
//  - Logical channel numbers (LCN, 1..31 here) name the channels; their frequencies are the system's own: the user supplies the table.
//  - Talkgroups are 11-bit agency / fleet / subfleet (AFS) numbers, individual radios 14-bit logical IDs (LID).
//
// The message type codes and field positions inside the 28 data bits are NOT verified against a live system here: they are kept in
// `Commands` (one table) and the layout below, so they can be checked and corrected in one place:
//   group voice assignment / update   cmd 8 | LCN 5 | digital (ProVoice) 1 | emergency 1 | reserved 2 | group 11
//   individual voice assignment       cmd 8 | LCN 5 | digital 1 | target LID 14
//   caller (message B of the frame)   cmd 8 | reserved 6 | caller LID 14
//   system ID                         cmd 8 | site 4 | system 16
//   idle                              cmd 8 | (anything)
// A frame whose three copies agree exactly is used even when the BCH check fails (counted apart): the copies are the code that is certain,
// so a deviation of a real system from the generator above shows in the counts instead of silencing the decoder.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::edacs {

constexpr double kBaud = 9600.0, kBaudNarrow = 4800.0;
constexpr int kSyncBits = 48, kMsgBits = 40, kFrameBits = 288;
constexpr uint64_t kSync = 0x555557125555ull;
constexpr uint32_t kBchGen = 0x1539;   // x^12 + x^10 + x^8 + x^5 + x^4 + x^3 + 1

// ------------------------------------------------------------------------------------------------ BCH (40,28)
uint32_t bchCheck(uint32_t data28);                  // the 12 check bits
uint64_t bchEncode(uint32_t data28);                 // data << 12 | check
bool bchDecode(uint64_t& word40, int* corrected = nullptr);   // corrects up to two bit errors in place; false beyond

// ------------------------------------------------------------------------------------------------ frame
// One frame, as bits (1 = higher frequency): the sync, then message A three times (middle inverted), message B the same. Real check bits.
std::vector<uint8_t> encodeFrame(uint32_t msgA, uint32_t msgB);
// Majority vote of a received message: 120 bits (copy, inverted copy, copy) -> 40 voted bits (MSB first); disagreements: bit positions where
// the copies differ
uint64_t vote(const uint8_t* bits120, int& disagreements);

struct Decoded {
    uint32_t data = 0;           // 28 bits
    bool ok = false;             // used: BCH good (after at most two corrections), or the three copies agree exactly
    bool bchOk = false;          // the BCH check passed
    int corrected = 0, disagree = 0;
};
Decoded decodeMessage(const uint8_t* bits120);

// ------------------------------------------------------------------------------------------------ messages
struct Commands {   // the message type codes (see the header comment)
    int groupVoice = 0xEE, groupUpdate = 0xEC, indivVoice = 0xEA, caller = 0xE9, systemId = 0xFD, idle = 0xFC;
};
struct Message {
    enum Kind { Unknown, GroupVoice, IndivVoice, Caller, SystemId, Idle } kind = Unknown;
    int cmd = 0;
    int lcn = -1;
    int group = -1, lid = -1;    // group (AFS) for group calls; target LID for individual calls, the caller's LID for Caller
    bool digital = false, emergency = false, update = false;
    int sysId = -1, site = -1;
    uint32_t raw = 0;
};
Message parse(uint32_t data28, const Commands& c = Commands());
uint32_t build(const Message& m, const Commands& c = Commands());
std::string describe(const Message& m);

struct AfsSplit { int agencyBits = 4, fleetBits = 4, subfleetBits = 3; };   // the split many systems use (the bits add up to 11)
std::string afsText(int group, const AfsSplit& s = AfsSplit());             // "A-FFS" as agency-fleet+subfleet, e.g. "02-071"

// ------------------------------------------------------------------------------------------------ test signals
// FM test signal at fs (bit 1 = +devHz) for the bits at `baud`, carrier offsetHz from 0 Hz; invert flips the polarity
std::vector<cf32> modulate(const std::vector<uint8_t>& bits, double fs, double offsetHz, double devHz, float amp, bool invert = false, double baud = kBaud);
// A working (voice) channel: FM of a voice tone with the subaudible low-speed data under it (150 bit/s), then at the end `dropFrames`
// frames of high-speed data (the unkey / drop messages) and the carrier off. provoice: 9600 bit/s random data instead of the voice tone
// (what a ProVoice channel looks like to an FM receiver).
std::vector<cf32> workingChannel(double fs, double offsetHz, double voiceSec, double toneHz, int dropFrames, float amp, bool provoice = false, uint32_t seed = 1);

} // namespace dect2::edacs
