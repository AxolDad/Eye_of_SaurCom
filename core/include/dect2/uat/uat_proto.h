// UAT message decode: ADS-B reports, FIS-B text, TIS-B relay (C decode from dump978).
#pragma once
#include "uat_tel.h"
#include "uat_demod.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct UatDecoded {
    enum class Kind { Unknown, Adsb, Tisb, Fisb, Other };
    Kind kind = Kind::Unknown;
    UatAircraft ac;
    UatFisbProduct fisb;         // the first product that could be read from this frame alone
    // Every FIS-B APDU of an uplink frame, as it came: a frame usually carries several. The tracker reads them, because a product in
    // segments, and the text and graphic halves of a NOTAM, only make sense across frames.
    std::vector<std::vector<uint8_t>> fisbApdus;
    std::string summary;
    std::string hex;
};

// Apply a demodulated downlink or uplink frame to produce UI-facing fields.
bool uatDecodeFrame(const UatRawFrame& raw, UatDecoded& out);

std::string uatFisbProductName(uint16_t productId);

// The products in one whole FIS-B payload (a single APDU, or a product put back together from its segments): one per DLAC text, one per
// TWGO text record and one per TWGO graphic record. Damaged payloads
// yield nothing rather than something half read.
void uatFisbProducts(uint16_t productId, const uint8_t* payload, size_t n, std::vector<UatFisbProduct>& out);

} // namespace dect2
