// AX.25 link layer for packet radio: the frame check sequence, HDLC bit handling (flags, bit stuffing, NRZI), frame parsing and building.
// Plain data in, plain data out; the radio side is in packet_rx.cpp and the test signal in packet_gen.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {
namespace ax25 {

// CRC-16-CCITT as AX.25 uses it (reflected, start 0xFFFF, result inverted). appendFcs() adds it low byte first.
uint16_t fcs(const uint8_t* d, size_t n);
void appendFcs(std::vector<uint8_t>& frame);
bool fcsOk(const uint8_t* d, size_t n);                 // n includes the two FCS bytes

struct Address {
    std::string call;            // without the SSID, up to 6 characters
    int ssid = 0;                // 0..15
    bool h = false;              // "has been repeated" bit (digipeaters), command/response bit (source and destination)
    std::string str() const;     // CALL or CALL-SSID
};

struct Frame {
    Address to, from;
    std::vector<Address> path;   // digipeaters
    uint8_t control = 0x03;
    int pid = 0xF0;              // -1: the frame type carries none
    std::string info;
    std::string pathStr() const;     // "WIDE1-1*,WIDE2-1": the star marks the last digipeater that has repeated the frame
    std::string tnc2() const;        // FROM>TO,PATH:info
    std::string typeName() const;    // "UI", "I", "RR", "SABM", ...
    bool isUi() const { return (control & 0xEF) == 0x03; }
};

// Parses a frame without its FCS. False when the addresses are malformed.
bool parseFrame(const uint8_t* d, size_t n, Frame& f);
// Builds the bytes of a frame, FCS included.
std::vector<uint8_t> buildFrame(const Frame& f);
// Frame bytes (with FCS) -> bits ready for NRZI: flags in front, bit stuffing, flags behind. LSB first as on the air.
std::vector<uint8_t> hdlcBits(const std::vector<uint8_t>& frameWithFcs, int flagsBefore, int flagsAfter);
// NRZI: a 0 changes the level, a 1 keeps it. Returns the levels.
std::vector<uint8_t> nrziEncode(const std::vector<uint8_t>& bits, uint8_t startLevel = 0);
// G3RUH scrambler x^17 + x^12 + 1 for 9600 baud (state: the last 17 output bits)
std::vector<uint8_t> scramble(const std::vector<uint8_t>& bits, uint32_t& state);

// State of the HDLC receiver: a plain struct (same layout as the Rust one in rust/saurcom_ax25/src/hdlc.rs) so that both implementations share it.
struct HdlcState {
    int32_t prev = 0;
    uint8_t pat = 0, acc = 0;
    int32_t cnt = 0;
    bool inFrame = false, hunting = true, resyncClock = false;
    int32_t flagRecent = 0;
    uint32_t len = 0;            // bytes in buf
    uint8_t buf[402] = {};       // frame bytes (at most 401 are stored)
};
// One bit through the receiver state: 0 nothing completed, 1 a frame with a good FCS (buf[0..len) without the FCS), 2 a frame of plausible length with
// a bad FCS (buf[0..len) all bytes). Rust (packet_ax25_rs.cpp).
int hdlcStep(HdlcState& s, int level, bool nrzi, size_t& len);
void hdlcReset(HdlcState& s);

// Receives the NRZI line levels (or, with nrzi = false, plain HDLC bits) one at a time and cuts out the frames.
class HdlcRx {
public:
    // ok = the FCS was right (data = the frame without FCS); !ok = a frame of a plausible length failed the check (data = all bytes)
    using Callback = std::function<void(const std::vector<uint8_t>& data, bool ok)>;
    explicit HdlcRx(Callback cb = nullptr, bool nrzi = true) : cb_(std::move(cb)), nrzi_(nrzi) {}
    void setCallback(Callback cb) { cb_ = std::move(cb); }
    void reset() { hdlcReset(st_); }
    void bit(int level);
    bool inFrame() const { return st_.inFrame; }
    bool flagSeen() const { return st_.flagRecent > 0; }   // a flag came a short while ago
    bool takeClockResync() { const bool r = st_.resyncClock; st_.resyncClock = false; return r; }
private:
    Callback cb_;
    bool nrzi_;
    HdlcState st_;
};

} // namespace ax25
} // namespace dect2
