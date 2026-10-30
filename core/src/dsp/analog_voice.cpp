// The FM voice of analog trunked traffic channels (see analog_voice.h).
#include "dect2/dsp/analog_voice.h"
#include "dect2/audio/audioout.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kOpenHz = 900, kCloseHz = 1300;   // squelch: rms of the discriminator above 5 kHz (open below, close above)
}

double AnalogVoice::Bq::run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }

AnalogVoice::Bq AnalogVoice::lowpass(double fc, double fs, double q) {
    Bq b;
    const double w = 2 * kPi * fc / fs, al = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + al;
    b.b0 = (1 - c) / 2 / a0; b.b1 = (1 - c) / a0; b.b2 = b.b0; b.a1 = -2 * c / a0; b.a2 = (1 - al) / a0;
    return b;
}

AnalogVoice::Bq AnalogVoice::highpass(double fc, double fs, double q) {
    Bq b;
    const double w = 2 * kPi * fc / fs, al = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + al;
    b.b0 = (1 + c) / 2 / a0; b.b1 = -(1 + c) / a0; b.b2 = b.b0; b.a1 = -2 * c / a0; b.a2 = (1 - al) / a0;
    return b;
}

AnalogVoice::AnalogVoice() = default;
AnalogVoice::~AnalogVoice() = default;

void AnalogVoice::configure(double workRate, double devHz, double noiseHz) {
    rate_ = workRate; dev_ = devHz > 0 ? devHz : 3000;
    if (rate_ <= 0) return;
    const double hfc = std::min(noiseHz, 0.4 * rate_);
    hf_[0] = highpass(hfc, rate_, 0.5412); hf_[1] = highpass(hfc, rate_, 1.3066);
    hp_[0] = highpass(300, rate_, 0.5412); hp_[1] = highpass(300, rate_, 1.3066);
    lp_[0] = lowpass(3000, rate_, 0.5412); lp_[1] = lowpass(3000, rate_, 1.3066);
    deA_ = 1 - std::exp(-1 / (750e-6 * rate_));
    noiseA_ = 1 - std::exp(-1 / (0.02 * rate_));
    reset();
}

void AnalogVoice::reset() {
    for (Bq* b : {&hf_[0], &hf_[1], &hp_[0], &hp_[1], &lp_[0], &lp_[1]}) b->clear();
    deY_ = 0; noisePow_ = kCloseHz * kCloseHz * 4; outPos_ = 0; prev_ = 0;
    mono_.clear();
    const bool en = st_.enabled;
    st_ = AnalogVoiceState();
    st_.enabled = en;
}

void AnalogVoice::push(const float* hz, size_t n) {
    if (rate_ <= 0) return;
    const bool en = enabled_.load();
    st_.enabled = en;
    const double step = rate_ / 48000.0;   // input samples per output sample
    for (size_t i = 0; i < n; i++) {
        const double x = std::isfinite(hz[i]) ? hz[i] : 0.0;   // a NaN would stay in the filters and open the squelch
        const double h = hf_[1].run(hf_[0].run(x));
        noisePow_ += noiseA_ * (h * h - noisePow_);
        double v = lp_[1].run(lp_[0].run(hp_[1].run(hp_[0].run(x))));
        deY_ += deA_ * (v - deY_);
        v = deY_ * (2 * kPi * 750e-6 * 1000) / dev_;   // de-emphasised, about unity at 1 kHz for the full deviation
        // linear interpolation to 48 kHz (the voice band is far below both rates)
        while (outPos_ <= 1.0) { mono_.push_back((float)(prev_ + (v - prev_) * outPos_)); outPos_ += step; }
        outPos_ -= 1.0;
        prev_ = v;
    }
    const double rms = std::sqrt(std::max(0.0, noisePow_));
    st_.noiseHz = (float)rms;
    if (st_.carrier ? rms > kCloseHz : rms < kOpenHz) st_.carrier = !st_.carrier;
    if (st_.carrier) st_.carrierSec += (double)n / rate_;
    if (mono_.size() >= 2400) flush();
}

void AnalogVoice::flush() {
    const bool play = enabled_.load() && st_.carrier;
    if (play) {
        if (tap_) tap_(mono_.data(), mono_.size());
        st_.playing = true;
        if (!silent_.load()) {
            stereo_.resize(mono_.size() * 2);
            for (size_t i = 0; i < mono_.size(); i++) stereo_[2 * i] = stereo_[2 * i + 1] = std::clamp(mono_[i], -1.f, 1.f);
            if (!out_) { out_ = std::make_unique<AudioOut>(); out_->start(48000); out_->setStartThreshold(48000 / 5); }
            out_->setVolume(volume_.load()); out_->setMuted(muted_.load());
            if (out_->bufferedFrames() > 48000 * 3 / 2) out_->flush();
            out_->write(stereo_.data(), (int)mono_.size());
        }
    }
    mono_.clear();
}

} // namespace dect2
