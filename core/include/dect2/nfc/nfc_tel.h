// NFC / RFID sniffer telemetry, shared by the receiver and the UI. RxTelemetry::nfc, engine standard 150, active standard 149 (family 148).
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include "nfc_proto.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One frame between a reader and a card, as decoded
struct NfcFrame {
    int64_t wallTime = 0;
    double sec = 0;              // signal seconds since the start
    double durationUs = 0;
    nfc::Tech tech = nfc::TechUnknown;
    bool poll = true;            // true: the reader's command; false: the card's answer
    int rateKbps = 106;
    int bits = 0;                // a frame shorter than a byte: its bit count (7 for REQA, 4 for ACK), else 0
    std::vector<uint8_t> bytes;  // as on the air, CRC included where the frame carries one
    std::string name;            // what it is (nfc_proto.h)
    bool parityError = false, crcError = false, truncated = false, encrypted = false, shortFrame = false;
};

// One card seen in an anticollision exchange, keyed by technology and UID
struct NfcCard {
    nfc::Tech tech = nfc::TechA;
    std::vector<uint8_t> uid;    // NFC-A: 4, 7 or 10 bytes; NFC-B: the PUPI; NFC-F: the IDm; NFC-V: the UID, most significant first
    uint8_t atqa[2] = {0, 0};
    uint8_t sak = 0;
    std::string type;            // the public tables' guess from ATQA and SAK ("" when they say nothing)
    std::string manufacturer;    // from the UID's first byte (NFC-A, NFC-V)
    bool iso14443_4 = false;
    bool encryptedSeen = false;  // an exchange after a MIFARE authentication: marked, not decoded
    int64_t firstWall = 0, lastWall = 0;
    double lastSec = 0;
    uint64_t selections = 0;     // how many times it was selected
};

struct NfcTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 no input yet, 1 listening, 2 frames in the last 5 s
    double cfoHz = 0;            // not measured (0): the magnitude of the field is decoded, whatever its offset
    float snrDb = 0;             // not measured (0)
    float levelDb = -200;        // input power, dBFS
    bool dataValid = false;      // a frame in the last 5 s
    uint64_t blocksOk = 0;       // frames without a parity or CRC error
    uint64_t blocksBad = 0;      // frames with one
    // set-up and statistics
    double timeSec = 0;
    double inputRate = 0;
    double centerHz = 0;         // the radio's centre (0: unknown)
    bool fieldOn = false;        // a reader's 13.56 MHz field is present (the decoder's carrier detector)
    uint64_t fieldOnCount = 0;   // how many times the field came up
    double fieldSec = 0;         // when it last came up
    uint64_t pollFrames = 0, listenFrames = 0;
    uint64_t framesA = 0, framesB = 0, framesF = 0, framesV = 0;
    int techMask = 15;           // which technologies the decoder looks for: 1 A, 2 B, 4 F, 8 V
    float powerThreshold = 0.01f;   // the magnitude below which there is no field
    std::vector<NfcFrame> frames;   // newest first, at most kNfcLogMax
    std::vector<NfcCard> cards;     // most recently seen first
};

constexpr size_t kNfcLogMax = 600;
constexpr size_t kNfcCardMax = 200;

inline std::string nfcSummary(const NfcTelemetry& t) {
    char b[160];
    if (t.state == 0) return "NFC: waiting for samples";
    if (!t.fieldOn && t.frames.empty()) { snprintf(b, sizeof b, "NFC: no reader field (%.0f dBFS)", t.levelDb); return b; }
    snprintf(b, sizeof b, "NFC: field %s, %llu frames, %zu card%s", t.fieldOn ? "on" : "off", (unsigned long long)(t.blocksOk + t.blocksBad), t.cards.size(), t.cards.size() == 1 ? "" : "s");
    return b;
}

} // namespace dect2
