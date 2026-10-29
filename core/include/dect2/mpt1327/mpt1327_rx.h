// MPT 1327 control channel decoder: FM audio of the control channel in, codewords and messages out.
// 1200 bit/s FFSK is demodulated as minimum shift keying around 1500 Hz (tones 1200 and 1800 Hz, a phase step of a quarter turn per bit);
// bit timing is found by running a sync search on every sample phase. The bit polarity (which tone is which bit) is taken from the
// sync pattern: SYNC or its complement locks the stream, so the receiver works with either discriminator polarity. On a traffic channel the
// codewords follow SYNT (after its preamble): CLEAR and MAINT are decoded there too.
#pragma once
#include "dect2/mpt1327/mpt1327_proto.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2::mpt1327 {

constexpr double kBitRate = 1200.0;

struct Event {
    double t = 0;        // seconds of audio since reset
    Message msg;
    bool traffic = false;   // on a traffic channel (after SYNT)
};

struct Stats {
    uint64_t good = 0;       // codewords with good check bits (control channel system codewords and address / data codewords after a SYNC)
    uint64_t bad = 0;        // codewords after a SYNC whose check bits fail
    uint64_t ccsc = 0;       // control channel system codewords seen
    uint64_t messages = 0;   // address codewords decoded
    uint64_t synt = 0;       // traffic channel synchronisation sequences (SYNT) found
    bool french = false;     // the French variant's sync sequences were heard
    uint64_t trafficMessages = 0;   // address codewords after a SYNT
    int sys = -1;            // system identity from the last good CCSC
    bool locked = false;     // a good codeword within the last 3 s
    double t = 0;
};

class Decoder {
public:
    Decoder();
    ~Decoder();
    void reset();
    void process(const float* audio, size_t n, double fs);   // FM discriminator audio at fs Hz (any rate above about 6 kHz)
    const Stats& stats() const;
    std::vector<Event> takeEvents();                          // messages decoded since the last call
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace dect2::mpt1327
