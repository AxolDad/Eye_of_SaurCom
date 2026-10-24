// Aaronia RTSA-Suite HTTP stream (core/src/source/aaronia_proto.cpp): the packet reader on streams built to the shapes in Aaronia's
// "RTSA HTTP Stream Server Endpoints" document, the three sample formats, the gap detection the document describes, and the requests.
#include "../../core/src/source/aaronia_proto.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::aaronia;
#include "../common/test_util.h"

// One packet as the server sends it: the JSON header, a line feed and the record separator, then the binary samples.
static void append(std::vector<uint8_t>& s, const std::string& json, const std::vector<uint8_t>& bin) {
    s.insert(s.end(), json.begin(), json.end());
    s.push_back(kLineFeed);
    s.push_back(kRecordSeparator);
    s.insert(s.end(), bin.begin(), bin.end());
}

static std::string iqHeader(int pairs, double t0, double dur, double f0, double f1, const char* extra = "") {
    char b[400];
    snprintf(b, sizeof b,
             "{\"startTime\":%.6f,\"endTime\":%.6f,\"unit\":\"generic\",\"payload\":\"iq\",\"minPower\":-2,\"maxPower\":2,"
             "\"startFrequency\":%.1f,\"endFrequency\":%.1f,\"sampleDepth\":1,\"sampleSize\":2,\"samples\":%d%s}",
             t0, t0 + dur, f0, f1, pairs, extra);
    return b;
}

static std::vector<uint8_t> f32Bytes(const std::vector<float>& v) {
    std::vector<uint8_t> b(v.size() * 4);
    for (size_t i = 0; i < v.size(); i++) {
        uint32_t u;
        std::memcpy(&u, &v[i], 4);
        b[i * 4] = (uint8_t)u; b[i * 4 + 1] = (uint8_t)(u >> 8); b[i * 4 + 2] = (uint8_t)(u >> 16); b[i * 4 + 3] = (uint8_t)(u >> 24);
    }
    return b;
}
static std::vector<uint8_t> u16Bytes(const std::vector<uint16_t>& v) {
    std::vector<uint8_t> b(v.size() * 2);
    for (size_t i = 0; i < v.size(); i++) { b[i * 2] = (uint8_t)v[i]; b[i * 2 + 1] = (uint8_t)(v[i] >> 8); }
    return b;
}
// a float to its IEEE 754-2008 half-precision bits (test side only, so the decoder is checked against something independent)
static uint16_t toHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int exp = (int)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t man = x & 0x7FFFFFu;
    if (exp <= 0) return (uint16_t)sign;
    if (exp >= 31) return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (man >> 13));
}

int main() {
    // the header of an IQ packet, with the fields the document lists
    {
        Header h;
        CHECK(parseHeader(iqHeader(512, 1501163970.1396854, 0.002048, 2400250000.0, 2400500000.0), h), "parse");
        CHECK(h.iq() && h.unit == "generic" && h.samples == 512 && h.sampleSize == 2 && h.sampleDepth == 1, "fields: %s %lld", h.payload.c_str(), (long long)h.samples);
        CHECK(std::fabs(h.centreHz() - 2400375000.0) < 1e-6, "centre %.1f", h.centreHz());
        CHECK(std::fabs(h.spanHz() - 250000.0) < 1e-6, "span %.1f", h.spanHz());
        CHECK(std::fabs(h.rateHz() - 250000.0) < 1.0, "rate %.1f", h.rateHz());
        CHECK(std::fabs(h.minPower + 2) < 1e-9 && std::fabs(h.maxPower - 2) < 1e-9, "power range");
        CHECK(h.plausible(), "plausible");
        // a spectrum packet is not IQ
        Header s;
        CHECK(parseHeader("{\"payload\":\"spectra\",\"startFrequency\":1,\"endFrequency\":2,\"sampleSize\":448}", s) && !s.iq(), "spectra");
        // rubbish and a header with no payload type are refused
        Header x;
        CHECK(!parseHeader("not json", x) && !parseHeader("{\"startFrequency\":1}", x) && !parseHeader("[1,2]", x), "refused");
        // when the timestamps and the span disagree badly, the timestamps are believed
        Header d;
        CHECK(parseHeader(iqHeader(1000, 100.0, 1.0, 1e9, 1e9 + 1e6), d), "parse 2");
        CHECK(std::fabs(d.rateHz() - 1000.0) < 1e-6, "timestamps win: %.1f", d.rateHz());
    }
    // float32: a whole packet through the reader
    {
        const std::vector<float> v = {0.5f, -0.25f, 1.0f, 0.0f, -1.0f, 0.125f};   // three IQ pairs
        std::vector<uint8_t> wire;
        append(wire, iqHeader(3, 10.0, 3.0 / 48000, 100e6 - 24000, 100e6 + 24000), f32Bytes(v));
        Reader r;
        r.setFormat(Format::Float32);
        r.feed(wire.data(), wire.size());
        Header h;
        std::vector<cf32> iq;
        CHECK(r.next(h, iq), "one packet");
        CHECK(iq.size() == 3, "three pairs: %zu", iq.size());
        CHECK(iq[0] == cf32(0.5f, -0.25f) && iq[1] == cf32(1.0f, 0.f) && iq[2] == cf32(-1.0f, 0.125f), "values");
        CHECK(std::fabs(h.centreHz() - 100e6) < 1e-6 && std::fabs(h.rateHz() - 48000) < 1, "centre %.0f rate %.0f", h.centreHz(), h.rateHz());
        CHECK(!r.next(h, iq) && r.buffered() == 0, "nothing left over");
    }
    // the bytes arriving in dribs and drabs: a packet comes out only once all of it is there
    {
        const std::vector<float> v = {1.f, 2.f, 3.f, 4.f};
        std::vector<uint8_t> wire;
        append(wire, iqHeader(2, 0, 1, 0, 2000), f32Bytes(v));
        Reader r;
        Header h;
        std::vector<cf32> iq;
        for (size_t i = 0; i + 1 < wire.size(); i++) {
            r.feed(wire.data() + i, 1);
            CHECK(!r.next(h, iq), "not yet at byte %zu", i);
        }
        r.feed(wire.data() + wire.size() - 1, 1);
        CHECK(r.next(h, iq) && iq.size() == 2 && iq[0] == cf32(1, 2) && iq[1] == cf32(3, 4), "the last byte completes it");
    }
    // several packets back to back, as the chunked stream delivers them
    {
        std::vector<uint8_t> wire;
        for (int k = 0; k < 5; k++) append(wire, iqHeader(2, k * 0.001, 0.001, 1e9 - 1000, 1e9 + 1000), f32Bytes({(float)k, 0.f, 0.f, (float)k}));
        Reader r;
        r.feed(wire.data(), wire.size());
        Header h, prev;
        std::vector<cf32> iq;
        int n = 0;
        uint64_t lost = 0;
        while (r.next(h, iq)) {
            CHECK(iq.size() == 2 && iq[0].real() == (float)n, "packet %d", n);
            if (n) lost += gapSamples(prev, h, h.rateHz());
            prev = h;
            n++;
        }
        CHECK(n == 5 && r.buffered() == 0, "five packets: %d", n);
        CHECK(lost == 0, "back to back, nothing lost: %llu", (unsigned long long)lost);
    }
    // the gap the document says marks dropped data (the server drops past an 8 MB outbound buffer)
    {
        Header a, b;
        parseHeader(iqHeader(1000, 10.0, 0.5, 0, 2000), a);      // ends at 10.5
        parseHeader(iqHeader(1000, 11.0, 0.5, 0, 2000), b);      // starts at 11.0: half a second missing
        CHECK(gapSamples(a, b, 2000.0) == 1000, "half a second at 2000 Hz: %llu", (unsigned long long)gapSamples(a, b, 2000.0));
        Header c;
        parseHeader(iqHeader(1000, 10.5, 0.5, 0, 2000), c);      // follows on
        CHECK(gapSamples(a, c, 2000.0) == 0, "no gap");
        CHECK(gapSamples(a, a, 2000.0) == 0 && gapSamples(a, b, 0) == 0, "nonsense gives 0");
        Header back;
        parseHeader(iqHeader(1000, 9.0, 0.5, 0, 2000), back);     // time running backwards is not a loss
        CHECK(gapSamples(a, back, 2000.0) == 0, "backwards");
    }
    // int16 with the scale the header carries, and float16
    {
        std::vector<uint8_t> wire;
        append(wire, iqHeader(2, 0, 1, 0, 2000, ",\"scale\":0.001"), u16Bytes({(uint16_t)1000, (uint16_t)(uint16_t)-2000, (uint16_t)32767, (uint16_t)(uint16_t)-32768}));
        Reader r;
        r.setFormat(Format::Int16);
        r.feed(wire.data(), wire.size());
        Header h;
        std::vector<cf32> iq;
        CHECK(r.next(h, iq) && iq.size() == 2, "int16 packet");
        CHECK(std::fabs(iq[0].real() - 1.0) < 1e-6 && std::fabs(iq[0].imag() + 2.0) < 1e-6, "scaled: %.4f %.4f", iq[0].real(), iq[0].imag());
        CHECK(std::fabs(iq[1].real() - 32.767) < 1e-4 && std::fabs(iq[1].imag() + 32.768) < 1e-4, "extremes");
        // float16, against an independent encoder
        const std::vector<float> want = {0.f, 1.f, -1.f, 0.5f, 123.456f, -0.0001f};
        std::vector<uint16_t> halves;
        for (float f : want) halves.push_back(toHalf(f));
        std::vector<uint8_t> w2;
        append(w2, iqHeader(3, 0, 1, 0, 3000), u16Bytes(halves));
        Reader r2;
        r2.setFormat(Format::Float16);
        r2.feed(w2.data(), w2.size());
        std::vector<cf32> iq2;
        CHECK(r2.next(h, iq2) && iq2.size() == 3, "float16 packet");
        for (size_t i = 0; i < 3; i++) {
            const float re = iq2[i].real(), im = iq2[i].imag();
            CHECK(std::fabs(re - want[i * 2]) <= std::fabs(want[i * 2]) * 0.002 + 1e-7, "half %zu real %g want %g", i, re, want[i * 2]);
            CHECK(std::fabs(im - want[i * 2 + 1]) <= std::fabs(want[i * 2 + 1]) * 0.002 + 1e-7, "half %zu imag %g want %g", i, im, want[i * 2 + 1]);
        }
        CHECK(halfToFloat(0x3C00) == 1.0f && halfToFloat(0x0000) == 0.0f && halfToFloat(0xBC00) == -1.0f, "known half bits");
        CHECK(std::fabs(halfToFloat(0x0001) - 5.9604645e-8f) < 1e-12, "the smallest subnormal: %g", halfToFloat(0x0001));
    }
    // a damaged packet is stepped over and the stream picks up again
    {
        std::vector<uint8_t> wire;
        const std::string junk = "{\"payload\":\"iq\",\"startFrequency\":9,\"endFrequency\":1,\"samples\":2}";   // end below start
        wire.insert(wire.end(), junk.begin(), junk.end());
        wire.push_back(kLineFeed);
        wire.push_back(kRecordSeparator);
        append(wire, iqHeader(1, 0, 1, 0, 1000), f32Bytes({7.f, 8.f}));
        Reader r;
        r.feed(wire.data(), wire.size());
        Header h;
        std::vector<cf32> iq;
        CHECK(!r.next(h, iq), "the damaged one is refused");
        CHECK(r.next(h, iq) && iq.size() == 1 && iq[0] == cf32(7, 8), "the good one follows");
        CHECK(r.badPackets() == 1, "counted: %llu", (unsigned long long)r.badPackets());
    }
    // the requests
    {
        CHECK(streamPath(Format::Float32) == "/stream?format=float32", "%s", streamPath(Format::Float32).c_str());
        CHECK(streamPath(Format::Int16, 4) == "/stream?format=int16&rate_reduction=4", "%s", streamPath(Format::Int16, 4).c_str());
        CHECK(streamPath(Format::Float16, 0, "main") == "/stream?format=float16", "main is the default input");
        CHECK(streamPath(Format::Float32, 0, "{3b459e11}") == "/stream?format=float32&input=%7B3b459e11%7D", "%s", streamPath(Format::Float32, 0, "{3b459e11}").c_str());
        CHECK(tuneBody(1920e6, 200e6) == "{\"frequencyCenter\":1920000000.000000,\"frequencySpan\":200000000.000000,\"type\":\"capture\"}", "%s", tuneBody(1920e6, 200e6).c_str());
        CHECK(streamingBody(true) == "{\"start\":true,\"type\":\"streaming\"}" && streamingBody(false) == "{\"start\":false,\"type\":\"streaming\"}", "control");
        const std::string g = httpRequest("GET", "/stream?format=float32", "localhost:54664");
        CHECK(g.rfind("GET /stream?format=float32 HTTP/1.1\r\nHost: localhost:54664\r\n", 0) == 0, "request line");
        CHECK(g.find("Content-Length") == std::string::npos && g.substr(g.size() - 4) == "\r\n\r\n", "no body");
        const std::string p = httpRequest("PUT", "/control", "h:1", tuneBody(1e9, 2e6), "Basic dXNlcjpwdw==");
        CHECK(p.find("Authorization: Basic dXNlcjpwdw==\r\n") != std::string::npos, "authorization");
        CHECK(p.find("Content-Length: " + std::to_string(tuneBody(1e9, 2e6).size())) != std::string::npos, "length");
        CHECK(p.size() > tuneBody(1e9, 2e6).size() && p.substr(p.size() - tuneBody(1e9, 2e6).size()) == tuneBody(1e9, 2e6), "body last");
    }
    // the response and its chunks
    {
        int st = 0;
        std::string body;
        size_t hl = 0;
        bool chunked = false;
        CHECK(!httpResponse("HTTP/1.1 200 OK\r\nX: 1\r\n", st, body, hl, chunked), "headers not finished");
        CHECK(httpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nrest", st, body, hl, chunked), "parsed");
        CHECK(st == 200 && chunked && body == "rest", "status %d chunked %d body [%s]", st, chunked, body.c_str());
        CHECK(httpResponse("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n", st, body, hl, chunked) && st == 401 && !chunked, "401");
        const std::string ch = "5\r\nhello\r\n3\r\nabc\r\n0\r\n\r\n";
        size_t pos = 0;
        std::string out, all;
        bool done = false;
        while (chunkedNext(ch, pos, out, done) && !done) all += out;
        CHECK(all == "helloabc" && done, "chunks [%s] done %d", all.c_str(), done);
        pos = 0;
        CHECK(!chunkedNext("5\r\nhel", pos, out, done), "a part chunk waits");
    }
    printf(fails ? "FAILED\n" : "aaronia ok\n");
    return fails ? 1 : 0;
}
