// NFC / RFID sniffer: the exchange between a contactless reader and a card at 13.56 MHz, heard from an antenna near them. Receive only:
// nothing is sent, no field is made and no card is emulated; what passes in the clear between the two is listed, what they encrypt is marked
// and left alone. The point is to see what one's own cards and readers give away (the UID, the card type, the commands) and that a
// reader is not doing more than it should.
//
// The radio sits on 13.56 MHz with the carrier at its centre (no tune offset, and the engine leaves the DC spike alone for this mode:
// ModeTuning::carrierAtCentre). The magnitude of each complex sample is handed to nfc-laboratory's decoder (third_party/nfc-laboratory,
// GPL-3.0), which finds the reader's pauses (100 % ASK, modified Miller at 106 kbit/s and up) and the card's load modulation (the
// 847.5 kHz subcarrier) for ISO/IEC 14443-A and -B, 15693 (NFC-V) and 18092 (NFC-F), and returns frames. This receiver names them,
// follows the anticollision exchange to the UID, ATQA and SAK, and keeps the cards and the frame log (nfc_tel.h). 2.5 Msps is the least
// that carries both subcarrier sidebands; 10 Msps is what nfc-laboratory itself runs at and what this mode asks for.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct NfcTelemetry;   // nfc_tel.h

class NfcReceiver {
public:
    NfcReceiver();
    ~NfcReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);   // unused by the decoder (it works on the magnitude): kept for the engine's contract
    void setCenterHz(double hz);       // the radio's centre (0: unknown, for files)
    bool ready() const;                // false when the input rate is too low for this mode
    void reset();                      // after a retune: the decoder starts over (the frames and cards stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(NfcTelemetry& out, uint64_t lastSeq);
    void clearResults();               // the user's Clear button: frames and cards
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setTechMask(int mask);        // 1 NFC-A, 2 NFC-B, 4 NFC-F, 8 NFC-V (default all); takes effect at once
    void setPowerThreshold(float mag); // the magnitude (full scale 1.0) below which there is no reader field; default 0.01

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning nfcTuning();

} // namespace dect2
