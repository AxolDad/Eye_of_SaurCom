// Codec 2 for M17 voice, from libcodec2 loaded at run time (as FreeDV does, hfdig_freedv.cpp). Eye of ZorCom ships it as a separate shared library
// (third_party/codec2, LGPL-2.1) next to the program; an installed libcodec2 or a copy in a codecs folder works as well.
// Without the library the M17 receiver still shows the frames and callsigns, only without sound.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

bool m17Codec2Available(std::string* path = nullptr);   // the library was found and has the functions needed
const char* m17Codec2InstallHint();                     // how to install it on this platform

// One Codec 2 state: mode 3200 (8 bytes per 20 ms, 160 samples) or 1600 (8 bytes per 40 ms, 320 samples); 8 kHz audio
class M17Codec2 {
public:
    explicit M17Codec2(int rate);                       // 3200 or 1600; ok() false when the library is missing
    ~M17Codec2();
    bool ok() const;
    int samplesPerFrame() const;
    void decode(const uint8_t bits[8], int16_t* pcm);   // samplesPerFrame() samples
    void encode(const int16_t* pcm, uint8_t bits[8]);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
