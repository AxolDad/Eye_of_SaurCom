// "Stream audio": the sound you hear (any mode, before the volume slider) as an MP3 stream to an Icecast server (or anything that takes an
// Icecast source: Broadcastify's Icecast feeds, AzuraCast, Liquidsoap's harbor). Every AudioOut::write() offers its frames; like "Record audio"
// the first output that plays after Start owns the stream and another takes over after 2 s of silence. The audio thread only copies into a
// lock-free ring; the streamer's thread encodes (LAME, mp3_enc.h) and sends, and reconnects every few seconds when the server goes away.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

struct AudioStreamConfig {
    std::string host;
    int port = 8000;
    std::string mount = "/saurcom";       // the stream's address on the server ("/" added in front when missing)
    std::string user = "source", password;
    std::string name = "Eye of SaurCom";  // ice-name: the station name listeners see
    std::string description;             // ice-description (the mode and frequency)
    int kbps = 64;
    bool publicListing = false;          // ice-public: listed in the server's public directory
};

struct AudioStreamStats {
    bool active = false;         // Start was pressed and not Stop
    bool connected = false;      // the server accepted the stream
    uint64_t bytesSent = 0;
    double seconds = 0;          // audio sent since Start
    uint64_t dropped = 0;        // frames lost because the encoder or the network could not keep up
    int reconnects = 0;
    std::string error;           // the last reason the server refused or the connection broke
};

// The HTTP request that opens an Icecast source (PUT, Basic authentication, the ice-* headers), and whether the first line of the server's
// answer accepts it. Without network: the tests check them.
std::string icecastRequest(const AudioStreamConfig& c);
bool icecastAccepted(const std::string& statusLine, std::string& why);

class AudioStreamer {
public:
    static AudioStreamer& instance();
    // false with err when MP3 encoding is not available or the configuration is incomplete. The connection itself is made on the thread.
    bool start(const AudioStreamConfig& c, std::string& err);
    void stop();
    bool active() const { return active_.load(std::memory_order_acquire); }
    AudioStreamStats stats() const;
    // For AudioOut::write(): never blocks, never allocates. src identifies the output.
    static void offer(const void* src, const float* stereo, int frames, int rate) {
        AudioStreamer& s = instance();
        if (s.active_.load(std::memory_order_relaxed)) s.push(src, stereo, frames, rate);
    }
    AudioStreamer();
    ~AudioStreamer();

    struct Impl;

private:
    void push(const void* src, const float* stereo, int frames, int rate);
    std::unique_ptr<Impl> p_;
    std::atomic<bool> active_{false};
};

}  // namespace dect2
