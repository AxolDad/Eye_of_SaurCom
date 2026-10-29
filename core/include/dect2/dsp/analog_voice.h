// The FM voice of an analog trunked radio channel (EDACS, LTR and MPT1327 traffic channels) for the receivers of those modes: their FM
// discriminator output (Hz, at the decoder's working rate) in, sound out.
//   - Squelch from FM quieting: the discriminator's noise above the channel's modulation (from 5 kHz up) is small while a carrier is there
//     and large without.
//   - Voice band 300 - 3000 Hz (the subaudible signalling of LTR and the EDACS low-speed data below it are taken out), 750 us de-emphasis.
//   - 48 kHz to the sound device (the recorder gets it there, like any mode's sound), only while enabled (the trunking follower carries a
//     call on this receiver) and the squelch is open.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dect2 {

// What the receiver reports about its voice channel (part of the mode's telemetry)
struct AnalogVoiceState {
    bool carrier = false;        // squelch open: a carrier is there
    bool enabled = false;        // the follower asked for sound here
    bool playing = false;        // sound went to the device in the last report period
    float noiseHz = 0;           // the discriminator's rms above 5 kHz (the squelch's measure)
    double carrierSec = 0;       // seconds with the squelch open since the last reset
};

class AudioOut;

class AnalogVoice {
public:
    AnalogVoice();
    ~AnalogVoice();
    // workRate: the discriminator's rate (at least 12 kHz); devHz: the deviation that plays at full scale; noiseHz: where the squelch's
    // noise band starts (above the channel's own modulation: 5 kHz for voice and slow data, higher for a channel with 9600 bit/s data)
    void configure(double workRate, double devHz = 3000, double noiseHz = 5000);
    void reset();
    void push(const float* hz, size_t n);
    // the interface thread
    void setEnabled(bool on) { enabled_ = on; }
    void setVolume(float v) { volume_ = v; }
    void setMuted(bool m) { muted_ = m; }
    void setSilent(bool s) { silent_ = s; }     // decode, never open the sound device (tests)
    void setTap(std::function<void(const float* mono48k, size_t n)> cb) { tap_ = std::move(cb); }   // the feed thread calls it (tests)
    AnalogVoiceState state() const { return st_; }
    void markReported() { st_.playing = false; }
    bool carrier() const { return st_.carrier; }

private:
    struct Bq { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; double run(double x); void clear() { z1 = z2 = 0; } };
    static Bq lowpass(double fc, double fs, double q);
    static Bq highpass(double fc, double fs, double q);
    double rate_ = 0, dev_ = 3000;
    Bq hf_[2], hp_[2], lp_[2];
    double deY_ = 0, deA_ = 0;
    double noisePow_ = 0, noiseA_ = 0;
    double outPos_ = 0, prev_ = 0;
    std::atomic<bool> enabled_{false}, muted_{false}, silent_{false};
    std::atomic<float> volume_{1.f};
    std::function<void(const float*, size_t)> tap_;
    std::unique_ptr<AudioOut> out_;
    std::vector<float> mono_, stereo_;
    AnalogVoiceState st_;
    void flush();
};

} // namespace dect2
