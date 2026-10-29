// P25 Phase 2 TDMA voice channel, outbound (the repeater's downlink). Receive only.
//
// Written from the public descriptions of TIA-102.BBAB (physical layer), TIA-102.BBAC (MAC layer) and TIA-102.BBAD (MAC messages): the
// documents themselves were not available, so what follows is the structure as publicly described, with the bit-level choices this
// implementation had to make kept together in this header (see "Layout" below) so they can be corrected in one place.
//
// What is from the public descriptions:
//   - H-DQPSK outbound, 6000 symbols/s (12 kbit/s), the dibit to phase change mapping of P25 CQPSK (00 +45, 01 +135, 10 -45, 11 -135 degrees),
//     root raised cosine shaping. (H-CPM is the subscriber's inbound modulation: a receiver that follows calls on the repeater never needs it.)
//   - 30 ms timeslots of 360 bits (180 dibits), 12 timeslots to a 360 ms superframe; two logical voice channels (slot 0 and slot 1 of a grant)
//     take turns in timeslots 0..9 (even: channel 0, odd: channel 1), timeslots 10 and 11 carry the slow associated control channel (SACCH)
//     of channel 0 and channel 1. A channel's five bursts in a superframe carry 4V, 4V, 4V, 4V, 2V: 18 AMBE+2 frames of 20 ms = 360 ms.
//   - Every timeslot starts with the 40-bit inter-slot signalling channel: the sync S-ISCH (0x575D57F7FF) or an information I-ISCH.
//   - The burst's data unit ID (4V, 2V, FACCH, SACCH, scrambled or not) is a 4-bit value sent with 4 parity bits.
//   - Voice: AMBE+2 half rate, 72-bit frames (the vocoder DMR and NXDN use: dect2/nxdn/nxdn_ambe.h).
//   - Encryption sync (ESS): ALGID 8, key ID 16, MI 72 bits as an RS(44,16,29) code over GF(64): 4 x 24 bits in the 4V bursts (ESS-B) and
//     168 bits in the 2V burst (ESS-A). FACCH: RS(45,26,20); SACCH: RS(52,30,23); MAC PDUs carry a CRC-12.
//   - The bursts are scrambled with a pseudo random sequence seeded with WACN (20 bits), system ID (12) and NAC / colour code (12).
//   - MAC PDU opcodes (3 bits): MAC_SIGNAL 0, MAC_PTT 1, MAC_END_PTT 2, MAC_IDLE 3, MAC_ACTIVE 4, MAC_HANGTIME 6. MAC_PTT carries MI, ALGID,
//     key ID, source and group; MAC_END_PTT the colour code, source and group; MAC_ACTIVE / IDLE / HANGTIME carry MAC messages, of which the
//     group voice channel user message (0x01: service options, group, source) and unit to unit voice channel user (0x02) are read.
//
// Layout (this implementation's choices where the public description gives no bit positions; NOT checked against the documents or a live
// system): the DUID dibits at burst dibits 0, 53, 106, 159; the payload order of the 4V (V1 V2 ESS-B V3 V4) and 2V (V1 ESS-A V2) bursts; the
// I-ISCH (40,9) code; the S-ISCH at timeslot 0 of each superframe only; the scrambler's polynomial and its run over the 312 payload bits of
// each timeslot; the CRC-12 polynomial; the MAC byte positions. The receive chain, the follow logic and the tests do not depend on them.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace p25p2 {

constexpr double kSymRate = 6000.0;
constexpr int kSlotDibits = 180, kIschDibits = 20, kBurstDibits = 160, kPayloadBits = 312, kSlotsPerSuperframe = 12;
constexpr uint64_t kSisch = 0x575D57F7FFull;   // 40 bits, first bit sent is bit 39
constexpr int kAlgClear = 0x80;

enum Duid { k4V = 0, kSacchClear = 3, k2V = 6, kFacch = 9, kSacch = 12, kFacchClear = 15 };
const char* duidName(int duid);
bool duidScrambled(int duid);

// ------------------------------------------------------------------------------------------------ Layout (see the header comment)
constexpr int kDuidPos[4] = {0, 53, 106, 159};   // burst dibits that carry the DUID
// logical channel of timeslot k (0..11) and whether it is that channel's SACCH
inline int chanOfSlot(int k) { return k < 10 ? (k & 1) : k - 10; }
inline bool sacchSlot(int k) { return k >= 10; }

// ------------------------------------------------------------------------------------------------ codes
uint8_t duidEncode(int duid);                         // 8 bits: 4 data (bit 7 first) + 4 parity (extended Hamming (8,4,4))
int duidDecode(uint8_t v, int* errs = nullptr);       // nearest codeword; -1 beyond one error
uint64_t ischEncode(int info9);                       // I-ISCH: 9 bits -> 40
int ischDecode(uint64_t cw, int* dist = nullptr);     // nearest codeword's info; -1 when farther than 8 bits
inline int ischInfo(int slot, int chan, int ultra) { return (slot & 15) << 5 | (chan & 1) << 4 | (ultra & 3) << 2; }
inline int ischSlot(int info) { return (info >> 5) & 15; }
uint16_t crc12(const uint8_t* bits, int n);           // one bit per byte

// The burst scrambler: a 44-bit LFSR seeded with WACN, system and NAC; bits(k) are the 312 payload bits' mask of timeslot k of a superframe
class Scrambler {
public:
    void seed(int wacn, int sysId, int nac);
    const uint8_t* bits(int k) const { return &pn_[(size_t)(k % kSlotsPerSuperframe) * kPayloadBits]; }
    uint64_t seedValue() const { return seed_; }
private:
    std::vector<uint8_t> pn_ = std::vector<uint8_t>((size_t)kSlotsPerSuperframe * kPayloadBits, 0);
    uint64_t seed_ = ~0ull;
};
uint64_t scramblerSeed(int wacn, int sysId, int nac);

// ------------------------------------------------------------------------------------------------ MAC PDUs (TIA-102.BBAD as described)
enum MacOp { kMacSignal = 0, kMacPtt = 1, kMacEndPtt = 2, kMacIdle = 3, kMacActive = 4, kMacHangtime = 6 };
const char* macName(int op);
struct MacPdu {
    int opcode = -1, offset = 0;
    int src = -1, tg = -1;       // PTT / END_PTT, or the voice channel user message inside ACTIVE / IDLE / HANGTIME
    bool group = true;
    int algId = kAlgClear, keyId = 0;
    int svcOpts = 0;             // the voice channel user message's service options (0x80 emergency, 0x40 encrypted)
    int colorCode = -1;          // END_PTT
    uint8_t mi[9] = {};
    int msgOpcode = -1;          // first MAC message of ACTIVE / IDLE / HANGTIME, -1 none
};
// bytes: at most 21 (SACCH: 168 bits; FACCH: 144 bits = 18 bytes)
std::vector<uint8_t> macPtt(int src, int tg, int algId = kAlgClear, int keyId = 0, const uint8_t* mi9 = nullptr);
std::vector<uint8_t> macEndPtt(int src, int tg, int colorCode);
std::vector<uint8_t> macWithUser(int opcode, int tg, int src, int svcOpts, bool group = true);   // ACTIVE / IDLE / HANGTIME + voice channel user
std::vector<uint8_t> macIdle();
bool macParse(const uint8_t* bytes, int n, MacPdu& m);

// ------------------------------------------------------------------------------------------------ bursts
// One burst as received: 160 dibits (the timeslot after its ISCH). Voice frames are 72 bits each, first bit sent first.
struct Burst {
    int duid = -1, duidErrs = 0;
    int nVoice = 0;
    uint8_t voice[4][72] = {};
    uint8_t ess[168] = {};       // 4V: 24 bits ESS-B; 2V: 168 bits ESS-A
    bool macPresent = false, macOk = false;
    MacPdu mac;
};
// Build: voice frames for 4V (4) and 2V (2); ess bits (24 or 168); mac bytes for FACCH / SACCH; scr: the timeslot's 312-bit mask or nullptr
void burstBuild(int duid, const uint8_t (*voice)[72], const uint8_t* ess, const std::vector<uint8_t>& mac, const uint8_t* scr, uint8_t dibits[kBurstDibits]);
// Parse: scr is the timeslot's mask (used for the scrambled DUIDs)
void burstParse(const uint8_t dibits[kBurstDibits], const uint8_t* scr, Burst& b);
// The encryption sync of one superframe: ALGID, key ID, MI <-> 4 x 24 ESS-B + 168 ESS-A bits (RS(44,16,29))
void essBuild(int algId, int keyId, const uint8_t mi[9], uint8_t essB[4][24], uint8_t essA[168]);
bool essParse(const uint8_t essB[4][24], const uint8_t essA[168], int& algId, int& keyId, uint8_t mi[9]);

// ------------------------------------------------------------------------------------------------ the frame layer: dibits in, events out
struct Event {
    enum Kind { Lock, Lost, Voice, Mac, Ess } kind = Lock;
    int chan = 0;                // logical channel 0 / 1 (the grant's slot)
    int slot = 0;                // timeslot in the superframe
    int nVoice = 0;
    uint8_t voice[4][72] = {};
    MacPdu mac;
    bool macOk = false;
    int algId = kAlgClear, keyId = 0;
};

class Framer {
public:
    void setSeed(int wacn, int sysId, int nac) { scr_.seed(wacn, sysId, nac); }
    void reset();
    void push(int dibit);
    std::vector<Event> take() { std::vector<Event> o; o.swap(out_); return o; }
    bool locked() const { return locked_; }
    uint64_t superframes() const { return sf_; }
    uint64_t bursts() const { return bursts_; }
    uint64_t macOk() const { return macOk_; }
    uint64_t macBad() const { return macBad_; }
private:
    Scrambler scr_;
    uint64_t reg_ = 0;           // the last 20 dibits (40 bits)
    bool locked_ = false;
    int pos_ = 0;                // dibits of the current timeslot taken (0..179); the ISCH is 0..19
    int slot_ = 0;               // the current timeslot
    int misses_ = 0;             // superframes in a row without the S-ISCH where it belongs
    uint8_t buf_[kSlotDibits] = {};
    uint8_t essB_[2][4][24] = {};
    int essBn_[2] = {0, 0};
    uint64_t sf_ = 0, bursts_ = 0, macOk_ = 0, macBad_ = 0;
    std::vector<Event> out_;
    void slotDone();
};

// One logical channel's call as the MAC PDUs and voice describe it
struct Call {
    bool active = false, endSeen = false, encrypted = false, emergency = false, group = true, lateEntry = false;
    bool cryptoKnown = false;    // MAC_PTT, the encryption sync or a MAC_ACTIVE said whether it is encrypted (joined late: not yet, so nothing is played)
    int src = -1, tg = -1, algId = kAlgClear, keyId = 0;
    uint32_t frames = 0;
};
class CallTracker {
public:
    // returns true when the event changed the call (started, ended, new ids)
    bool onEvent(const Event& e);
    const Call& call(int chan) const { return c_[chan & 1]; }
    void reset() { c_[0] = c_[1] = Call(); }
private:
    Call c_[2];
};

// ------------------------------------------------------------------------------------------------ the modem
// H-DQPSK receiver: complex baseband at any rate in (the channel offsetHz from 0 Hz), dibits out. Root raised cosine matched filter at
// 8 samples per symbol, symbol timing from the modulus dispersion (the constant envelope at the right instant) with early / late tracking,
// differential detection with the carrier offset taken out (4th power).
class Demod {
public:
    Demod();
    ~Demod();
    bool configure(double inRate, double offsetHz);
    void reset();
    void feed(const cf32* x, size_t n, std::vector<uint8_t>& dibits);
    double cfoHz() const;
    double levelDb() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Test signal: H-DQPSK of the dibits at fs, carrier at offsetHz, amplitude amp (root raised cosine, roll-off 0.2)
std::vector<cf32> modulate(const std::vector<uint8_t>& dibits, double fs, double offsetHz, float amp);

// A synthetic outbound voice channel (for tests and the demo): calls on logical channel 0 and 1, superframe by superframe
struct GenCall {
    int chan = 0;
    int startSf = 0, superframes = 3;   // the call's voice superframes; MAC_PTT in the superframe before the voice, MAC_END_PTT after
    int tg = 0, src = 0;
    int algId = kAlgClear, keyId = 0;
};
class Generator {
public:
    Generator(int wacn, int sysId, int nac);
    void add(const GenCall& c) { calls_.push_back(c); }
    // the 2160 dibits of superframe s
    std::vector<uint8_t> superframe(int s);
    // the voice frame the generator puts in place n (0, 1, 2 ...) of channel c's voice: what a decoder must hand to the vocoder
    static void voiceFrame(int chan, uint32_t n, uint8_t bits[72]);
    const Scrambler& scrambler() const { return scr_; }
private:
    Scrambler scr_;
    int nac_;
    std::vector<GenCall> calls_;
    uint32_t vn_[2] = {0, 0};
    const GenCall* active(int chan, int s, int& phase) const;   // phase: 0 PTT superframe, 1 voice, 2 END superframe
};

} // namespace p25p2
} // namespace dect2
