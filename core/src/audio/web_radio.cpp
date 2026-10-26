#include "dect2/audio/web_radio.h"
#include "dect2/audio/audioout.h"
#include "dect2/common/thread_name.h"
#include "dect2/engine/curl_stream.h"
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace dect2 {

// ---------------------------------------------------------------- the decoder

struct WebRadioDecoder::Impl {
    enum class Kind { Unknown, Mp3, Aac, Bad } kind = Kind::Unknown;
    std::vector<uint8_t> pending;            // bytes before the first frame sync was found
    AVCodecParserContext* parser = nullptr;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    SwrContext* swr = nullptr;
    int swrRate = 0, swrFmt = -1, swrCh = 0;
    std::vector<float> out;
    ~Impl() {
        if (swr) swr_free(&swr);
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (ctx) avcodec_free_context(&ctx);
        if (parser) av_parser_close(parser);
    }
    // the first MPEG audio frame sync (11 set bits): layer bits 00 are ADTS (AAC), anything else is MPEG-1/2 audio (MP3)
    Kind detect() {
        size_t from = 0;
        if (pending.size() >= 3 && pending[0] == 'I' && pending[1] == 'D' && pending[2] == '3') {   // an ID3v2 tag first (MP3 files): skip it
            if (pending.size() < 10) return Kind::Unknown;
            from = 10 + ((size_t)(pending[6] & 0x7F) << 21 | (size_t)(pending[7] & 0x7F) << 14 | (size_t)(pending[8] & 0x7F) << 7 | (size_t)(pending[9] & 0x7F));
        }
        for (size_t i = from; i + 1 < pending.size(); i++) {
            if (pending[i] != 0xFF || (pending[i + 1] & 0xE0) != 0xE0) continue;
            return ((pending[i + 1] >> 1) & 3) == 0 ? Kind::Aac : Kind::Mp3;
        }
        // no sync in 64 kB after the tag (or a tag over 4 MB): not an audio stream we read
        return pending.size() > from + 64 * 1024 || pending.size() > (4u << 20) ? Kind::Bad : Kind::Unknown;
    }
    bool open() {
        const AVCodecID id = kind == Kind::Aac ? AV_CODEC_ID_AAC : AV_CODEC_ID_MP3;
        const AVCodec* c = avcodec_find_decoder(id);
        parser = av_parser_init(id);
        if (!c || !parser) return false;
        ctx = avcodec_alloc_context3(c);
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        return ctx && pkt && frame && avcodec_open2(ctx, c, nullptr) >= 0;
    }
    void emit(const Out& o) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (!swr || swrRate != frame->sample_rate || swrFmt != frame->format || swrCh != frame->ch_layout.nb_channels) {
            if (swr) swr_free(&swr);
            if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, frame->sample_rate, &frame->ch_layout, (AVSampleFormat)frame->format, frame->sample_rate, 0, nullptr) < 0 ||
                swr_init(swr) < 0) { swr_free(&swr); return; }
            swrRate = frame->sample_rate; swrFmt = frame->format; swrCh = frame->ch_layout.nb_channels;
        }
        const int maxOut = swr_get_out_samples(swr, frame->nb_samples) + 64;
        out.resize((size_t)maxOut * 2);
        uint8_t* dst[1] = {reinterpret_cast<uint8_t*>(out.data())};
        const int n = swr_convert(swr, dst, maxOut, (const uint8_t**)frame->extended_data, frame->nb_samples);
        if (n > 0) o(out.data(), n, frame->sample_rate);
    }
    void decode(const uint8_t* d, size_t n, const Out& o) {
        while (n > 0) {
            uint8_t* data = nullptr; int size = 0;
            const int used = av_parser_parse2(parser, ctx, &data, &size, d, (int)std::min<size_t>(n, 1 << 20), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0) return;
            d += used; n -= (size_t)used;
            if (size <= 0) continue;
            pkt->data = data; pkt->size = size;
            if (avcodec_send_packet(ctx, pkt) < 0) continue;   // a damaged frame: the next one decodes
            while (avcodec_receive_frame(ctx, frame) >= 0) emit(o);
        }
    }
};

WebRadioDecoder::WebRadioDecoder() : p_(new Impl) {}
WebRadioDecoder::~WebRadioDecoder() = default;
const char* WebRadioDecoder::codec() const { return p_->kind == Impl::Kind::Mp3 ? "MP3" : p_->kind == Impl::Kind::Aac ? "AAC" : ""; }
int WebRadioDecoder::rate() const { return p_->swrRate; }

bool WebRadioDecoder::feed(const uint8_t* d, size_t n, const Out& out) {
    Impl& s = *p_;
    if (s.kind == Impl::Kind::Bad) return false;
    if (s.kind == Impl::Kind::Unknown) {
        s.pending.insert(s.pending.end(), d, d + n);
        s.kind = s.detect();
        if (s.kind == Impl::Kind::Unknown) return true;
        if (s.kind == Impl::Kind::Bad || !s.open()) { s.kind = Impl::Kind::Bad; return false; }
        std::vector<uint8_t> first;
        first.swap(s.pending);
        s.decode(first.data(), first.size(), out);
        return true;
    }
    s.decode(d, n, out);
    return true;
}

// ---------------------------------------------------------------- the player

struct WebRadio::Impl {
    AudioOut audio;
    std::thread th;
    std::atomic<bool> cancel{false};
    mutable std::mutex mu;
    WebRadioStats st;
    float volume = 1; bool muted = false;
};

WebRadio& WebRadio::get() { static WebRadio r; return r; }
WebRadio::WebRadio() : p_(new Impl) {}
WebRadio::~WebRadio() { stop(); }

bool WebRadio::urlOk(const std::string& u) {
    if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0) return false;
    for (unsigned char c : u) if (c <= ' ' || c == 0x7F) return false;   // no spaces or control characters (a URL from a directory is untrusted)
    return u.size() < 2048;
}

void WebRadio::stop() {
    p_->cancel = true;
    if (p_->th.joinable()) p_->th.join();
    p_->audio.stop();
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->st.playing = false;
}

void WebRadio::setVolume(float v) { std::lock_guard<std::mutex> lk(p_->mu); p_->volume = v; p_->audio.setVolume(v); }
void WebRadio::setMuted(bool m) { std::lock_guard<std::mutex> lk(p_->mu); p_->muted = m; p_->audio.setMuted(m); }
WebRadioStats WebRadio::stats() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->st; }

bool WebRadio::play(const std::string& url, std::string& err) {
    if (!urlOk(url)) { err = "only http:// and https:// addresses play"; return false; }
    stop();
    Impl& s = *p_;
    s.cancel = false;
    { std::lock_guard<std::mutex> lk(s.mu); s.st = WebRadioStats(); s.st.playing = true; s.st.url = url; }
    s.th = std::thread([this, url] {
        setThreadName("web-radio");
        Impl& s = *p_;
        WebRadioDecoder dec;
        bool bad = false;
        uint64_t frames = 0;
        const auto played = [&](const float* stereo, int n, int rate) {
            if (!s.audio.running() || s.audio.sampleRate() != rate) {
                s.audio.stop();
                if (!s.audio.start(rate)) return;
                { std::lock_guard<std::mutex> lk(s.mu); s.audio.setVolume(s.volume); s.audio.setMuted(s.muted); s.st.rate = rate; s.st.codec = dec.codec(); }
            }
            // a stream that comes faster than real time (a file, the server's burst): wait while 3 s are queued
            while (!s.cancel && s.audio.bufferedFrames() > 3 * rate) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            s.audio.write(stereo, n);
            frames += (uint64_t)n;
            std::lock_guard<std::mutex> lk(s.mu); s.st.seconds = (double)frames / rate;
        };
        const std::vector<std::string> args = {"-sSL", "--no-buffer", "--proto", "=http,https", "--proto-redir", "=http,https", "--max-redirs", "5",
                                               "--connect-timeout", "10", "-A", "EyeOfSaurCom", "--url", url};
        const int rc = curlStream(args, [&](const char* d, size_t n) {
            if (bad || s.cancel) return;
            { std::lock_guard<std::mutex> lk(s.mu); s.st.bytes += n; }
            if (!dec.feed(reinterpret_cast<const uint8_t*>(d), n, played)) { bad = true; s.cancel = true; }
        }, s.cancel);
        std::lock_guard<std::mutex> lk(s.mu);
        if (bad) s.st.error = "not an MP3 or AAC stream (playlists such as .m3u or .pls: open the address inside them)";
        else if (rc == -2) s.st.error = "curl was not found; it is needed for internet radio";
        else if (rc > 0) s.st.error = "the stream ended or could not be reached (curl " + std::to_string(rc) + ")";
        s.st.playing = false;
    });
    return true;
}

}  // namespace dect2
