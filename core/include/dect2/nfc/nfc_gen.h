// NFC test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// A reader's 13.56 MHz field with a card in it, as an antenna beside them hears it: the carrier at the centre (plus the config's offset),
// the reader's commands as 100 % ASK pauses in modified Miller code at 106 kbit/s (ISO/IEC 14443-2), the card's answers as load
// modulation, an 847.5 kHz subcarrier switched on for half of each bit (Manchester), a few per cent deep. The exchange is the one every
// reader runs when a card arrives: REQA, ATQA, SELECT, the UID with its BCC, the SAK, then HLTA; a 7-byte UID takes two cascade levels.
// It repeats every periodSec with the carrier on throughout.
//
// makeNfcSynth (SynthConfig): snrDb = carrier to noise over the whole sample rate; cfoHz = carrier offset; modeOpt[0] = 0 a MIFARE Classic
// 1K with a 4-byte UID, 1 an NTAG with a 7-byte UID (two cascade levels and a READ), 2 noise only, no field.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct NfcGenConfig {
    double rate = 10e6;
    double snrDb = 40;            // carrier to noise over the whole sample rate
    double cfoHz = 0;             // the carrier's offset from the centre
    double amp = 0.5;             // carrier amplitude (full scale 1.0)
    double depth = 0.12;          // the card's load modulation: the subcarrier's amplitude as a fraction of the carrier
    double pauseUs = 2.5;         // the reader's pause (ISO/IEC 14443-2: 2 to 3 us at 106 kbit/s)
    int scene = 0;                // 0 MIFARE Classic 1K (4-byte UID), 1 NTAG (7-byte UID, two cascade levels, a READ)
    bool noiseOnly = false;
    double leadSec = 0.02;        // carrier alone before the first exchange
    double periodSec = 0.1;       // one exchange per period
    uint32_t seed = 1;
};

// The frames of a scene in the order they go over the air, as the decoder should list them (CRC bytes included where sent)
struct NfcGenFrame { bool poll; std::vector<uint8_t> bytes; int bits; };   // bits: 7 for a short frame, else 0
std::vector<NfcGenFrame> nfcTestExchange(int scene);
std::vector<uint8_t> nfcTestUid(int scene);   // the whole UID of the scene's card

class NfcGenerator {
public:
    explicit NfcGenerator(const NfcGenConfig& c);
    ~NfcGenerator();
    void generate(cf32* out, size_t n);
    static double noiseSigma(const NfcGenConfig& c);   // per I / Q component
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeNfcSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
