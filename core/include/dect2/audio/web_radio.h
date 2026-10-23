// Internet radio: plays an http(s) audio stream (Icecast / SHOUTcast MP3 or AAC, as radio-browser.info lists them) through an AudioOut, so
// it goes to the chosen sound output, "Record audio" and "Stream audio" like any mode. The bytes come from curl (curl_stream.h: https without a
// TLS library of our own); WebRadioDecoder finds the frames (MP3, or AAC in ADTS) and decodes them with FFmpeg's parser and decoder.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

// Bytes in, interleaved stereo float out. Not thread-safe; one stream per decoder.
class WebRadioDecoder {
public:
    using Out = std::function<void(const float* stereo, int frames, int rate)>;
    WebRadioDecoder();
    ~WebRadioDecoder();
    // false once the stream is found to be neither MP3 nor AAC (ADTS), or the decoder cannot start
    bool feed(const uint8_t* d, size_t n, const Out& out);
    const char* codec() const;     // "MP3", "AAC" or "" until known
    int rate() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

struct WebRadioStats {
    bool playing = false;          // play() was called and stop() not
    std::string url, codec;
    int rate = 0;
    uint64_t bytes = 0;
    double seconds = 0;            // audio decoded
    std::string error;             // why it stopped by itself
};

class WebRadio {
public:
    static WebRadio& get();
    ~WebRadio();
    // false with err for anything but an http:// or https:// address
    bool play(const std::string& url, std::string& err);
    void stop();
    void setVolume(float v);
    void setMuted(bool m);
    WebRadioStats stats() const;
    static bool urlOk(const std::string& url);
    struct Impl;
private:
    WebRadio();
    std::unique_ptr<Impl> p_;
};

}  // namespace dect2
