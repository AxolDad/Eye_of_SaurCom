// Flipper Zero Sub-GHz files (.sub): the captures a Flipper (or a tool that writes its format: PandwaRF Kaiju, the ESP32 Sub-GHz boards)
// saves from its CC1101. Three kinds share the header (Filetype, Version, Frequency, Preset, Protocol):
//   RAW     "Flipper SubGhz RAW File": RAW_Data lines of timings in microseconds, carrier on positive, off negative, up to 512 a line
//   BinRAW  Protocol: BinRAW, Bit_RAW / Data_RAW blocks of bits, one bit per TE microseconds (1 = carrier)
//   Key     "Flipper SubGhz Key File": a decoded remote (Protocol, Bit, Key, TE), no timings
// The timings of a RAW or BinRAW file become an IsmTrain, the same pulse list the receiver slices a burst into, so the ISM decoders
// (ism_proto.h) read a file exactly as they read the air. The format is documented in the Flipper firmware's
// documentation/file_formats/SubGhzFileFormats.md.
#pragma once
#include "ism_proto.h"
#include <string>

namespace dect2 {

struct SubFile {
    enum Kind { Raw, BinRaw, Key } kind = Raw;
    int version = 0;
    double freqHz = 0;
    std::string preset;          // FuriHalSubGhzPresetOok650Async, ...Custom, ...
    bool fsk = false;            // the preset is a 2FSK / GFSK / MSK one (a custom preset: unknown, taken as OOK)
    std::string customModule;    // Custom_preset_module (CC1101) and its register data, kept as written
    std::string customData;
    std::string protocol;        // RAW, BinRAW, or the key file's protocol (Princeton, KeeLoq, ...)
    IsmTrain train;              // RAW and BinRAW: the pulses (a key file has none)
    size_t values = 0;           // RAW: timings read; BinRAW: bits read
    int bits = 0;                // Key: Bit; BinRAW: Bit
    std::string key;             // Key: the Key bytes as written ("00 00 00 00 00 95 D5 D4")
    int teUs = 0;                // Key and BinRAW: TE
};

// The file's text. false: err says what is wrong (not a Sub-GHz file, no frequency, no timings ...). Lenient where Flippers are: two
// timings of one sign in a row are added up, a leading gap is dropped, zero is skipped.
bool parseSubFile(const std::string& text, SubFile& out, std::string& err);

// A RAW file of this train at this frequency, as a Flipper reads it back: the preset is Ook650Async, or 2FSKDev476Async for an FSK train.
std::string subFileText(const IsmTrain& t, double freqHz);

} // namespace dect2
