// TETRA network information receiver: the TETRA base stations around you, from what every base station broadcasts in the clear to all
// terminals on its downlink (ETSI EN 300 392-2): the synchronisation channel (BSCH: colour code, MCC, MNC, frame numbers, sharing mode),
// the broadcast network channel (BNCH SYSINFO: main carrier, band, duplex spacing, location area, cell services and security
// parameters), the access assignment channel (what each timeslot carries) and the neighbour cell broadcasts on the main control channel.
// Receive only. Nothing is decrypted and nothing addressed to a terminal is decoded: no short data, no subscriber identities. Voice: only unencrypted traffic bursts, as coded frames to a user-installed codec (tetra_voice.h); encrypted or unknown is never decoded.
// Signalling sent with air interface encryption is reported as encrypted and counted, never read. The user is responsible for following
// the rules on receiving radio signals that apply where they are.
//
// The input is taken in blocks of 2.3 s (more than two multiframes, so every carrier's synchronisation burst is in it twice). Each block's
// spectrum is searched for 25 kHz carriers; each one is brought to 4 samples per symbol, matched filtered (root raised cosine 0.35),
// frequency corrected (fourth power of the differential symbols), slot synchronised on the training sequences, and its broadcast
// channels decoded. A block is captured only while the previous one is not being searched, so a slow computer just searches less often.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/tetra/tetra_voice.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "tetra_pdu.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {
using tetra::SpeechFrameSink;   // tetra_voice.h declares it in dect2::tetra

struct TetraTelemetry;   // tetra_tel.h

class TetraReceiver {
public:
    TetraReceiver();
    ~TetraReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);   // where the user's (dial) frequency sits relative to 0 Hz in the input
    void setCenterHz(double hz);       // the radio's centre frequency (0: unknown, for files)
    bool ready() const;                // false when the input rate is too low for this mode
    void reset();                      // after a retune: drop the block being captured (the cell table is kept)
    void clearCells();                 // the user's Clear button
    void feed(const cf32* x, size_t n);
    bool telemetry(TetraTelemetry& out, uint64_t lastSeq);
    // The cell table and the event log stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);
    // Where unencrypted traffic bursts go (tetra_voice_codec.h). The sink must outlive the receiver; null: voice frames are dropped.
    void setVoiceSink(SpeechFrameSink* s);
    bool busy() const;                 // a block is captured or being searched (tools: feed a file only as fast as it is searched)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning tetraTuning();

namespace tetra {

// One carrier brought to 72 kHz (4 samples per symbol): x at fs mixed down by shiftHz and filtered to the TETRA channel
std::vector<cf32> channelize(const cf32* x, size_t n, double fs, double shiftHz);

// What one carrier's capture held
struct CarrierDecode {
    bool tetra = false;        // slot synchronised on the training sequences
    bool synced = false;       // a synchronisation burst (BSCH) decoded
    Sync sync;                 // the last one
    int syncs = 0;
    uint32_t scrambler = 0;    // the cell's scrambling code used for the other channels (0: unknown)
    double cfoHz = 0;          // the frequency error found, relative to the carrier's assumed centre
    float snrDb = -99;
    int slots = 0, trafficSlots = 0, blocksOk = 0, blocksBad = 0;
    int voicePassed = 0, voiceNotDecoded = 0;   // traffic bursts handed to the speech frame sink / refused (encrypted or state unknown)
    const char* voiceStatus = "";                // text of the last traffic burst verdict (tetra_voice.h)
    int voiceTn = 0;                             // the timeslot whose clear traffic reached the sink (0: none)
    std::vector<MacBlock> macs;
    int tsUsage[4] = {-1, -1, -1, -1};
};
// knownMcc etc.: the identity of a cell already heard on this carrier, used when this capture has no synchronisation burst (-1: none)
// calls: the per-call encryption state of this carrier, kept across captures by the caller (null: no call state, so no traffic burst is ever decoded);
// nowSec: the clock of its expiry (any monotonic seconds).
// voiceTn: the timeslot (1..4) whose clear traffic goes to the sink; -1 the first timeslot with clear traffic, 0 every one.
CarrierDecode decodeCarrier(const cf32* x72k, size_t n, int knownMcc = -1, int knownMnc = -1, int knownCc = -1,
                              SpeechFrameSink* sink = nullptr, CallTracker* calls = nullptr, double nowSec = 0, int voiceTn = 0);

} // namespace tetra

} // namespace dect2
