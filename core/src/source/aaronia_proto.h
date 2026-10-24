// Aaronia RTSA-Suite HTTP stream: the wire pieces without a socket (source_aaronia.cpp, tests/source/test_aaronia.cpp). The Spectran V6
// (and the other radios RTSA-Suite drives) is not reached directly: RTSA-Suite runs the hardware and an "HTTP Server" block in its graph
// serves the samples, so this is a client for that block.
//
// Written from the FACTS in Aaronia's own "RTSA HTTP Stream Server Endpoints" document (v6-forum.aaronia.de, revision 11); no code was
// copied from any implementation. VERIFIED = stated in that document; ASSUMED = inferred, marked where it matters.
//  - GET /stream?format=float32 streams with chunked transfer. "Each sample packet is encoded as individual JSON data separated by a line
//    feed (ASCII 10) and a record separator (ASCII 30) character."                                                          (VERIFIED)
//  - The combined raw format "transmits alternating pairs of JSON meta data and binary sample data as 16bit signed integers, 16bit floats
//    or 32bit floats. The binary part starts behind the record separator character."                                        (VERIFIED)
//  - In that raw format the JSON "samples" element "contains the number of samples (e.g. spectra or IQ pairs)".             (VERIFIED)
//  - Header fields: startTime, endTime (seconds since 1970), unit, payload, minPower, maxPower, startFrequency, endFrequency,
//    sampleDepth, sampleSize, samples, scale (integer formats only).                                                        (VERIFIED)
//  - IQ packets carry payload "iq" and "IQ Samples are transmitted as a flat array of alternating I and Q values".          (VERIFIED)
//  - There is NO sample-rate field. For complex IQ the rate equals the span, endFrequency - startFrequency, and the centre is the middle
//    of the two.                                                                                                            (ASSUMED, cross-checked against samples / (endTime - startTime))
//  - "The RTSA HTTP server block will start dropping data when the outbound TCP buffer exceeds 8 Mbytes. A loss of data can be determined
//    by comparing the timestamps of two adjacent data packets."                                                             (VERIFIED)
//  - float16 "follows the IEEE 754-2008 specification for half precision with five exponent and ten mantissa bits".         (VERIFIED)
//  - int16 carries a "scale value ... in the JSON metadata section to convert from the integer format to float values".     (VERIFIED)
//  - Tuning goes to PUT /control as {"frequencyCenter": Hz, "frequencySpan": Hz, "type": "capture"}, and the graph is started and stopped
//    with {"start": true|false, "type": "streaming"}.                                                                       (VERIFIED)
//  - The documented examples all listen on port 54664; the port is set on the HTTP Server block, so it is only a default.   (VERIFIED as the example, ASSUMED as the default)
#pragma once
#include "dect2/common/ring.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dect2 {
namespace aaronia {

constexpr int kDefaultPort = 54664;          // the port every example in Aaronia's document uses
constexpr uint8_t kRecordSeparator = 0x1E;   // ASCII 30, after a line feed
constexpr uint8_t kLineFeed = 0x0A;

// The sample format asked of /stream. Float32 is the plain one; int16 halves the bytes and carries a scale in the header.
enum class Format { Float32, Float16, Int16 };
inline const char* formatName(Format f) { return f == Format::Int16 ? "int16" : f == Format::Float16 ? "float16" : "float32"; }
inline int bytesPerValue(Format f) { return f == Format::Float32 ? 4 : 2; }

// One packet's metadata, as the JSON header gives it.
struct Header {
    double startTime = 0, endTime = 0;       // seconds since 1970
    double startFreq = 0, endFreq = 0;       // Hz
    double minPower = 0, maxPower = 0;       // dBm (a source that is not calibrated reports unit "generic")
    double scale = 1;                        // int16 only: multiply the integers by this
    int64_t samples = 0;                     // raw formats: the number of IQ pairs in the binary part
    int sampleSize = 0, sampleDepth = 0;
    std::string payload, unit;
    bool iq() const { return payload == "iq"; }
    double centreHz() const { return 0.5 * (startFreq + endFreq); }
    double spanHz() const { return endFreq - startFreq; }
    double durationSec() const { return endTime - startTime; }
    // The rate the packet implies: the span for complex IQ, cross-checked against what the timestamps say. 0 when neither is usable.
    double rateHz() const;
    bool plausible() const;
};

// Reads the JSON header of one packet. False when the text is not an object or carries no payload type.
bool parseHeader(const std::string& json, Header& out);

// How many bytes of binary follow a raw-format header: IQ pairs times two values times the value size.
inline size_t payloadBytes(const Header& h, Format f) {
    return h.samples > 0 ? (size_t)h.samples * 2u * (size_t)bytesPerValue(f) : 0;
}

// IEEE 754-2008 half precision to float.
float halfToFloat(uint16_t h);

// Turns one packet's binary part into complex samples, appended to out. n is the number of IQ pairs. For Int16 the header's scale is
// applied. False when there are not enough bytes.
bool decodeSamples(const uint8_t* data, size_t len, size_t pairs, Format f, double scale, std::vector<cf32>& out);

// ---- the packet reader ---------------------------------------------------------------------------------------------------------------
// Feed it whatever arrives from the socket; it hands back whole packets. A packet is a JSON header, then the line feed and record
// separator, then (for a raw format) the binary samples. It keeps a partial packet until the rest turns up.
class Reader {
public:
    void reset() { buf_.clear(); }
    void setFormat(Format f) { fmt_ = f; }
    Format format() const { return fmt_; }
    void setMaxBuffer(size_t bytes) { maxBuf_ = bytes; }
    // Adds bytes to the buffer.
    void feed(const uint8_t* p, size_t n);
    // Takes the next whole packet. False when more bytes are needed. The samples are appended to iq (cleared first).
    bool next(Header& h, std::vector<cf32>& iq);
    size_t buffered() const { return buf_.size(); }
    uint64_t badPackets() const { return bad_; }
private:
    std::vector<uint8_t> buf_;
    Format fmt_ = Format::Float32;
    size_t maxBuf_ = 64u << 20;
    uint64_t bad_ = 0;
};

// How many samples went missing between two packets: the gap between the end of one and the start of the next, in samples. 0 when they
// follow on, or when the times make no sense. (Aaronia's document: the server drops data past an 8 MB outbound buffer, and a loss "can be
// determined by comparing the timestamps of two adjacent data packets".)
uint64_t gapSamples(const Header& prev, const Header& now, double rateHz);

// ---- control -------------------------------------------------------------------------------------------------------------------------
// The body of PUT /control that sets the capture's centre and span.
std::string tuneBody(double centreHz, double spanHz);
// The body of PUT /control that starts or stops the streaming blocks.
std::string streamingBody(bool start);
// The path and query of the stream request.
std::string streamPath(Format f, int rateReduction = 0, const std::string& input = {});
// A minimal HTTP/1.1 request. auth, when set, is the value of the Authorization header ("Basic ..." or "RToken ...").
std::string httpRequest(const std::string& method, const std::string& path, const std::string& host, const std::string& body = {},
                        const std::string& auth = {});
// Splits a response into its status code, headers and the body already received. False when the headers are not complete yet.
bool httpResponse(const std::string& text, int& status, std::string& body, size_t& headerLen, bool& chunked);
// Takes one chunk off a chunked body. State is the offset into text. Returns false when more bytes are needed; done is set at the last chunk.
bool chunkedNext(const std::string& text, size_t& pos, std::string& out, bool& done);

} // namespace aaronia
} // namespace dect2
