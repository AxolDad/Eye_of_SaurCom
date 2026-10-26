// General receiver: listen anywhere in the radio's band with NFM, WFM (mono), AM, USB, LSB or CW.
//
// The channel (dial frequency + a channel offset, set by a click on the spectrum) is mixed to 0 Hz, decimated by two per stage
// (qo100::Decimator) and brought to exactly 48 kHz (WFM: 240 kHz) by the exact resampler, then cut out by a complex FIR whose edges
// follow the demodulator and the chosen width (generalFilterEdges). Demodulators: FM discriminator (NFM 5 kHz, WFM 75 kHz deviation
// to half scale, de-emphasis 50 / 75 / 750 us or off, WFM audio 15 kHz low-passed and taken down by 5), AM envelope with the carrier as
// the AGC, SSB and CW as the real part of the filtered channel (CW: mixed up by the BFO pitch) with an envelope AGC (fast attack, slow
// release) or a fixed gain.
//
// Squelch: NFM, WFM, AM and CW compare the channel power with the noise beside the channel (the median of an averaged spectrum of the
// decimated stream, outside the filter, scaled to the filter's width). "auto" opens 6 dB over it (closes under 4), "level" opens at a
// threshold in dBFS. USB and LSB have no carrier, so auto ignores that and opens on the signal-to-noise inside the voice passband (the
// channel power against the median bin there): speech lifts the mean, the median stays near the noise. A 150 ms hang and a 5 ms ramp keep
// the mute from clicking. The recording hang is separate and longer, so a gap between words does not start a new file.
//
// Audio: 48 kHz to the sound card with the shared volume and mute, and optionally into a 16-bit mono WAV file (what is heard, before
// the volume). Engine standard 57 (active 56). Written to the engine's contract: configure() once the sample rate is known, feed() from
// the analysis thread (never blocks), telemetry() and the setters from any other thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct GeneralTelemetry;   // general_tel.h

// SAM is synchronous AM: a loop locks the station's own carrier instead of measuring the envelope, so fading that takes the carrier
// down no longer distorts the audio, and one sideband can be heard on its own to step around a station crowding the other side.
// SAM is last on purpose: the numbers before it are stored in the settings and used by the test signal generator, so they must not move.
enum GeneralDemod { GenNfm = 0, GenWfm, GenAm, GenUsb, GenLsb, GenCw, GenSam, GenDemodCount };

constexpr double kGeneralAudioRate = 48000.0;
constexpr double kGeneralWideRate = 240000.0;       // WFM demodulation rate (5 x 48 kHz)
constexpr double kGeneralSsbLowCutHz = 300.0;       // USB / LSB: the passband starts this far from the dial

const char* generalDemodName(int demod);             // "NFM", "WFM", "AM", "USB", "LSB", "CW"
// Width presets of a demodulator (Hz), the default among them, and the range a dragged edge may set
const double* generalWidthPresets(int demod, int& count);
double generalDefaultWidth(int demod);
void generalWidthRange(int demod, double& minHz, double& maxHz);
double generalClampWidth(int demod, double hz);
// The channel filter relative to the channel frequency: [-w/2, w/2] for NFM / WFM / AM / CW, [300, 300 + w] USB, [-300 - w, -300] LSB
void generalFilterEdges(int demod, double widthHz, double& loHz, double& hiHz);
double generalDefaultDeemphUs(int demod);           // WFM 50 us, the others off (0)

class GeneralReceiver {
public:
    GeneralReceiver();
    ~GeneralReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                 // where the dial frequency sits relative to 0 Hz in the input
    void setCenterHz(double) {}
    bool ready() const;
    void reset();                                    // a retune: filters, squelch and noise estimate start again
    void feed(const cf32* x, size_t n);
    bool telemetry(GeneralTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setChannelOffsetHz(double hz);              // the channel relative to the dial frequency (click-to-tune)
    void setDemod(int demod);                        // GeneralDemod
    void setWidthHz(double hz);                      // filter width (clamped to the demodulator's range)
    void setSquelch(bool on, bool autoMode, float levelDb);   // levelDb: the "level" threshold in dBFS
    void setAgc(bool on, float manualGainDb);        // AM / SSB / CW: AGC, or a fixed gain (dB) when off
    void setDeemphUs(double us);                     // NFM / WFM: 0 off, 50, 75, 750
    void setCwPitchHz(double hz);                    // CW: the tone a carrier on the channel frequency gives (BFO)
    // SAM: which sideband to listen to (sam::Dsb both, sam::Usb, sam::Lsb) and how fast the carrier loop follows (3 - 20 Hz).
    void setSamSide(int side);
    void setSamLoopBwHz(double hz);
    // Co-channel canceller (AM and SAM): locks the strong station on the channel, rebuilds it from the sideband the interference is
    // not on (cleanSide) and subtracts it, so the station underneath can be heard. depth 0 passes the channel through unchanged.
    void setCoChannel(bool on, int cleanSide, float depth);

    void setVolume(float v);                         // 0 .. 1
    void setMuted(bool m);
    void setAudioFilters(double hpHz, double lpHz, double notchHz);   // on the audio: high-pass, low-pass, notch (Hz; 0 = off)
    // Neural noise remover (RNNoise, speech only), after the audio filters: on/off and a dry/wet strength 0..1. Off by default; delays the audio
    // by about 20 ms while on. For NFM, WFM and AM voice; it damages SSB data, CW and music.
    void setNoiseRemover(bool on, float strength);
    float noiseRemoverVad() const;                   // speech probability of the last 10 ms (0 while off)
    void setSilent(bool s);                          // demodulate but never open the sound device (tests, command line)
    void setAudioTap(std::function<void(const float* x, size_t n)> cb);   // tests: the 48 kHz audio after the squelch

    bool startRecording(const std::string& wavPath); // 16-bit mono 48 kHz WAV of what is heard; false when the file cannot be made
    void stopRecording();
    bool recording() const;
    // One file per transmission: a file opens when the squelch opens and closes after the recording hang (clips whose squelch-open
    // time is under minSec are deleted). A non-empty prefix keeps the old names dir/prefix_<n>.wav. An empty prefix names each file
    // from the time, the RF frequency (setListenHz) and the mode: zorcom_YYYYMMDD-HHMMSS_145.500000MHz_USB.wav.
    void setTransmissionRecording(bool on, const std::string& dir, const std::string& prefix, double minSec = 0.5);
    // The channel's RF frequency in Hz, read when a per-transmission file opens (the name and the log).
    void setListenHz(double hz);
    // preRollSec 0..3: the file starts this far before the squelch opened (the first syllable). hangSec 0..3: after the squelch shuts,
    // wait this long before ending the file, so a pause between words stays one transmission. iq: also write the channel IQ, the complex
    // baseband at the demodulator rate (48 kHz, WFM 240 kHz), little-endian float, beside the audio as <name>_48k.cf32. csv: append
    // time,frequency_hz,mode,duration_s,path to dir/transmissions.csv for each file kept.
    void setTransmissionGate(double preRollSec, double hangSec, bool iq, bool csv);
    // The per-transmission files as FLAC (lossless, about half the size) instead of WAV, with Vorbis comments ("TITLE=..."). startRecording()
    // writes FLAC when its path ends in .flac (the tags apply there too).
    void setRecordFormat(bool flac, const std::vector<std::string>& tags = {});
    // MP3 (64 kbit/s mono, through LAME loaded at run time) wins over FLAC for the per-transmission files and for paths ending in .mp3;
    // ignored when libmp3lame is not installed (mp3Available()).
    void setRecordMp3(bool on);
    static bool mp3Available();
    int transmissionFiles() const;                   // files kept since it was switched on

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning generalTuning();
std::string generalSummary(const GeneralTelemetry& t);

} // namespace dect2
