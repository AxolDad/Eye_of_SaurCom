// Video out for the pictures that exist only as frames (analog TV, analog FPV): MPEG-2 video in an MPEG transport stream over UDP, unicast
// or multicast, that VLC, ffplay, OBS or a TV head end opens as udp://@:<port>. (Digital TV goes out as it came in: the TS outputs and the
// network tuner in tsout.h / nettuner.h.) push() only hands the newest picture to the streamer's thread, which encodes and sends.
#pragma once
#include "dect2/atv/atv_tel.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

struct VideoStreamConfig {
    std::string host = "127.0.0.1";   // a multicast address (239.x.x.x) reaches every listener on the network
    int port = 5004;
    int kbps = 4000;
};

struct VideoStreamStats {
    bool active = false;
    uint64_t framesSent = 0, framesDropped = 0, bytesSent = 0;
    int width = 0, height = 0;
    std::string error;
};

class VideoStreamer {
public:
    VideoStreamer();
    ~VideoStreamer();
    bool start(const VideoStreamConfig& c, std::string& err);
    void stop();
    bool active() const { return active_.load(std::memory_order_acquire); }
    void push(std::shared_ptr<const AtvFrame> f);   // the newest picture; a picture the thread has not taken yet is replaced (counted as dropped)
    VideoStreamStats stats() const;
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
    std::atomic<bool> active_{false};
};

}  // namespace dect2
