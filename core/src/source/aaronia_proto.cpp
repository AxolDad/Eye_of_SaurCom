// Aaronia RTSA-Suite HTTP stream, see aaronia_proto.h
#include "aaronia_proto.h"
#include "dect2/automation/json_mini.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace aaronia {

namespace js = dect2::automation::json;

double Header::rateHz() const {
    const double span = spanHz();
    const double dur = durationSec();
    // the span is the rate for complex sampling; when the timestamps give a usable figure too, they settle any doubt
    const double byTime = dur > 1e-9 && samples > 0 ? (double)samples / dur : 0;
    if (span > 0 && std::isfinite(span)) {
        if (byTime > 0 && (byTime > span * 1.5 || byTime < span / 1.5)) return byTime;   // they disagree: the timestamps win
        return span;
    }
    return byTime;
}

bool Header::plausible() const {
    if (!std::isfinite(startFreq) || !std::isfinite(endFreq) || endFreq < startFreq) return false;
    if (!std::isfinite(startTime) || !std::isfinite(endTime)) return false;
    return samples >= 0;
}

bool parseHeader(const std::string& text, Header& out) {
    out = Header();
    js::Value v;
    std::string err;
    if (!js::parse(text, v, err) || v.type != js::Value::Obj) return false;
    out.payload = v.str("payload");
    out.unit = v.str("unit");
    out.startTime = v.num("startTime");
    out.endTime = v.num("endTime");
    out.startFreq = v.num("startFrequency");
    out.endFreq = v.num("endFrequency");
    out.minPower = v.num("minPower");
    out.maxPower = v.num("maxPower");
    out.sampleSize = (int)v.num("sampleSize");
    out.sampleDepth = (int)v.num("sampleDepth");
    const double sc = v.num("scale", 1);
    out.scale = std::isfinite(sc) && sc != 0 ? sc : 1;
    // in a raw format "samples" is the number of IQ pairs; in the JSON format it is the array, which this reader does not take
    const js::Value* s = v.get("samples");
    if (s && s->type == js::Value::Num) out.samples = (int64_t)s->n;
    return !out.payload.empty();
}

float halfToFloat(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t man = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) bits = sign;                       // zero
        else {                                           // subnormal: shift it up until the leading bit falls out
            uint32_t e = 127 - 15 + 1, m = man;
            while (!(m & 0x400u)) { m <<= 1; e--; }
            bits = sign | (e << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);         // infinity or not-a-number
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

namespace {
// the document does not say the byte order; every machine RTSA-Suite runs on is little endian, and the file format is too (ASSUMED)
float readF32(const uint8_t* p) {
    uint32_t b = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    std::memcpy(&f, &b, sizeof f);
    return f;
}
uint16_t readU16(const uint8_t* p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
} // namespace

bool decodeSamples(const uint8_t* data, size_t len, size_t pairs, Format f, double scale, std::vector<cf32>& out) {
    const size_t step = (size_t)bytesPerValue(f);
    if (pairs == 0) return true;
    if (len < pairs * 2 * step) return false;
    out.reserve(out.size() + pairs);
    for (size_t i = 0; i < pairs; i++) {
        const uint8_t* p = data + i * 2 * step;
        float re, im;
        if (f == Format::Float32) { re = readF32(p); im = readF32(p + 4); }
        else if (f == Format::Float16) { re = halfToFloat(readU16(p)); im = halfToFloat(readU16(p + 2)); }
        else {
            re = (float)((double)(int16_t)readU16(p) * scale);
            im = (float)((double)(int16_t)readU16(p + 2) * scale);
        }
        if (!std::isfinite(re)) re = 0;
        if (!std::isfinite(im)) im = 0;
        out.emplace_back(re, im);
    }
    return true;
}

// ---- the packet reader ---------------------------------------------------------------------------------------------------------------

void Reader::feed(const uint8_t* p, size_t n) {
    if (buf_.size() + n > maxBuf_) {            // the stream has run away from us: drop what is held and start again at a packet boundary
        buf_.clear();
        bad_++;
    }
    buf_.insert(buf_.end(), p, p + n);
}

bool Reader::next(Header& h, std::vector<cf32>& iq) {
    iq.clear();
    // a packet begins at the first '{'; anything before it is the tail of a damaged one
    size_t start = 0;
    while (start < buf_.size() && buf_[start] != '{') start++;
    if (start) { buf_.erase(buf_.begin(), buf_.begin() + (ptrdiff_t)start); if (start) bad_++; }
    if (buf_.empty()) return false;
    // the header ends at the record separator; the line feed in front of it belongs to the header's line
    size_t rs = 0;
    bool found = false;
    for (size_t i = 0; i < buf_.size(); i++) if (buf_[i] == kRecordSeparator) { rs = i; found = true; break; }
    if (!found) return false;
    size_t jsonEnd = rs;
    while (jsonEnd > 0 && (buf_[jsonEnd - 1] == kLineFeed || buf_[jsonEnd - 1] == '\r')) jsonEnd--;
    const std::string json((const char*)buf_.data(), jsonEnd);
    Header hh;
    if (!parseHeader(json, hh) || !hh.plausible()) {   // not a packet we can read: step over it and look for the next
        buf_.erase(buf_.begin(), buf_.begin() + (ptrdiff_t)(rs + 1));
        bad_++;
        return false;
    }
    const size_t need = payloadBytes(hh, fmt_);
    if (buf_.size() < rs + 1 + need) return false;    // the binary part has not all arrived
    if (need) decodeSamples(buf_.data() + rs + 1, need, (size_t)hh.samples, fmt_, hh.scale, iq);
    buf_.erase(buf_.begin(), buf_.begin() + (ptrdiff_t)(rs + 1 + need));
    h = hh;
    return true;
}

uint64_t gapSamples(const Header& prev, const Header& now, double rate) {
    if (rate <= 0 || prev.endTime <= 0 || now.startTime <= 0) return 0;
    const double gap = now.startTime - prev.endTime;
    if (!std::isfinite(gap) || gap <= 0) return 0;
    const double tol = 1.5 / rate;                    // a sample or so of jitter in the timestamps is not a loss
    if (gap <= tol) return 0;
    const double n = gap * rate;
    return n > 0 && n < 1e12 ? (uint64_t)n : 0;
}

// ---- control -------------------------------------------------------------------------------------------------------------------------

std::string tuneBody(double centreHz, double spanHz) {
    char b[160];
    snprintf(b, sizeof b, "{\"frequencyCenter\":%.6f,\"frequencySpan\":%.6f,\"type\":\"capture\"}", centreHz, spanHz);
    return b;
}

std::string streamingBody(bool start) {
    return std::string("{\"start\":") + (start ? "true" : "false") + ",\"type\":\"streaming\"}";
}

std::string streamPath(Format f, int rateReduction, const std::string& input) {
    std::string p = std::string("/stream?format=") + formatName(f);
    if (rateReduction > 1) p += "&rate_reduction=" + std::to_string(rateReduction);
    if (!input.empty() && input != "main") {
        p += "&input=";
        for (unsigned char c : input) {                // the input name can be a brace-wrapped uuid
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') p += (char)c;
            else { char e[4]; snprintf(e, sizeof e, "%%%02X", c); p += e; }
        }
    }
    return p;
}

std::string httpRequest(const std::string& method, const std::string& path, const std::string& host, const std::string& body,
                        const std::string& auth) {
    std::string r = method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: EyeOfZorCom\r\nAccept: */*\r\n";
    if (!auth.empty()) r += "Authorization: " + auth + "\r\n";
    if (!body.empty()) r += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    r += "Connection: keep-alive\r\n\r\n";
    return r + body;
}

bool httpResponse(const std::string& text, int& status, std::string& body, size_t& headerLen, bool& chunked) {
    const size_t end = text.find("\r\n\r\n");
    if (end == std::string::npos) return false;
    headerLen = end + 4;
    const std::string head = text.substr(0, end);
    status = 0;
    if (head.compare(0, 5, "HTTP/") == 0) {
        const size_t sp = head.find(' ');
        if (sp != std::string::npos) status = atoi(head.c_str() + sp + 1);
    }
    std::string lower;
    lower.reserve(head.size());
    for (char c : head) lower += (char)tolower((unsigned char)c);
    chunked = lower.find("transfer-encoding: chunked") != std::string::npos;
    body = text.substr(headerLen);
    return true;
}

bool chunkedNext(const std::string& text, size_t& pos, std::string& out, bool& done) {
    done = false;
    const size_t eol = text.find("\r\n", pos);
    if (eol == std::string::npos) return false;
    const std::string line = text.substr(pos, eol - pos);
    const long n = strtol(line.c_str(), nullptr, 16);
    if (n < 0) { done = true; return false; }
    if (text.size() < eol + 2 + (size_t)n + 2) return false;
    out.assign(text, eol + 2, (size_t)n);
    pos = eol + 2 + (size_t)n + 2;
    if (n == 0) done = true;
    return true;
}

} // namespace aaronia
} // namespace dect2
