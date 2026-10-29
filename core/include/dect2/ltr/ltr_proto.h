// LTR word format and test-signal generator, clean-room from public descriptions:
//  - Word: 40 bits, MSB first: 9-bit sync 0x158 (a "10" start and a 7-bit Barker code), then 24 information bits
//    AREA 1, GO-TO 5, HOME 5, ID 8, FREE 5, then 7 check bits (US 6,529,486 B1 describes this word for an LTR-compatible system).
//  - 300 bit/s subaudible NRZ below 150 Hz (same patent's background section).
//  - Talkgroup text A-HH-GGG, special ids 240-247 tokens, 253 busy, 254 callsign ID, 255 idle (RadioReference wiki).
// NOT established from non-code public sources, so left out: the check-bit algorithm (the received check bits are shown, never verified),
// and the meaning of the GO-TO / FREE codes beyond the 1 - 20 repeater numbers. Polarity of the FM data is not known: both are accepted.
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

std::vector<uint8_t> encodeWord(const Word& w);                       // 40 bits, MSB first; chk is written as given
bool parseWord(const uint8_t* bits40, Word& w);                        // false when the sync is not 0x158
bool plausible(const Word& w);                                         // home 1-20, go-to 0-20 or 29-31, free 0-20 or 31
std::string talkgroup(const Word& w);                                  // "A-HH-GGG"
std::string kindOf(int id);                                            // "idle", "callsign ID", "busy", "token" or ""
// A talkgroup as one number for the talkgroup list: area * 100000 + home * 1000 + id (0-05-123 -> 5123, 1-05-123 -> 105123)
inline int tgNumber(int area, int home, int id) { return area * 100000 + home * 1000 + id; }
std::string tgText(int number);                                        // the number back as "A-HH-GGG"
constexpr int kTurnOff = 31;                                           // go-to 31: the turn-off word sent as a transmission ends
inline bool isCallId(int id) { return id >= 1 && id <= 239; }          // ids 240 and up are tokens, busy, callsign and idle

// Test signal: FM at fs with the subaudible data (bit 1 = higher frequency) of the bits, optionally with a voice-band tone and noise.
// devHz: deviation of the data; voiceHz/voiceDevHz: a tone riding on top (0 = none); the carrier sits offsetHz from 0 Hz.
std::vector<cf32> modulate(const std::vector<uint8_t>& bits, double fs, double offsetHz, double devHz, float amp, double voiceHz = 0, double voiceDevHz = 0, bool invert = false);
// A repeater's transmission for a call: the words for (area, home, id) on repeater `rep` repeated with the voice tone, then `turnOff` turn-off
// words (go-to 31) and the carrier off
std::vector<cf32> transmission(double fs, double offsetHz, int area, int rep, int home, int id, double voiceSec, double toneHz, int turnOff, float amp);

} // namespace dect2::ltr
