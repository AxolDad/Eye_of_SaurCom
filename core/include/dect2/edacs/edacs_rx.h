// EDACS receiver: FM discriminator at 76.8 kHz, the 48-bit sync searched at every sample phase (either polarity), three-copy vote and
// BCH(40,28) per message (edacs_proto.h). On a working channel: the FM voice (analog_voice.h), the low-speed data and ProVoice detection.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct EdacsTelemetry;

class EdacsReceiver {
public:
    EdacsReceiver();
    ~EdacsReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    void setCenterHz(double hz);
    void setInverted(bool inv);   // start with the other polarity (the sync finds the right one by itself)
    void setNarrow(bool nb);      // EDACS narrowband: 4800 bit/s
    bool ready() const;
    void reset();
    void clearMessages();
    void feed(const cf32* x, size_t n);
    bool telemetry(EdacsTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    // the voice of a working channel: played only while enabled (the trunking follower has a call here) and a carrier is there
    void setVoice(bool enabled);
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);
    void setAudioTap(std::function<void(const float* mono48k, size_t n)> cb);   // tests
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning edacsTuning();

} // namespace dect2
