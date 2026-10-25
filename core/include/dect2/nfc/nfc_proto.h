// NFC / ISO/IEC 14443 frames as the radio hears them: what a frame is (its command or answer), the UID a card gives up in the anticollision
// exchange, and what the ATQA and SAK bytes say about it. The demodulation is nfc-laboratory's (third_party/nfc-laboratory, GPL-3.0); this
// layer only names what it decoded, from ISO/IEC 14443-3 and -4 and the public MIFARE command sets. Receive only: nothing is sent, no
// card is emulated, and an exchange a card and reader encrypt is marked as such and left alone.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace nfc {

enum Tech { TechUnknown = 0, TechA, TechB, TechF, TechV };
const char* techName(Tech t);   // "NFC-A", ...

// CRC_A of ISO/IEC 14443-3 Annex B: CRC-16 polynomial 0x8408 (reflected 0x1021), initial value 0x6363, no final XOR, sent LSB first.
// crcA({0x00, 0x00}) is 0x1EA0 (bytes A0 1E on the air), crcA({0x12, 0x34}) is 0xCF26.
uint16_t crcA(const uint8_t* d, size_t n);
bool crcAOk(const uint8_t* d, size_t n);   // n >= 3: the last two bytes are the CRC of the ones before
// CRC_B (ISO/IEC 14443-3 Annex B): initial 0xFFFF, final XOR 0xFFFF
uint16_t crcB(const uint8_t* d, size_t n);
bool crcBOk(const uint8_t* d, size_t n);

std::string hex(const uint8_t* d, size_t n, char sep = ' ');

// The name of an NFC-A frame: a reader command (poll) or a card answer (listen), the latter read in the light of the last command.
// bits: the frame's length in bits when it is not whole bytes (a REQA is 7 bits, an ACK 4), else 0.
std::string nameA(const uint8_t* d, size_t n, bool poll, int lastPollCmd, int bits = 0);
std::string nameB(const uint8_t* d, size_t n, bool poll);
std::string nameF(const uint8_t* d, size_t n, bool poll);
std::string nameV(const uint8_t* d, size_t n, bool poll);

// What the ATQA (2 bytes, as sent: low byte first) and the SAK say about an NFC-A card: the UID size, whether it speaks ISO/IEC 14443-4
// (APDUs), and the type the public tables give for the pair (a guess: several products share the same values).
struct CardInfo {
    int uidBytes = 0;            // 4, 7 or 10 from the ATQA; 0 unknown
    bool iso14443_4 = false;     // SAK bit 6: RATS / APDUs
    bool uidIncomplete = false;  // SAK bit 3: another cascade level follows
    std::string type;            // "MIFARE Classic 1K", "NTAG / Ultralight", "DESFire", ...
};
CardInfo cardInfo(uint8_t atqa0, uint8_t atqa1, uint8_t sak);
const char* uidManufacturer(uint8_t uid0);   // ISO/IEC 7816-6 registered manufacturer of a UID's first byte ("" unknown; 0x08 random UID)

// One card's anticollision exchange, followed frame by frame: REQA / WUPA, the ATQA, the cascade levels (SELECT 0x93 / 0x95 / 0x97 with
// NVB 0x20 asking for the UID part, the card's UID CLn + BCC, the SELECT with NVB 0x70 and the card's SAK). complete() is true once the
// SAK says the UID is whole; then uid() holds 4, 7 or 10 bytes.
class Anticollision {
public:
    void reset();
    // Feed every NFC-A frame in order. Returns true when this frame completed a card (uid(), atqa(), sak() are then valid until reset or
    // the next REQA / WUPA).
    bool frame(const uint8_t* d, size_t n, bool poll);
    bool complete() const { return complete_; }
    const std::vector<uint8_t>& uid() const { return uid_; }
    uint8_t atqa0() const { return atqa_[0]; }
    uint8_t atqa1() const { return atqa_[1]; }
    uint8_t sak() const { return sak_; }
    int lastPoll() const { return lastPoll_; }   // the last reader command byte (-1 none), for naming the card's answer

private:
    std::vector<uint8_t> uid_;
    uint8_t atqa_[2] = {0, 0}, sak_ = 0;
    int lastPoll_ = -1, lastNvb_ = 0, level_ = 0;
    bool haveAtqa_ = false, complete_ = false;
};

} // namespace nfc
} // namespace dect2
