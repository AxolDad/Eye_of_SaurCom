// Video out (dect2/tv/video_stream.h): analog pictures as MPEG-2 in an MPEG transport stream over UDP. Every datagram is whole TS packets,
// and FFmpeg reads the result back as one MPEG-2 video stream of the pictures' size.
#include "dect2/tv/video_stream.h"
#include "../../core/src/net/tcp_client.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
}

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

int main() {
    netInit();
    sock_t rx = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(rx, (sockaddr*)&a, sizeof a);
    socklen_t n = sizeof a; getsockname(rx, (sockaddr*)&a, &n);
    sockRecvTimeout(rx, 1500);
    const int bigBuf = 4 << 20; setsockopt(rx, SOL_SOCKET, SO_RCVBUF, (const char*)&bigBuf, sizeof bigBuf);

    VideoStreamer vs;
    std::string err;
    VideoStreamConfig bad; bad.port = 0;
    CHECK(!vs.start(bad, err) && !err.empty(), "port 0 refused");
    VideoStreamConfig lowRate; lowRate.kbps = 100;
    CHECK(!vs.start(lowRate, err) && !err.empty(), "kbit/s below 200 refused");
    { VideoStreamer unused; VideoStreamConfig b2; b2.port = 70000; CHECK(!unused.start(b2, err), "port 70000 refused"); }   // destroyed after a refused start
    VideoStreamConfig c; c.host = "127.0.0.1"; c.port = ntohs(a.sin_port); c.kbps = 3000;
    CHECK(vs.start(c, err), "start: %s", err.c_str());

    std::vector<uint8_t> ts;
    bool whole = true;
    std::thread reader([&] {
        uint8_t b[2048];
        for (;;) {
            const long long k = sockRecv(rx, b, sizeof b);
            if (k <= 0) break;
            whole &= k % 188 == 0 && b[0] == 0x47;
            ts.insert(ts.end(), b, b + k);
        }
    });
    for (int i = 0; i < 40; i++) {   // a moving gradient
        auto f = std::make_shared<AtvFrame>();
        f->seq = (uint64_t)i + 1; f->width = 640; f->height = 480; f->fieldRate = 59.94; f->colour = true;
        f->rgba.resize((size_t)640 * 480 * 4);
        for (int y = 0; y < 480; y++) for (int x = 0; x < 640; x++) {
            uint8_t* p = &f->rgba[((size_t)y * 640 + x) * 4];
            p[0] = (uint8_t)(x + i * 4); p[1] = (uint8_t)y; p[2] = (uint8_t)(i * 6); p[3] = 255;
        }
        vs.push(f);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const VideoStreamStats st = vs.stats();
    vs.stop();
    reader.join();
    sockClose(rx);
    CHECK(st.framesSent >= 20 && st.width == 640 && st.height == 480 && st.error.empty(), "sent %llu frames %dx%d '%s'", (unsigned long long)st.framesSent, st.width, st.height, st.error.c_str());
    CHECK(whole && ts.size() > 20000, "whole TS packets in every datagram: %d, %zu bytes", (int)whole, ts.size());

    // read it back
    const std::string path = "test_video_stream.ts";
    { std::ofstream o(path, std::ios::binary); o.write((const char*)ts.data(), (std::streamsize)ts.size()); }
    AVFormatContext* ic = nullptr;
    bool ok = avformat_open_input(&ic, path.c_str(), nullptr, nullptr) == 0 && avformat_find_stream_info(ic, nullptr) >= 0;
    bool mpeg2 = false; int w = 0, h = 0;
    if (ok) for (unsigned i = 0; i < ic->nb_streams; i++) {
        const AVCodecParameters* p = ic->streams[i]->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO) { mpeg2 = p->codec_id == AV_CODEC_ID_MPEG2VIDEO; w = p->width; h = p->height; }
    }
    if (ic) avformat_close_input(&ic);
    std::remove(path.c_str());
    CHECK(ok && mpeg2 && w == 640 && h == 480, "FFmpeg reads an MPEG-2 video of 640x480: ok %d mpeg2 %d %dx%d", (int)ok, (int)mpeg2, w, h);

    std::printf(fails ? "video stream tests FAILED (%d)\n" : "video stream tests passed\n", fails);
    return fails ? 1 : 0;
}
