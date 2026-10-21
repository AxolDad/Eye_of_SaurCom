// Optional USAC (xHE-AAC) decoder that the user installs: libfdk-aac, found and loaded at run time. It is never shipped or linked.
// Only the six C functions below are used (TT_MP4_RAW transport, the AudioSpecificConfig built from the DRM config).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2 { namespace drm {

struct UsacLib {
    typedef void* (*OpenFn)(int transport, unsigned layers);
    typedef int (*ConfigRawFn)(void* h, uint8_t** conf, const unsigned* len);
    typedef int (*FillFn)(void* h, uint8_t** buf, const unsigned* size, unsigned* valid);
    typedef int (*DecodeFn)(void* h, int16_t* pcm, int pcmSize, unsigned flags);
    typedef const int* (*InfoFn)(void* h);          // CStreamInfo: sampleRate, frameSize, numChannels are its first three ints
    typedef void (*CloseFn)(void* h);
    OpenFn open = nullptr; ConfigRawFn configRaw = nullptr; FillFn fill = nullptr; DecodeFn decode = nullptr; InfoFn info = nullptr; CloseFn close = nullptr;
    bool ok() const { return open && configRaw && fill && decode && info && close; }

    // Resolves the six symbols through `sym` (name -> address or null).
    static UsacLib fromResolver(const std::function<void*(const char*)>& sym);
    // Loads libfdk-aac (or the library named by `path` when not empty; DECT2_FDKAAC_LIB is tried next).
    static UsacLib load(const std::string& path = std::string());
    static const char* libraryHint();               // names to install, for the in-app message
};

// One decoder session on top of UsacLib: open with the ASC, feed raw access units, get 16-bit PCM.
class UsacSession {
public:
    explicit UsacSession(const UsacLib& lib) : lib_(lib) {}
    ~UsacSession() { close(); }
    UsacSession(const UsacSession&) = delete;
    UsacSession& operator=(const UsacSession&) = delete;
    bool open(const std::vector<uint8_t>& asc);
    // Interleaved PCM of one access unit; false when the decoder gave nothing. rate/channels as the decoder reports them.
    bool decode(const std::vector<uint8_t>& au, std::vector<int16_t>& pcm, int& rate, int& channels);
    void close();
    bool isOpen() const { return h_ != nullptr; }
private:
    UsacLib lib_;
    void* h_ = nullptr;
};

}} // namespace dect2::drm
