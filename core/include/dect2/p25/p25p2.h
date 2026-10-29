// P25 Phase 2 TDMA voice channel, outbound (the repeater's downlink, H-DQPSK). Receive only.
//
// The layout below is derived from two GPL-3 implementations (this project is GPL-3 too, see THIRD_PARTY.md), and where they overlap they
// agree; the test checks them against each other:
//   - OP25 (boatbod/op25, op25/gr-op25_repeater): lib/p25p2_tdma.cc (burst positions of the voice frames, ESS, FACCH and SACCH, the MAC PDU
//     fields, CRC-12, the Reed-Solomon (63,35) shortening and puncturing), lib/p25p2_duid.cc (the DUID decoding table), lib/p25p2_isch.cc
//     (the 128 I-ISCH codewords and the S-ISCH), lib/p25p2_sync.cc (which timeslots carry an I-ISCH, the slot number in it),
//     lib/p25p2_vf.cc (the AMBE+2 voice codeword interleave), apps/tdma/lfsr.py (the scrambling sequence), lib/frame_sync_magics.h.
//   - SDRTrunk (DSheirer/sdrtrunk, io.github.dsheirer.module.decode.p25.phase2): enumeration/DataUnitID.java (DUID codewords),
//     timeslot/Timeslot.java (DUID bit positions), timeslot/LinearFeedbackShiftRegister.java and ScramblingSequence.java (the scrambler and
//     where its sequence lines up with the timeslots), message/SuperFrameFragment.java (ISCH / timeslot layout).
//
// On air: 6000 symbols/s H-DQPSK (dibit 00 +45, 01 +135, 10 -45, 11 -135 degrees), root raised cosine. A 360 ms superframe is 12 frames of
// 180 dibits: a 40-bit ISCH, then a 320-bit timeslot. Timeslots 0..9 alternate between logical channels 0 and 1, timeslot 10 is channel 1's
// SACCH and 11 channel 0's. The ISCH before timeslots 0, 1, 4, 5, 8, 9 is an I-ISCH (a (40,9) codeword naming the timeslot), before 2, 3,
// 6, 7, 10, 11 the S-ISCH sync 0x575D57F7FF. H-CPM is the subscribers' inbound modulation: following calls on a repeater does not need it,
// and it is not implemented.
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
constexpr int kSlotDibits = 180, kIschDibits = 20, kBurstDibits = 160, kSlotsPerSuperframe = 12;
constexpr uint64_t kSisch = 0x575D57F7FFull;   // 40 bits, first bit sent is bit 39 (OP25 frame_sync_magics.h)
constexpr int kAlgClear = 0x80;

// DUID values (SDRTrunk DataUnitID.java; OP25 p25p2_tdma.cc handle_packet)
enum Duid { k4V = 0, kSacch = 3, kLcch = 4, k2V = 6, kFacch = 9, kSacchClear = 12, kLcchClear = 13, kFacchClear = 15 };
const char* duidName(int duid);
bool duidScrambled(int duid);

// ------------------------------------------------------------------------------------------------ layout
constexpr int kDuidPos[4] = {0, 37, 122, 159};                   // timeslot dibits that carry the DUID (SDRTrunk Timeslot.java bits 0,1,74,75,244,245,318,319)
constexpr int kVoicePos[4] = {1, 38, 86, 123};                   // 4V: the four 36-dibit voice frames; 2V: the first two (OP25 xored_burst[11,48,96,133])
constexpr int kEssBPos = 74;                                     // 4V: 12 dibits of ESS-B (4 hexbits)
constexpr int kEssAPos = 74;                                     // 2V: 84 dibits of ESS-A (28 hexbits), skipping the DUID dibit at 122
inline int chanOfSlot(int k) { static const int c[12] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 0}; return c[k % 12]; }   // OP25 which_slot[]
inline bool sacchSlot(int k) { return k >= 10; }
inline bool ischIsSync(int k) { return (k & 2) != 0; }          // timeslots 2, 3, 6, 7, 10, 11

// ------------------------------------------------------------------------------------------------ codes
uint8_t duidEncode(int duid);                         // the 8-bit codeword (value << 4 | parity)
int duidDecode(uint8_t v, int* errs = nullptr);       // OP25's table; -1 when it cannot tell
// I-ISCH: the 7-bit information chn(2) loc(2) fr(1) cnt(2), most significant first; the timeslot is loc * 4 + chn
uint64_t ischEncode(int info7);
int ischDecode(uint64_t cw, int* dist = nullptr);     // the information, -2 for the S-ISCH, -1 farther than 7 bits from every codeword
inline int ischInfo(int slot, int fr = 0, int cnt = 0) { return ((slot & 3) << 5) | ((slot >> 2 & 3) << 3) | ((fr & 1) << 2) | (cnt & 3); }
inline int ischSlot(int info) { return ((info >> 3) & 3) * 4 + ((info >> 5) & 3); }
uint16_t crc12(const uint8_t* bits, int n);           // one bit per byte (OP25 crc12: x^12+x^11+x^7+x^4+x^2+x+1, the remainder inverted)

// Reed-Solomon (63,35) over GF(64) (x^6 + x + 1, first root alpha^1) with errors and erasures: sym[0..62], data first. erasures: positions.
// true when corrected (or clean); corrected: symbols changed.
bool rs63Decode(uint8_t sym[63], const std::vector<int>& erasures, int* corrected = nullptr);
void rs63Encode(uint8_t sym[63]);                      // fills sym[35..62]

// The scrambling sequence of a superframe: a 44-bit LFSR seeded with WACN (20 bits), system (12) and NAC (12), taps 43 33 19 14 8 3
// (SDRTrunk LinearFeedbackShiftRegister.java; the same sequence as OP25 apps/tdma/lfsr.py). Timeslot k's 320 bits are sequence bits
// 360 k + 20 ... (ScramblingSequence.java: it starts half way into the superframe's first ISCH).
class Scrambler {
public:
    void seed(int wacn, int sysId, int nac);
    const uint8_t* bits(int k) const { return &seq_[(size_t)(k % kSlotsPerSuperframe) * 360 + 20]; }   // 320 bits for timeslot k
    const std::vector<uint8_t>& sequence() const { return seq_; }   // 4320 + 20 bits
private:
    std::vector<uint8_t> seq_ = std::vector<uint8_t>(4340, 0);
};
uint64_t scramblerSeed(int wacn, int sysId, int nac);

// ------------------------------------------------------------------------------------------------ MAC PDUs (OP25 p25p2_tdma.cc)
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
    int msgOpcode = -1;          // the first non-null MAC message of ACTIVE / IDLE / HANGTIME, -1 none (all of them are walked for the voice user)
};
std::vector<uint8_t> macPtt(int src, int tg, int algId = kAlgClear, int keyId = 0, const uint8_t* mi9 = nullptr);
std::vector<uint8_t> macEndPtt(int src, int tg, int colorCode);
std::vector<uint8_t> macWithUser(int opcode, int tg, int src, int svcOpts, bool group = true);   // ACTIVE / IDLE / HANGTIME + voice channel user
std::vector<uint8_t> macIdle();
bool macParse(const uint8_t* bytes, int n, MacPdu& m);

// ------------------------------------------------------------------------------------------------ voice frames (OP25 p25p2_vf.cc)
// One AMBE+2 voice frame is 36 dibits on air; its code words c0 (24 bits, Golay), c1 (23, Golay, scrambled by c0's data), c2 (11), c3 (14)
// are interleaved. fr is mbelib's ambe_fr[4][24] (fr[k][i] = bit i of ck, the most significant at the top), what the AMBE+2 decoder takes.
void voiceDeinterleave(const uint8_t dibits[36], char fr[4][24]);
void voiceInterleave(const uint32_t c[4], uint8_t dibits[36]);
void voiceFr(const uint32_t c[4], char fr[4][24]);   // the same code words as fr, for tests

// ------------------------------------------------------------------------------------------------ bursts
struct Burst {
    int duid = -1, duidErrs = 0;
    int nVoice = 0;
    uint8_t voice[4][36] = {};   // the voice frames' dibits, descrambled
    uint8_t ess[28] = {};        // hexbits: 4V 4 (ESS-B), 2V 28 (ESS-A)
    bool macPresent = false, macOk = false;
    int rsCorrected = 0;
    MacPdu mac;
};
// Build: voice frames (36 dibits each) for 4V (4) and 2V (2); ess hexbits (4 or 28); mac bytes for FACCH / SACCH; scr: the timeslot's 320
// scrambling bits (scrambled DUIDs); into the 160 timeslot dibits
void burstBuild(int duid, const uint8_t (*voice)[36], const uint8_t* ess, const std::vector<uint8_t>& mac, const uint8_t* scr, uint8_t dibits[kBurstDibits]);
void burstParse(const uint8_t dibits[kBurstDibits], const uint8_t* scr, Burst& b);
// The encryption sync of one superframe: ALGID, key ID, MI <-> ESS-B (16 hexbits, 4 per 4V) and ESS-A (28 hexbits): RS(44,16) of the
// (63,35) code (OP25 handle_4V2V_ess)
void essBuild(int algId, int keyId, const uint8_t mi[9], uint8_t essB[16], uint8_t essA[28]);
bool essParse(const uint8_t essB[16], const uint8_t essA[28], int& algId, int& keyId, uint8_t mi[9]);

// ------------------------------------------------------------------------------------------------ the frame layer: dibits in, events out
struct Event {
    enum Kind { Lock, Lost, Voice, Mac, Ess } kind = Lock;
    int chan = 0;                // logical channel 0 / 1 (the grant's slot)
    int slot = 0;                // timeslot in the superframe
    int nVoice = 0;
    uint8_t voice[4][36] = {};
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
    bool locked() const { return locked_ && slot_ >= 0; }
    uint64_t superframes() const { return sf_; }
    uint64_t bursts() const { return bursts_; }
    uint64_t macOk() const { return macOk_; }
    uint64_t macBad() const { return macBad_; }
private:
    Scrambler scr_;
    uint64_t reg_ = 0;           // the last 20 dibits (40 bits)
    bool locked_ = false, rev_ = false;   // rev_: the spectrum is inverted (OP25's P25P2_FRAME_SYNC_REV_P): every dibit's first bit flips
    int pos_ = 0;                // dibits of the current frame taken (0..179); the ISCH is 0..19
    int slot_ = -1;              // the current timeslot (-1: not known yet, until an I-ISCH)
    int misses_ = 0;             // ISCHs in a row that did not fit
    uint8_t buf_[kSlotDibits] = {};
    uint8_t essB_[2][16] = {};
    int essBn_[2] = {0, 0};
    uint64_t sf_ = 0, bursts_ = 0, macOk_ = 0, macBad_ = 0;
    std::vector<Event> out_;
    void frameDone();
};

struct Call {
    bool active = false, endSeen = false, encrypted = false, emergency = false, group = true, lateEntry = false;
    bool cryptoKnown = false;    // MAC_PTT, the encryption sync or a MAC_ACTIVE said whether it is encrypted (joined late: not yet, so nothing is played)
    int src = -1, tg = -1, algId = kAlgClear, keyId = 0;
    uint32_t frames = 0;
};
class CallTracker {
public:
    bool onEvent(const Event& e);
    const Call& call(int chan) const { return c_[chan & 1]; }
    void reset() { c_[0] = c_[1] = Call(); }
private:
    Call c_[2];
};

// ------------------------------------------------------------------------------------------------ the modem
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

std::vector<cf32> modulate(const std::vector<uint8_t>& dibits, double fs, double offsetHz, float amp);

// A synthetic outbound voice channel (tests and the demo), laid out as above
struct GenCall {
    int chan = 0;
    int startSf = 0, superframes = 3;   // the voice superframes; MAC_PTT in the superframe before, MAC_END_PTT after
    int tg = 0, src = 0;
    int algId = kAlgClear, keyId = 0;
};
class Generator {
public:
    Generator(int wacn, int sysId, int nac);
    void add(const GenCall& c) { calls_.push_back(c); }
    std::vector<uint8_t> superframe(int s);   // 2160 dibits
    // the code words the generator puts in voice frame n of channel c: c0 (12 data bits, Golay), c1 (12 data bits, Golay, scrambled), c2, c3.
    // The vocoder of a receiver gets exactly these (voiceFr of them).
    static void voiceWords(int chan, uint32_t n, uint32_t c[4]);
    const Scrambler& scrambler() const { return scr_; }
private:
    Scrambler scr_;
    int nac_;
    std::vector<GenCall> calls_;
    uint32_t vn_[2] = {0, 0};
    const GenCall* active(int chan, int s, int& phase) const;
};

} // namespace p25p2
} // namespace dect2
