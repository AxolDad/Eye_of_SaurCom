// AMBE (D-STAR voice) through the user's own mbelib, loaded at run time: AMBE is patented and Eye of SaurCom does not ship it (docs/CODECS.md).
// Without the library the D-STAR receiver still shows the calls, the slow data and the positions, only without sound.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

bool dstarAmbeAvailable(std::string* path = nullptr);   // mbelib was found and has the functions needed
bool dstarAmbeRescan();                                 // look again (the "Look again" button); true when found

// One decoder state: 72 voice bits in, 160 samples of 8 kHz audio out (20 ms)
class DstarAmbe {
public:
    DstarAmbe();
    ~DstarAmbe();
    bool ok() const;
    void decode(const uint8_t bits[72], int16_t pcm[160]);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
