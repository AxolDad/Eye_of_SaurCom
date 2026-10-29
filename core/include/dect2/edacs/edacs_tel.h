// EDACS telemetry. RxTelemetry::edacs, engine standard 132, active standard 131 (family 130).
// The control channel's messages (as data words and parsed) and, on a working channel, its FM voice, low-speed data and ProVoice detection.
// Receive only.
#pragma once
#include "dect2/dsp/analog_voice.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

struct EdacsFrame {
    int64_t wallTime = 0;
    double timeSec = 0;
    uint32_t msgA = 0, msgB = 0;      // the 28 data bits of the two messages, after the vote and the BCH correction
    int errA = 0, errB = 0;           // bit positions (of 40) on which the three copies of each message disagreed
    bool okA = false, okB = false;    // used (BCH good, or the copies agree exactly)
    bool bchA = false, bchB = false;  // the BCH check passed
    bool good = false;
};

struct EdacsWordCount {
    uint32_t word = 0;                // 28 data bits
    uint64_t count = 0;
};

// One message for the trunking follower: the sequence number is unique across receivers (the follower remembers the last one taken)
struct EdacsMsgRec {
    uint64_t seq = 0;
    uint32_t data = 0;
    bool bch = false;
    double timeSec = 0;
};

struct EdacsTelemetry {
    uint64_t seq = 0;
    int state = 0;                    // 0 no input, 1 listening, 2 a good frame in the last 5 s
    bool locked = false;
    double cfoHz = 0;                 // not measured (0)
    float snrDb = 0, levelDb = -200, deviationHz = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0, blocksBad = 0;   // frames with at least one message used / frames found by the sync with none
    uint64_t syncs = 0;                     // sync words found
    uint64_t bchOk = 0, copiesOnly = 0, msgBad = 0;   // messages: BCH good / used on the agreeing copies alone / refused
    bool inverted = false;                  // the data polarity found on the sync
    bool narrow = false;                    // EDACS narrowband: 4800 bit/s
    int sysId = -1, site = -1;              // from the system ID message
    double timeSec = 0, inputRate = 0, centerHz = 0, channelHz = 0;   // channelHz: the channel decoded (the radio's centre + the offset)
    std::vector<EdacsFrame> frames;         // newest first
    std::vector<EdacsWordCount> words;      // the most frequent data words, descending
    std::vector<EdacsMsgRec> msgs;          // the last messages used, oldest first, at most kEdacsMsgMax
    // working channel
    AnalogVoiceState voice;
    bool lsd = false;                       // the subaudible low-speed data is there
    bool provoice = false;                  // the channel carries 9600 bit/s digital voice (ProVoice), not FM speech
    float lsdHz = 0;                        // rms of the discriminator below 200 Hz
    float crossingCoherence = 0;            // how much the crossings keep to the 9600 bit/s clock (0..1)
};

constexpr size_t kEdacsLogMax = 400;
constexpr size_t kEdacsMsgMax = 96;

inline std::string edacsSummary(const EdacsTelemetry& t) {
    char b[160];
    if (t.state == 0) return "EDACS: waiting for samples";
    if (t.provoice) return "EDACS: ProVoice (digital voice) on this channel";
    if (t.voice.carrier && !t.locked) { snprintf(b, sizeof b, "EDACS: voice channel%s", t.lsd ? ", low-speed data" : ""); return b; }
    if (!t.dataValid) { snprintf(b, sizeof b, "EDACS: listening (%.0f dBFS)", t.levelDb); return b; }
    if (t.sysId >= 0) snprintf(b, sizeof b, "EDACS: %s, system %04X site %d, %llu frames", t.locked ? "locked" : "no lock", t.sysId, t.site, (unsigned long long)t.blocksOk);
    else snprintf(b, sizeof b, "EDACS: %s, %llu good frames", t.locked ? "locked" : "no lock", (unsigned long long)t.blocksOk);
    return b;
}

} // namespace dect2
