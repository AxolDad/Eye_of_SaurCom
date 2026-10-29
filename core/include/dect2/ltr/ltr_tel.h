// LTR (Logic Trunked Radio) telemetry. RxTelemetry::ltr, engine standard 131, active standard 130 (family 129).
// The 300 bit/s subaudible words (LTR standard / LTR-Net 40-bit words, or Passport 68-bit words: told apart by their checksums),
// and the repeater's FM voice (played when the trunking follower carries a call here). Receive only.
#pragma once
#include "dect2/dsp/analog_voice.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct LtrMessage {
    int64_t wallTime = 0;
    double timeSec = 0;
    bool passport = false;                                     // a Passport word (the fields below marked P); otherwise LTR
    int area = 0, gotoCh = 0, home = 0, id = 0, freeCh = 0;   // LTR: the 24 information bits (P: freeCh is the 11-bit free LCN)
    int dcc = 0, lcn = 0, site = 0, group = 0, type = 0;       // P
    int chk = 0;                                               // the check bits as received (they matched: only good words are listed)
    bool inbound = false;                                      // LTR: the inverted checksum matched (an inbound word from a mobile)
    std::string talkgroup;                                     // LTR A-HH-GGG; Passport the group number
    std::string kind;                                          // LTR: idle, call end, callsign ID, busy, token or empty; Passport: its message type
};

// One word for the trunking follower: the sequence number is unique across receivers (the follower remembers the last one taken)
struct LtrWordRec {
    uint64_t seq = 0;
    bool passport = false;
    int area = 0, gotoCh = 0, home = 0, id = 0, freeCh = 0;
    int dcc = 0, lcn = 0, site = 0, group = 0, type = 0;   // Passport
    double timeSec = 0;
};

struct LtrRepeater {
    int ch = 0;                    // repeater number 1 - 20
    uint64_t asGoto = 0, asHome = 0, asFree = 0;
    double lastSec = -1;
};

struct LtrTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 no input, 1 listening, 2 a good word in the last 15 s
    bool locked = false;
    double cfoHz = 0;              // not measured (0)
    float snrDb = 0, levelDb = -200, deviationHz = 0;   // deviationHz: peak-to-peak size of the subaudible data
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // words whose checksum matched / words with the sync whose checksum did not
    uint64_t syncs = 0;
    int area = -1;                 // area bit of the last good LTR word
    bool passport = false;         // the last good word was a Passport word
    int passportSite = -1, passportDcc = -1, passportBand = -1;   // from the Passport idle words: site, colour code, the site's band (-1 unknown)
    double timeSec = 0, inputRate = 0, centerHz = 0;
    std::vector<LtrMessage> messages;       // newest first
    std::vector<LtrRepeater> repeaters;     // numbers seen, ascending
    std::vector<LtrWordRec> words;          // the last good words, oldest first, at most kLtrWordMax
    uint64_t turnOffs = 0;                  // turn-off words (LTR go-to 31, Passport call end) heard: the end of a transmission
    int turnOffArea = -1, turnOffHome = -1, turnOffId = -1;   // the last one's talkgroup (LTR)
    int turnOffGroup = -1;                  // Passport: the group of the last call end word
    double channelHz = 0;                   // the channel decoded (the radio's centre + the offset)
    AnalogVoiceState voice;
};

constexpr size_t kLtrWordMax = 96;

constexpr size_t kLtrLogMax = 400;

inline std::string ltrSummary(const LtrTelemetry& t) {
    char b[160];
    if (t.state == 0) return "LTR: waiting for samples";
    if (!t.dataValid) { snprintf(b, sizeof b, "LTR: listening (%.0f dBFS)", t.levelDb); return b; }
    if (t.passport) snprintf(b, sizeof b, "Passport: %s, %llu words, site %d", t.locked ? "locked" : "no lock", (unsigned long long)t.blocksOk, t.passportSite);
    else snprintf(b, sizeof b, "LTR: %s, %llu words, %zu repeaters", t.locked ? "locked" : "no lock", (unsigned long long)t.blocksOk, t.repeaters.size());
    return b;
}

} // namespace dect2
