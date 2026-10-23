// Audio output (CoreAudio on macOS, miniaudio elsewhere) with a lock-free ring buffer; keeps count of frames written and played for A/V sync.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

class AudioOut {
public:
    AudioOut();
    ~AudioOut();
    // Output devices by name (the system's list; the first is the default, shown as ""). setDevice() before start(): "" = the system default.
    // A named device is opened through miniaudio (Windows, Linux) or a HAL unit (macOS); one that is not found falls back to the default.
    static std::vector<std::string> devices();
    void setDevice(const std::string& name) { device_ = name; }
    // The device every output without a device of its own plays on ("" = the system default): where the program's sound goes, for example a
    // virtual audio cable (VB-Cable, BlackHole, a PipeWire / PulseAudio null sink) that another program reads. Takes effect at the next start().
    static void setDefaultDevice(const std::string& name);
    static std::string defaultDevice();
    const std::string& device() const { return device_; }
    // A monitor output (audio_monitor.h) is not "the mode's sound": the monitor yields while any other, unmuted AudioOut has written lately.
    void setMonitorRole(bool m) { monitor_ = m; }
    static int msSinceModeWrite();      // milliseconds since an unmuted non-monitor output last wrote (large when never)
    bool start(int sampleRate = 48000);
    void stop();
    // Interleaved stereo float. Returns frames accepted (drops if the ring is full).
    int write(const float* stereo, int frames);
    void flush();                       // discard queued audio
    int bufferedFrames() const;
    uint64_t writtenFrames() const;
    uint64_t playedFrames() const;      // real frames consumed by the device (silence is not counted)
    void setVolume(float v);
    void setMuted(bool m);
    int sampleRate() const { return rate_; }
    bool running() const { return running_; }
    int underruns() const;
    bool playing() const;               // false while (re)buffering
    // Playback only begins once this many frames are queued (and again after an underrun).
    void setStartThreshold(int frames);

    struct Impl;

private:
    std::unique_ptr<Impl> p_;
    int rate_ = 48000;
    bool running_ = false;
    bool monitor_ = false;
    std::string device_;
};

} // namespace dect2
