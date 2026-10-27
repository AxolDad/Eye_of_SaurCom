// KAWS test signal for the engine's synthetic source (see kaws_gen.h).
#include "dect2/kaws/kaws_gen.h"
#include "dect2/kaws/kaws_txgen.h"
#include "dect2/engine/synth_loop.h"
#include "dect2/kaws/kaws_live.h"
#include <chrono>

namespace dect2 {

std::unique_ptr<ModeSynth> makeKawsSynth(const SynthConfig& s, double sampleRate) {
    if (sampleRate < 250000 - 1) return nullptr;
    int profile = s.modeOpt[0] > 0 ? s.modeOpt[0] - 1 : 3;
    if (profile < 0 || profile > 5) profile = 3;
    const double rate = sampleRate;
    // the UTC of the first sample is the computer's clock when the source starts; from there the samples count the time (the receiver does the same)
    const double utc0 = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto synth = std::make_unique<TxLoopSynth>(rate, -kawsTuning().tuneOffsetHz + 30.0 + s.cfoHz, s.snrDb, 0.0, TxLoopSynth::TimedFactory([rate, profile, utc0](int pass, double streamSec) -> std::unique_ptr<TxGenerator> {
    TxRequest r;
    r.modeId = "kaws";
    r.callsign = "SAURCM";
    r.grid = "KO24RM";
    r.dest = "CQ";
    r.text = "SaurCom KAWS test signal: chat from the real transmitter.";
    r.opt["kaws.kind"] = (pass % 2 == 0) ? 0 : 1;   // chat, then a short beacon
    r.opt["kaws.profile"] = profile;
    r.opt["kaws.repeat"] = 1;
    r.opt["kaws.align"] = 1;   // wait for the next slot
    r.opt["kaws.utc"] = utc0 + streamSec;   // of the stream's clock: the computer clock at the first sample plus the samples since
    r.opt["kaws.msgid"] = pass % 16;
    std::string err;
    return makeKawsTxGenerator(r, rate, err);
    }), 0x4B415753ULL);
    if (!synth->ok()) return nullptr;
    return synth;
}

} // namespace dect2
