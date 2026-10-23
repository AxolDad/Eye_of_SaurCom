#include "dect2/tv/video_stream.h"
#include "dect2/common/thread_name.h"
#include "../net/netcompat.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#ifndef _WIN32
#include <netdb.h>
#endif

namespace dect2 {

namespace {
constexpr int kDatagram = 7 * 188;   // the usual TS-over-UDP payload: seven transport packets
} // namespace

struct VideoStreamer::Impl {
    VideoStreamConfig cfg;
    std::thread th;
    std::mutex mu;
    std::condition_variable cv;
    std::shared_ptr<const AtvFrame> next;   // the mailbox: the newest picture not taken yet
    bool quit = false;
    VideoStreamStats st;
    // the thread's own
    sock_t s = kBadSock;
    sockaddr_in to{};
    std::vector<uint8_t> dgram;
    AVFormatContext* oc = nullptr;
    AVCodecContext* enc = nullptr;
    AVStream* vs = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* yuv = nullptr;
    AVPacket* pkt = nullptr;
    bool headerWritten = false;   // the trailer and the encoder flush only make sense after the header
    int w = 0, h = 0;
    int64_t pts = 0;

    void sendDatagram() {
        if (dgram.empty()) return;
        if (sockSendTo(s, dgram.data(), dgram.size(), to) > 0) { std::lock_guard<std::mutex> lk(mu); st.bytesSent += dgram.size(); }
        dgram.clear();
    }
#if LIBAVFORMAT_VERSION_MAJOR >= 61
    static int write(void* opaque, const uint8_t* buf, int size) {
#else
    static int write(void* opaque, uint8_t* buf, int size) {
#endif
        Impl& I = *static_cast<Impl*>(opaque);
        for (int i = 0; i < size; i++) {
            I.dgram.push_back(buf[i]);
            if ((int)I.dgram.size() == kDatagram) I.sendDatagram();
        }
        return size;
    }
    void close() {
        if (oc) {
            if (headerWritten) {
                if (enc && pkt) {   // what the encoder still holds
                    avcodec_send_frame(enc, nullptr);
                    while (avcodec_receive_packet(enc, pkt) >= 0) { av_packet_rescale_ts(pkt, enc->time_base, vs->time_base); pkt->stream_index = 0; av_interleaved_write_frame(oc, pkt); }
                }
                av_write_trailer(oc);
            }
            if (oc->pb) { avio_flush(oc->pb); av_freep(&oc->pb->buffer); avio_context_free(&oc->pb); }
            avformat_free_context(oc);
            oc = nullptr; vs = nullptr;
            sendDatagram();
        }
        headerWritten = false;
        if (enc) avcodec_free_context(&enc);
        if (sws) { sws_freeContext(sws); sws = nullptr; }
        if (yuv) av_frame_free(&yuv);
        if (pkt) av_packet_free(&pkt);
        w = h = 0;
    }
    bool open(const AtvFrame& f, std::string& err) {
        close();
        const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
        if (!c) { err = "this FFmpeg has no MPEG-2 video encoder"; return false; }
        enc = avcodec_alloc_context3(c);
        if (!enc) { err = "out of memory"; return false; }
        const int fps = f.fieldRate > 55 ? 30 : 25;   // a woven frame per two fields
        enc->width = f.width; enc->height = f.height;
        enc->time_base = AVRational{1, fps}; enc->framerate = AVRational{fps, 1};
        enc->pix_fmt = AV_PIX_FMT_YUV420P;
        enc->bit_rate = (int64_t)cfg.kbps * 1000;
        enc->gop_size = 12; enc->max_b_frames = 0;   // no B frames: low delay for a live picture
        if (avcodec_open2(enc, c, nullptr) < 0) { err = "the MPEG-2 encoder did not start"; close(); return false; }
        if (avformat_alloc_output_context2(&oc, nullptr, "mpegts", nullptr) < 0 || !oc) { err = "no MPEG-TS muxer"; close(); return false; }
        uint8_t* iob = (uint8_t*)av_malloc(kDatagram * 4);
        if (!iob) { err = "out of memory"; close(); return false; }
        oc->pb = avio_alloc_context(iob, kDatagram * 4, 1, this, nullptr, &Impl::write, nullptr);
        if (!oc->pb) { av_free(iob); err = "out of memory"; close(); return false; }
        vs = avformat_new_stream(oc, nullptr);
        if (!vs) { err = "no video stream in the transport stream"; close(); return false; }
        if (avcodec_parameters_from_context(vs->codecpar, enc) < 0) { err = "out of memory"; close(); return false; }
        vs->time_base = enc->time_base;
        if (avformat_write_header(oc, nullptr) < 0) { err = "the transport stream could not start"; close(); return false; }
        headerWritten = true;
        sws = sws_getContext(f.width, f.height, AV_PIX_FMT_RGBA, f.width, f.height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        yuv = av_frame_alloc();
        pkt = av_packet_alloc();
        if (!sws || !yuv || !pkt) { err = "out of memory"; close(); return false; }
        yuv->format = AV_PIX_FMT_YUV420P; yuv->width = f.width; yuv->height = f.height;
        if (av_frame_get_buffer(yuv, 0) < 0) { err = "out of memory"; close(); return false; }
        w = f.width; h = f.height; pts = 0;
        return true;
    }
    void encode(const AtvFrame& f) {
        if (f.width <= 0 || f.height <= 0 || f.rgba.size() < (size_t)f.width * f.height * 4) return;
        if (f.width != w || f.height != h) {
            std::string err;
            if (!open(f, err)) { std::lock_guard<std::mutex> lk(mu); st.error = err; return; }
            std::lock_guard<std::mutex> lk(mu); st.width = w; st.height = h; st.error.clear();
        }
        av_frame_make_writable(yuv);
        const uint8_t* src[1] = {f.rgba.data()};
        const int stride[1] = {f.width * 4};
        sws_scale(sws, src, stride, 0, f.height, yuv->data, yuv->linesize);
        yuv->pts = pts++;
        if (avcodec_send_frame(enc, yuv) < 0) return;
        while (avcodec_receive_packet(enc, pkt) >= 0) {
            av_packet_rescale_ts(pkt, enc->time_base, vs->time_base);
            pkt->stream_index = 0;
            av_interleaved_write_frame(oc, pkt);
        }
        std::lock_guard<std::mutex> lk(mu); st.framesSent++;
    }
};

VideoStreamer::VideoStreamer() : p_(new Impl) {}
VideoStreamer::~VideoStreamer() { stop(); }

VideoStreamStats VideoStreamer::stats() const { std::lock_guard<std::mutex> lk(p_->mu); VideoStreamStats s = p_->st; s.active = active(); return s; }

void VideoStreamer::push(std::shared_ptr<const AtvFrame> f) {
    if (!active() || !f) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->next) p_->st.framesDropped++;
    p_->next = std::move(f);
    p_->cv.notify_one();
}

void VideoStreamer::stop() {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->quit = true; }
    p_->cv.notify_one();
    if (p_->th.joinable()) p_->th.join();
    active_ = false;
}

bool VideoStreamer::start(const VideoStreamConfig& c, std::string& err) {
    stop();
    netInit();
    Impl& I = *p_;
    if (c.port <= 0 || c.port > 65535 || c.kbps < 200 || c.kbps > 50000) { err = "port 1 to 65535 and 200 to 50000 kbit/s"; return false; }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(c.host.c_str(), nullptr, &hints, &res) != 0 || !res) { err = "cannot find the address " + c.host; return false; }
    I.to = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    I.to.sin_port = htons((uint16_t)c.port);
    freeaddrinfo(res);
    I.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (I.s == kBadSock) { err = "no UDP socket"; return false; }
    const unsigned char ttl = 4;   // multicast: a few routers on a site network, not the internet
    setsockopt(I.s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);
    I.cfg = c;
    { std::lock_guard<std::mutex> lk(I.mu); I.quit = false; I.next.reset(); I.st = VideoStreamStats(); }
    active_ = true;
    I.th = std::thread([this] {
        setThreadName("video-stream");
        Impl& I = *p_;
        for (;;) {
            std::shared_ptr<const AtvFrame> f;
            {
                std::unique_lock<std::mutex> lk(I.mu);
                I.cv.wait(lk, [&] { return I.quit || I.next; });
                if (I.quit) break;
                f = std::move(I.next);
            }
            I.encode(*f);
        }
        I.close();
        sockClose(I.s); I.s = kBadSock;
    });
    return true;
}

}  // namespace dect2
