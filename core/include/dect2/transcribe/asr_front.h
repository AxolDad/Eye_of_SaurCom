// Conditioning the audio the speech engine reads, which is not the audio anyone listens to.
//
// A radio channel carries things that are not speech and that a speech engine will nevertheless try to turn into words: CTCSS tones (67 - 254
// Hz) and DCS turn-off bitstreams under the voice band, discriminator hiss above it, and MDC-1200 or single-tone key-up bursts in the middle
// of it. None of them are audible as speech to a person, and all of them cost accuracy.
//
// This chain runs on the copy handed to the transcriber (transcriber.h), ahead of the voice segmenter, so the burst blanker and the band limit
// help the speech detector as well as the engine: a data burst that never reaches the segmenter can never start a transmission. Everything a
// person hears or records is untouched by it - the listener's chain is audio_cleanup.h, and it is a different thing for a different reason.
//
// Deliberately not here: a squelch-tail noise gate. The segmenter already tracks the noise floor and will not open below it plus a margin,
// and a squelch tail sits at the noise floor by definition; a second, independent speech-or-noise judgement would mostly argue with the first.
//
// All of it is deterministic: fixed filters and a tone detector, no model, no training, and the same input always gives the same output. With
// every stage off, process() leaves the samples bit for bit as they were.
#pragma once
#include <cstddef>
#include <vector>

namespace dect2 {

struct AsrFrontOptions {
    bool enabled = true;
    // The voice band. Radio speech lives in 300 - 3400 Hz; what is outside it is signalling or noise.
    bool bandpass = true;
    // The high pass is 6th order, not 4th: a 4th-order corner at 300 Hz leaves 254.1 Hz - the top of the CTCSS range - only 6.4 dB down,
    // which is not enough to stop a strong tone holding the segmenter's level gate open. 6th order puts it 9.1 dB down and the far more
    // common 203.5 Hz tone 20 dB down, for one more biquad.
    float highPassHz = 300;     // above CTCSS (254.1 Hz at the top) and above DCS (134.4 bps)
    float lowPassHz = 3400;     // 4th order is plenty: nothing we care about sits just above it
    // Key-up and signalling bursts, muted before the segmenter sees them.
    bool blankBursts = true;
    float burstToneAHz = 1200;  // MDC-1200 and its relatives signal with this pair
    float burstToneBHz = 1800;
    float burstHoldMs = 12;     // keep muting this long after the tone stops, so the tail does not leak
    // How much of a frame's energy must be the two tones before it counts as a burst rather than speech that happens to have energy there.
    // Speech spreads its energy across formants, so a frame this concentrated on two fixed tones is not speech.
    float burstRatio = 0.55f;
    // How many frames in a row must look like that before the blanker acts. A signalling burst is steady and lasts hundreds of
    // milliseconds; a held vowel can be concentrated enough to look like one for a frame or two, so a single frame is not evidence.
    int burstFramesNeeded = 3;
    float fadeMs = 2;           // the blanker ramps in and out over this, so muting never clicks
};

// What the chain did to the audio it has seen, for the UI and the tests. Never read by the processing itself.
struct AsrFrontStats {
    size_t samples = 0;         // samples processed
    size_t frames = 0;          // analysis frames the burst detector judged
    size_t burstFrames = 0;     // frames it muted
    float lastToneRatio = 0;    // share of the last frame's energy at the two tones, 0 .. 1
    float subAudibleDb = -120;  // the strongest sub-audible level seen, dBFS: how much CTCSS the channel carries
};

// One instance per source (channel). Not thread safe: the transcriber owns one per channel and feeds it from one thread.
class AsrFront {
public:
    AsrFront();
    // Sizes the filters and the analysis frame for this sample rate. Call before process(), and again whenever the rate changes.
    // Rates from 8 kHz up; a rate below 2.5 x lowPassHz turns the band pass off, since there would be no band left to pass.
    void prepare(int sampleRate);
    void setOptions(const AsrFrontOptions& o);   // takes effect on the next process(); redesigns the filters when a corner moved
    const AsrFrontOptions& options() const { return o_; }
    int rate() const { return rate_; }

    // Conditions `n` mono samples in place. Allocates only when a call is larger than any before it, and never blocks.
    // With options().enabled false, or with both stages off, the samples are left exactly as they were.
    void process(float* x, size_t n);

    // True while the blanker is muting: a caller that cuts transmissions can treat the audio as silence rather than as quiet speech.
    bool blanking() const { return holdLeft_ > 0; }

    // The detector measures whole frames and applies what it finds to the audio that follows, because process() must return as many samples
    // as it was given and so cannot hold any back. A burst is therefore muted from about one frame (15 ms) in, not from its first sample.
    // That is enough: what matters is that the sustained part never reaches the segmenter, and 15 ms cannot open a transmission on its own.

    const AsrFrontStats& stats() const { return st_; }
    void clearStats();
    void reset();               // forget the filter state and the hold: a new transmission, or the channel went away

    // The analysis frame length chosen for a rate: the multiple of rate / gcd(rate, 600) nearest 15 ms, so that both tones fall on Goertzel
    // bin centres (120 samples at 8 kHz, 240 at 16 kHz, 720 at 48 kHz, 588 at 44.1 kHz). Public for the tests.
    static size_t frameLenFor(int sampleRate);

private:
    // a normalised biquad in direct form 1, y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        inline double step(double x) {
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            return y;
        }
        void clear() { x1 = x2 = y1 = y2 = 0; }
    };
    // One Goertzel bin: how much of a block is one tone, without paying for an FFT.
    struct Goertzel {
        double coeff = 0;
        void set(double hz, double fs);
        double energy(const float* x, size_t n) const;   // energy at the tone, in the same units as sum(x*x)
    };

    void design();
    void judge(const float* x, size_t n);    // one whole analysis frame: measure the tones and owe blanking if it is a burst

    AsrFrontOptions o_;
    int rate_ = 0;
    bool bandOk_ = false;                    // the rate leaves a voice band to pass
    // Butterworth as cascaded second-order sections: 6th order (three) up, 4th order (two) down
    Biquad hp_[3], lp_[2];
    Goertzel gA_, gB_;
    size_t frameLen_ = 0;
    std::vector<float> acc_;                 // the frame being measured; it is a copy, nothing is held back from the caller
    int holdLeft_ = 0;                       // samples of blanking still owed
    int burstRun_ = 0;                       // consecutive frames that looked like a burst
    int fadeLen_ = 1;
    float gain_ = 1;                         // the blanker's current gain, ramped towards 0 or 1
    AsrFrontStats st_;
};

// The level below `cornerHz` in a block of samples, in dBFS, measured with the same 4th-order corner the chain uses. For the UI reading of how
// much sub-audible tone a channel carries, and for the tests. -120 for silence.
float subAudibleLevelDb(const float* x, size_t n, int sampleRate, float cornerHz);

}  // namespace dect2
