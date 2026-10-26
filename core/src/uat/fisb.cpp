// FIS-B products, APDU header, DLAC, TWGO and segment reassembly: see fisb.h
#include "dect2/uat/fisb.h"
#include <algorithm>

namespace dect2::fisb {
namespace {

// Reads bits most significant first, and says so rather than read past the end.
class BitReader {
public:
    BitReader(const uint8_t* d, size_t n) : d_(d), nbits_(n * 8) {}
    bool get(size_t pos, int count, uint32_t& v) const {
        if (count < 0 || count > 32 || pos + (size_t)count > nbits_) return false;
        v = 0;
        for (int i = 0; i < count; i++) {
            const size_t b = pos + (size_t)i;
            v = (v << 1) | ((d_[b >> 3] >> (7 - (b & 7))) & 1u);
        }
        return true;
    }

private:
    const uint8_t* d_;
    size_t nbits_;
};

// A raw coordinate of `bits` bits, as a fraction of a full circle. Longitudes past 180 and latitudes past 180 are west and south: the
// encoding is the two's complement of the angle over the circle. fisb-decode turns latitudes past 90 by 180 instead; the two agree on every
// latitude FIS-B can carry (it is broadcast over the United States only, 18 to 72 degrees north), and this is the one that holds south.
double angleOf(uint32_t raw, int bits) {
    double v = (double)raw * 360.0 / (double)(1u << bits);
    if (v >= 180.0) v -= 360.0;
    return v;
}

}  // namespace

// ---------------------------------------------------------------- products

const char* productName(uint16_t id) {
    switch (id) {
    // DO-358B: what FIS-B ground stations broadcast now
    case 8: return "NOTAM";
    case 11: return "AIRMET";
    case 12: return "SIGMET";
    case 13: return "SUA status";
    case 14: return "G-AIRMET";
    case 15: return "CWA";
    case 16: return "NOTAM-TRA";
    case 17: return "NOTAM-TMOA";
    case 63: return "NEXRAD regional";
    case 64: return "NEXRAD CONUS";
    case 70: return "Icing (low)";
    case 71: return "Icing (high)";
    case 84: return "Cloud tops";
    case 90: return "Turbulence (low)";
    case 91: return "Turbulence (high)";
    case 103: return "Lightning";
    case 413: return "Text";   // METAR, SPECI, TAF, PIREP, winds aloft: the text names itself
    // DO-267A numbering, as dump978 names them
    case 0: case 20: return "METAR";
    case 1: case 21: return "TAF";
    case 2: case 22: return "SIGMET";
    case 3: case 23: return "Convective SIGMET";
    case 4: case 24: return "AIRMET";
    case 5: case 25: return "PIREP";
    case 6: case 26: return "AWW";
    case 7: case 27: return "Winds aloft";
    case 9: return "D-ATIS";
    case 10: return "TWIP";
    case 351: return "System time";
    case 352: return "Operational status";
    case 353: return "Ground station status";
    case 411: return "Text";
    default: return "";
    }
}

Form productForm(uint16_t id) {
    if (id == 413 || id == 411) return Form::Text;
    if (id == 8 || (id >= 11 && id <= 17)) return Form::Twgo;
    if (id == 63 || id == 64 || id == 70 || id == 71 || id == 84 || id == 90 || id == 91 || id == 103) return Form::Graphic;
    return Form::Other;
}

// ---------------------------------------------------------------- DLAC

std::string dlacDecode(const uint8_t* d, size_t n) {
    // The alphabet of dump978's decode_dlac and fisb-decode's dlacString, which agree on every printable character. 0 ends the text, 27 and
    // 29 are control codes with nothing to print, 28 is a tab and 30 a new line.
    static const char kAlpha[64] = {
        0,   'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
        'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 0,   0,   0,   '\n', '|',
        ' ', '!', '"', '#', '$', '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
    };
    std::string out;
    if (!d) return out;
    out.reserve(n * 4 / 3);
    const BitReader br(d, n);
    const size_t chars = n * 8 / 6;
    bool tab = false;
    for (size_t i = 0; i < chars; i++) {
        uint32_t c = 0;
        if (!br.get(i * 6, 6, c)) break;
        if (tab) {
            out.append(c, ' ');
            tab = false;
        } else if (c == 0) {
            break;
        } else if (c == 28) {
            tab = true;
        } else if (kAlpha[c]) {
            out += kAlpha[c];
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    return out;
}

// ---------------------------------------------------------------- the APDU header

bool parseApdu(const uint8_t* apdu, size_t len, Apdu& out) {
    out = Apdu();
    if (!apdu || len < 4) return false;
    const BitReader br(apdu, len);
    uint32_t v = 0;
    br.get(3, 11, v); out.productId = (uint16_t)v;
    br.get(14, 1, v); out.segmented = v != 0;
    br.get(15, 2, v); out.timeOption = (int)v;
    size_t pos = 17;
    auto take = [&](int bits, int& field) {
        uint32_t x = 0;
        if (!br.get(pos, bits, x)) return false;
        field = (int)x;
        pos += (size_t)bits;
        return true;
    };
    // Time options 0 and 2 are the ones FIS-B sends (hours and minutes, with month and day for 2); every TWGO product uses 2. Options 1 and
    // 3 follow dump978, adding seconds; fisb-decode reads them as month and day without seconds instead, and no test group sends either.
    out.hasMonthDay = out.timeOption >= 2;
    out.hasSeconds = out.timeOption == 1 || out.timeOption == 3;
    if (out.hasMonthDay && (!take(4, out.month) || !take(5, out.day))) return false;
    if (!take(5, out.hour) || !take(6, out.minute)) return false;
    if (out.hasSeconds && !take(6, out.second)) return false;
    if (out.segmented) {
        // DO-358A segmentation block: product file id, how many segments, and which one this is. dump978 predates it and reads the payload
        // from where this block starts.
        int fid = 0, cnt = 0, num = 0;
        if (!take(10, fid) || !take(9, cnt) || !take(9, num)) return false;
        out.fileId = (uint16_t)fid;
        out.fileSegments = (uint16_t)cnt;
        out.segmentNumber = (uint16_t)num;
    }
    const size_t start = (pos + 7) / 8;
    if (start > len) return false;
    out.payload = apdu + start;
    out.payloadLen = len - start;
    return true;
}

// ---------------------------------------------------------------- TWGO records

namespace {

bool fail(std::string* err, const char* m) {
    if (err) *err = m;
    return false;
}

bool textRecords(const uint8_t* p, size_t n, int count, Twgo& out, std::string* err) {
    size_t ros = 0;
    for (int i = 0; i < count; i++) {
        if (ros + 5 > n) return fail(err, "a text record runs past the product");
        const uint8_t* r = p + ros;
        const size_t len = ((size_t)r[0] << 8) | r[1];   // includes its own 5 header bytes
        if (len < 5 || ros + len > n) return fail(err, "a text record's length does not fit the product");
        TextRecord t;
        t.reportNumber = (uint16_t)(((unsigned)r[2] << 6) | (r[3] >> 2));
        t.reportYear = (uint8_t)(((r[3] & 0x03) << 5) | ((r[4] & 0xF8) >> 3));
        // This record's own status byte. fisb-decode reads byte 4 of the whole payload here, which is right for the first record only.
        t.active = ((r[4] >> 2) & 1) != 0;
        if (t.active) t.text = dlacDecode(r + 5, len - 5);
        out.text.push_back(std::move(t));
        ros += len;
    }
    return true;
}

bool readTime(const uint8_t* p, size_t n, size_t& ros, int fmt, int& mo, int& dy, int& hr, int& mi) {
    const size_t need = fmt == 1 ? 4 : fmt == 2 ? 3 : fmt == 3 ? 2 : 0;
    if (ros + need > n) return false;
    if (fmt == 1) { mo = p[ros]; dy = p[ros + 1]; hr = p[ros + 2]; mi = p[ros + 3]; }
    else if (fmt == 2) { dy = p[ros]; hr = p[ros + 1]; mi = p[ros + 2]; }
    else if (fmt == 3) { hr = p[ros]; mi = p[ros + 1]; }
    ros += need;
    return true;
}

bool graphicRecords(const uint8_t* p, size_t n, int count, uint16_t productId, Twgo& out, std::string* err) {
    size_t os = 0;
    for (int i = 0; i < count; i++) {
        if (os + 5 > n) return fail(err, "a graphic record runs past the product");
        const uint8_t* r = p + os;
        const size_t recLen = ((size_t)r[0] << 2) | ((r[1] & 0xC0) >> 6);
        if (recLen < 5 || os + recLen > n) return fail(err, "a graphic record's length does not fit the product");
        GraphicRecord g;
        g.reportNumber = (uint16_t)(((r[1] & 0x3F) << 8) | r[2]);
        g.reportYear = (uint8_t)(r[3] >> 1);
        g.startYearOffset = ((r[3] & 0x01) << 1) | ((r[4] & 0x80) >> 7);
        g.endYearOffset = (r[4] & 0x60) >> 5;
        g.overlayRecordId = ((r[4] & 0x1E) >> 1) + 1;
        const bool labelFlag = (r[4] & 0x01) != 0;
        // Everything below stays inside this record: a field that would run past it means the record is damaged.
        const size_t end = os + recLen;
        size_t ros = os + 5;
        bool ok = true;
        auto need = [&](size_t k) { if (ros + k > end) ok = false; return ok; };
        if (!labelFlag) {
            if (need(2)) ros += 2;
        } else if (need(9)) {
            g.label = dlacDecode(p + ros, 9);
            ros += 9;
        }
        int qualFlag = 0, paramFlag = 0, applicability = 0, dtFormat = 0, vertexCount = 0;
        if (ok && need(2)) {
            qualFlag = (p[ros] >> 6) & 1;
            paramFlag = (p[ros] >> 5) & 1;
            g.objectElement = p[ros] & 0x1F;
            g.objectType = (p[ros + 1] & 0xF0) >> 4;
            g.objectStatus = p[ros + 1] & 0x0F;
            ros += 2;
        }
        if (ok && productId == 14 && qualFlag && need(3)) ros += 3;   // G-AIRMET qualifiers
        if (ok && paramFlag && need(2)) ros += 2;
        if (ok && need(2)) {
            applicability = (p[ros] & 0xC0) >> 6;
            dtFormat = (p[ros] & 0x30) >> 4;
            g.geometry = p[ros] & 0x0F;
            g.overlayOperator = (p[ros + 1] & 0xC0) >> 6;
            vertexCount = g.geometry != 0 ? (p[ros + 1] & 0x3F) + 1 : 0;
            ros += 2;
        }
        if (ok && (applicability == 1 || applicability == 3))
            ok = readTime(p, end, ros, dtFormat, g.startMonth, g.startDay, g.startHour, g.startMinute);
        if (ok && (applicability == 2 || applicability == 3))
            ok = readTime(p, end, ros, dtFormat, g.endMonth, g.endDay, g.endHour, g.endMinute);
        const bool prism = g.geometry == 7 || g.geometry == 8;
        const bool point = g.geometry == 3 || g.geometry == 4 || (g.geometry >= 9 && g.geometry <= 12);
        // A geometry or an overlay operator this does not know (the retired ellipses, the 'NOT' operator) is skipped as a whole record: its
        // length is known, so the records after it can still be read.
        const bool known = g.geometry == 0 || prism || point;
        if (ok && known && g.overlayOperator <= 1) {
            for (int v = 0; v < vertexCount && ok; v++) {
                if (prism) {
                    if (!need(14)) break;
                    const uint8_t* b = p + ros;
                    Prism z;
                    z.lonBottom = angleOf(((uint32_t)b[0] << 10) | ((uint32_t)b[1] << 2) | ((b[2] & 0xC0) >> 6), 18);
                    z.latBottom = angleOf(((uint32_t)(b[2] & 0x3F) << 12) | ((uint32_t)b[3] << 4) | ((b[4] & 0xF0) >> 4), 18);
                    z.lonTop = angleOf(((uint32_t)(b[4] & 0x0F) << 14) | ((uint32_t)b[5] << 6) | ((b[6] & 0xFC) >> 2), 18);
                    z.latTop = angleOf(((uint32_t)(b[6] & 0x03) << 16) | ((uint32_t)b[7] << 8) | b[8], 18);
                    z.bottomFt = ((b[9] & 0xFE) >> 1) * 500;
                    z.topFt = (((b[9] & 0x01) << 6) | ((b[10] & 0xFC) >> 2)) * 500;
                    z.rMajorNm = (((b[10] & 0x03) << 7) | ((b[11] & 0xFE) >> 1)) * 0.2;
                    z.rMinorNm = (((b[11] & 0x01) << 8) | b[12]) * 0.2;
                    z.alphaDeg = b[13];
                    g.prisms.push_back(z);
                    ros += 14;
                } else {
                    if (!need(6)) break;
                    const uint8_t* b = p + ros;
                    Vertex x;
                    x.lon = angleOf(((uint32_t)b[0] << 11) | ((uint32_t)b[1] << 3) | ((b[2] & 0xE0) >> 5), 19);
                    x.lat = angleOf(((uint32_t)(b[2] & 0x1F) << 14) | ((uint32_t)b[3] << 6) | ((b[4] & 0xFC) >> 2), 19);
                    x.altFt = (((b[4] & 0x03) << 8) | b[5]) * 100;
                    g.vertices.push_back(x);
                    ros += 6;
                }
            }
            if (ok) out.graphics.push_back(std::move(g));
        }
        os = end;
    }
    return true;
}

}  // namespace

bool decodeTwgo(const uint8_t* p, size_t n, uint16_t productId, Twgo& out, std::string* err) {
    out = Twgo();
    if (!p || n < 6) return fail(err, "too short for a TWGO header");
    out.recordFormat = (p[0] & 0xF0) >> 4;
    const int count = (p[1] & 0xF0) >> 4;
    out.location = dlacDecode(p + 2, 3);
    out.referencePoint = p[5];
    if (out.recordFormat == 2) return textRecords(p + 6, n - 6, count, out, err);
    if (out.recordFormat == 8) return graphicRecords(p + 6, n - 6, count, productId, out, err);
    return fail(err, "not a text or graphic TWGO record");
}

// ---------------------------------------------------------------- segments

bool Unsegmenter::add(const Apdu& a, double nowSec, std::vector<uint8_t>& whole) {
    whole.clear();
    if (!a.segmented || !a.payload) return false;
    // A segment number outside 1 .. the count is damage, and a count past 64 cannot be one TWGO product: either would grow this for nothing.
    if (a.fileSegments < 1 || a.fileSegments > 64 || a.segmentNumber < 1 || a.segmentNumber > a.fileSegments) return false;
    const uint32_t key = ((uint32_t)a.productId << 16) | a.fileId;
    auto it = pend_.find(key);
    if (it != pend_.end() && it->second.count != a.fileSegments) {
        pend_.erase(it);   // the same file id now promises a different count: a new product reusing the id
        it = pend_.end();
    }
    if (it == pend_.end()) {
        if (pend_.size() >= 256) return false;   // bounded, whatever arrives
        Pending p;
        p.count = a.fileSegments;
        p.firstSec = nowSec;
        it = pend_.emplace(key, std::move(p)).first;
    }
    it->second.parts[a.segmentNumber].assign(a.payload, a.payload + a.payloadLen);
    Pending& p = it->second;
    if (p.parts.size() < p.count) return false;
    for (uint16_t i = 1; i <= p.count; i++)
        if (!p.parts.count(i)) return false;
    // The first segment whole, then each later one less the TWGO header every segment repeats.
    whole = p.parts[1];
    bool fine = true;
    for (uint16_t i = 2; i <= p.count; i++) {
        const std::vector<uint8_t>& s = p.parts[i];
        if (s.size() < 6) { fine = false; break; }
        whole.insert(whole.end(), s.begin() + 6, s.end());
    }
    pend_.erase(it);
    if (!fine) whole.clear();
    return fine;
}

void Unsegmenter::expire(double nowSec, double maxAgeSec) {
    for (auto it = pend_.begin(); it != pend_.end();)
        it = nowSec - it->second.firstSec > maxAgeSec ? pend_.erase(it) : std::next(it);
}

}  // namespace dect2::fisb
