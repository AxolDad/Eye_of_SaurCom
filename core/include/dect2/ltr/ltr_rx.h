// LTR receiver: FM discriminator, 150 Hz low pass, 300 bit/s slicer, sync search in both polarities; the FM voice above 300 Hz.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread, telemetry() from the interface thread.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct LtrTelemetry;

class LtrReceiver {
public:
    LtrReceiver();
    ~LtrReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);
    bool ready() const;
    void reset();
    void clearMessages();
    void feed(const cf32* x, size_t n);
    bool telemetry(LtrTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    // the repeater's voice: played only while enabled (the trunking follower has a call here) and a carrier is there
    void setVoice(bool enabled);
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);
    void setAudioTap(std::function<void(const float* mono48k, size_t n)> cb);   // tests
    // Also decode the subaudible words of these other channels inside the capture (offsets from the radio's centre, like setSignalOffset):
    // every repeater of an LTR system the radio's bandwidth covers is watched at once. Their words come in the same telemetry, tagged by hz.
    void setWatch(const std::vector<double>& offsetsHz);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning ltrTuning();

} // namespace dect2
