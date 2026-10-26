// FIS-B, the weather and aeronautical information US ground stations broadcast on 978 MHz alongside UAT: the product table, the APDU
// header, DLAC text, TWGO records (text with graphic overlay: NOTAMs, TFRs, AIRMETs, SIGMETs ...) and the reassembly of products sent in
// several segments.
//
// Written against two independent references that agree wherever they overlap: dump978's uat_decode.c (vendored here, the DLAC alphabet
// and the product names of DO-267A) and fisb-decode (rand-projects, BSD-2-Clause Plus Patent, which follows DO-358B and supplied the TWGO
// record layout, the segmentation block and the products added since DO-267A). Checked against fisb-decode's DO-358B test groups, encoded
// independently of anything here.
//
// Every byte here came off the air and may be damaged or hostile, so every read is bounded: a record that runs past its buffer is refused,
// never read beyond.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2::fisb {

// ---------------------------------------------------------------- products

// What a product is, as DO-358B numbers them; products of the older DO-267A numbering keep dump978's names.
const char* productName(uint16_t productId);

enum class Form {
    Text,       // DLAC text: 413 carries METAR, SPECI, TAF, PIREP and winds aloft, told apart by the text's own first word
    Twgo,       // text with graphic overlay: 8 NOTAM (-D, -FDC, -TFR), 11 AIRMET, 12 SIGMET, 13 SUA, 14 G-AIRMET, 15 CWA, 16 NOTAM-TRA, 17 NOTAM-TMOA
    Graphic,    // gridded images: NEXRAD, icing, cloud tops, turbulence, lightning
    Other,
};
Form productForm(uint16_t productId);

// ---------------------------------------------------------------- DLAC

// 6-bit text, four characters to three bytes. Code 28 is a tab whose next code is a count of spaces. Code 0 ends the text; codes 27 and 29
// are control codes with no printable form and are left out. Never reads past n.
std::string dlacDecode(const uint8_t* d, size_t n);

// ---------------------------------------------------------------- the APDU header

struct Apdu {
    uint16_t productId = 0;
    int timeOption = 0;          // what the header carries: 0 hours and minutes, 2 month, day, hours and minutes (the two FIS-B uses)
    bool hasMonthDay = false, hasSeconds = false;
    int month = 0, day = 0, hour = 0, minute = 0, second = 0;
    // A product too long for one frame is sent in segments (DO-358A and later: only TWGO products are). These say which file this belongs to.
    bool segmented = false;
    uint16_t fileId = 0;         // ties the segments of one product together, within one product id
    uint16_t fileSegments = 0;   // how many there are in all
    uint16_t segmentNumber = 0;  // which one this is, from 1
    const uint8_t* payload = nullptr;
    size_t payloadLen = 0;
};

// The header of a FIS-B APDU, `apdu` pointing at its first byte (the payload of a UAT uplink information frame of type 0). False when it is
// too short to hold the header it declares.
bool parseApdu(const uint8_t* apdu, size_t len, Apdu& out);

// ---------------------------------------------------------------- TWGO records

struct TextRecord {
    uint16_t reportNumber = 0;   // with the year and the product, what a graphic record is matched to
    uint8_t reportYear = 0;      // the last two digits; for NOTAM-D and NOTAM-TFR only the last one
    bool active = true;          // false: the report was cancelled, and carries no text
    std::string text;
};

// A point of a polygon or a polyline, or a single point (6 bytes on the air).
struct Vertex {
    double lat = 0, lon = 0;
    int altFt = 0;               // in hundreds of feet on the air; MSL or AGL as the record's geometry says
};

// A circle or an ellipse with a floor and a ceiling (14 bytes on the air): how a TFR around a point is sent.
struct Prism {
    double latBottom = 0, lonBottom = 0, latTop = 0, lonTop = 0;
    int bottomFt = 0, topFt = 0; // in steps of 500 ft on the air
    double rMajorNm = 0, rMinorNm = 0;   // in steps of 0.2 NM on the air; equal for a circle
    int alphaDeg = 0;            // the orientation of an ellipse
};

struct GraphicRecord {
    uint16_t reportNumber = 0;
    uint8_t reportYear = 0;
    int startYearOffset = 0, endYearOffset = 0;   // years to add to the report year for the start and the end
    int overlayRecordId = 0;
    std::string label;           // an airport id when present
    int objectElement = 0;       // 0 TFR, 1 TURB, 2 LLWS, 3 SFC, 4 ICING, 5 FRZLVL, 6 IFR, 7 MTN
    int objectType = 0;          // 0 airport, 14 airspace
    int objectStatus = 0;        // 13 cancelled, 15 in effect
    // 3 polygon MSL, 4 polygon AGL, 7 circular prism MSL, 8 circular prism AGL, 9 point AGL, 10 point MSL, 11 polyline MSL, 12 polyline AGL
    int geometry = 0;
    bool altitudesAgl() const { return geometry == 4 || geometry == 8 || geometry == 9 || geometry == 12; }
    bool cancelled() const { return objectStatus == 13; }
    int overlayOperator = 0;     // 1: this record and the others make one shape together (NOTAM-TRA and -TMOA)
    // When it applies, as month / day / hour / minute where the format sends them; -1 where it does not.
    int startMonth = -1, startDay = -1, startHour = -1, startMinute = -1;
    int endMonth = -1, endDay = -1, endHour = -1, endMinute = -1;
    std::vector<Vertex> vertices;
    std::vector<Prism> prisms;
};

struct Twgo {
    int recordFormat = 0;        // 2 text, 8 graphic
    std::string location;        // DLAC, up to four characters; empty for none
    int referencePoint = 0;      // 0x00 a facility, 0xFF external; anything else means the records are to be ignored
    std::vector<TextRecord> text;
    std::vector<GraphicRecord> graphics;
};

// Decodes the payload of a TWGO product (from its 6-byte header). False with *err when the header or a record does not fit: on the air
// that is damage, and a half-read record is worse than none.
bool decodeTwgo(const uint8_t* p, size_t n, uint16_t productId, Twgo& out, std::string* err = nullptr);

// ---------------------------------------------------------------- segments

// Collects the segments of products sent in several parts and hands back each product whole, once. A product is the first segment's
// payload followed by every later segment's payload less its repeated 6-byte TWGO header.
class Unsegmenter {
public:
    // Takes one segment. True, with the whole payload in `whole`, when this segment completes its product.
    bool add(const Apdu& a, double nowSec, std::vector<uint8_t>& whole);
    // Forgets products still waiting for a segment after maxAgeSec: a segment that never came is not coming.
    void expire(double nowSec, double maxAgeSec);
    size_t pending() const { return pend_.size(); }
    void clear() { pend_.clear(); }

private:
    struct Pending {
        uint16_t count = 0;
        double firstSec = 0;
        std::map<uint16_t, std::vector<uint8_t>> parts;   // by segment number
    };
    std::map<uint32_t, Pending> pend_;   // by product id and file id
};

}  // namespace dect2::fisb
