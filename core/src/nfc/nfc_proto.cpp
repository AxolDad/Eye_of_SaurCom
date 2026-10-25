// NFC frame naming, CRCs, the anticollision exchange and the card tables: see nfc_proto.h
#include "dect2/nfc/nfc_proto.h"
#include <algorithm>
#include <cstdio>

namespace dect2 {
namespace nfc {

const char* techName(Tech t) {
    switch (t) {
    case TechA: return "NFC-A";
    case TechB: return "NFC-B";
    case TechF: return "NFC-F";
    case TechV: return "NFC-V";
    default: return "NFC";
    }
}

namespace {
// ISO/IEC 14443-3 Annex B, UpdateCrc
inline uint16_t crcStep(uint16_t crc, uint8_t b) {
    b ^= (uint8_t)(crc & 0xFF);
    b ^= (uint8_t)(b << 4);
    return (uint16_t)((crc >> 8) ^ ((uint16_t)b << 8) ^ ((uint16_t)b << 3) ^ ((uint16_t)b >> 4));
}
std::string byteStr(uint8_t b) { char s[4]; snprintf(s, sizeof s, "%02X", b); return s; }
const char* blockName(uint8_t pcb) {
    if ((pcb & 0xE2) == 0x02) return "I-block";
    if ((pcb & 0xE6) == 0xA2) return (pcb & 0x10) ? "R(NAK)" : "R(ACK)";
    if ((pcb & 0xC7) == 0xC2) return (pcb & 0x30) == 0x30 ? "S(WTX)" : (pcb & 0x30) == 0 ? "S(DESELECT)" : "S-block";
    return nullptr;
}
std::string isoDep(const uint8_t* d, size_t n) {
    const char* b = blockName(d[0]);
    if (!b) return "";
    std::string s = b;
    if ((d[0] & 0xE2) == 0x02) s += (d[0] & 0x10) ? " (chaining)" : "";
    if (d[0] & 0x08) s += std::string(" CID ") + std::to_string(n > 1 ? (d[1] & 0x0F) : 0);
    if ((d[0] & 0xE2) == 0x02 && n > 3) s += ", " + std::to_string(n - 1 - ((d[0] & 0x08) ? 1 : 0) - ((d[0] & 0x04) ? 1 : 0) - 2) + " bytes of APDU";
    return s;
}
} // namespace

uint16_t crcA(const uint8_t* d, size_t n) {
    uint16_t c = 0x6363;
    for (size_t i = 0; i < n; i++) c = crcStep(c, d[i]);
    return c;
}
bool crcAOk(const uint8_t* d, size_t n) {
    if (n < 3) return false;
    const uint16_t c = crcA(d, n - 2);
    return d[n - 2] == (c & 0xFF) && d[n - 1] == (c >> 8);
}
uint16_t crcB(const uint8_t* d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) c = crcStep(c, d[i]);
    return (uint16_t)~c;
}
bool crcBOk(const uint8_t* d, size_t n) {
    if (n < 3) return false;
    const uint16_t c = crcB(d, n - 2);
    return d[n - 2] == (c & 0xFF) && d[n - 1] == (c >> 8);
}

std::string hex(const uint8_t* d, size_t n, char sep) {
    std::string s;
    for (size_t i = 0; i < n; i++) { if (i && sep) s += sep; s += byteStr(d[i]); }
    return s;
}

std::string nameA(const uint8_t* d, size_t n, bool poll, int lastPoll, int bits) {
    if (n == 0) return "empty";
    if (poll) {
        if (n == 1 && (bits == 7 || d[0] == 0x26 || d[0] == 0x52)) {
            if (d[0] == 0x26) return "REQA";
            if (d[0] == 0x52) return "WUPA";
            return "short frame " + byteStr(d[0]);
        }
        const uint8_t c = d[0];
        if ((c == 0x93 || c == 0x95 || c == 0x97) && n >= 2) {
            const int level = (c - 0x93) / 2 + 1;
            if (d[1] == 0x20) return "SELECT CL" + std::to_string(level) + " (ask for UID)";
            if (d[1] == 0x70 && n >= 7) return "SELECT CL" + std::to_string(level) + " UID " + hex(d + 2, 4) + (n >= 9 ? (crcAOk(d, n) ? "" : " (bad CRC)") : "");
            char b[48]; snprintf(b, sizeof b, "ANTICOLLISION CL%d NVB %02X", level, d[1]);
            return b;
        }
        if (c == 0x50 && n == 4) return std::string("HLTA") + (crcAOk(d, n) ? "" : " (bad CRC)");
        if (c == 0xE0 && n == 4) { char b[48]; snprintf(b, sizeof b, "RATS (FSDI %d, CID %d)", d[1] >> 4, d[1] & 0x0F); return b; }
        if ((c & 0xF0) == 0xD0 && n >= 3 && n <= 5) { char b[32]; snprintf(b, sizeof b, "PPS (CID %d)", c & 0x0F); return b; }
        if ((c == 0x60 || c == 0x61) && n == 4) { char b[48]; snprintf(b, sizeof b, "MIFARE AUTH key %c, block %d", c == 0x60 ? 'A' : 'B', d[1]); return b; }
        if (c == 0x60 && n == 3) return "GET_VERSION";
        if (c == 0x30 && n == 4) { char b[32]; snprintf(b, sizeof b, "READ block %d", d[1]); return b; }
        if (c == 0xA0 && n == 4) { char b[32]; snprintf(b, sizeof b, "WRITE block %d", d[1]); return b; }
        if (c == 0xA2 && n == 8) { char b[48]; snprintf(b, sizeof b, "WRITE page %d: %s", d[1], hex(d + 2, 4).c_str()); return b; }
        if (c == 0xC0 && n == 4) { char b[32]; snprintf(b, sizeof b, "DECREMENT block %d", d[1]); return b; }
        if (c == 0xC1 && n == 4) { char b[32]; snprintf(b, sizeof b, "INCREMENT block %d", d[1]); return b; }
        if (c == 0xC2 && n == 4) { char b[32]; snprintf(b, sizeof b, "RESTORE block %d", d[1]); return b; }
        if (c == 0xB0 && n == 4) { char b[32]; snprintf(b, sizeof b, "TRANSFER block %d", d[1]); return b; }
        if (c == 0x1A && n == 4) return "AUTHENTICATE (Ultralight C)";
        if (c == 0x1B && n == 7) return "PWD_AUTH";
        if (c == 0x3A && n == 5) { char b[40]; snprintf(b, sizeof b, "FAST_READ pages %d-%d", d[1], d[2]); return b; }
        if (c == 0x3C && n == 4) return "READ_SIG";
        if (c == 0x39 && n == 4) { char b[32]; snprintf(b, sizeof b, "READ_CNT %d", d[1]); return b; }
        if (c == 0x3E && n == 4) return "CHECK_TEARING_EVENT";
        if (c == 0xA5 && n == 8) return "INCR_CNT";
        if (c == 0x4B) return "VCSL";
        if (c == 0x1C && n == 3) return "READ_SIG (NTAG 21x)";
        const std::string dep = isoDep(d, n);
        if (!dep.empty()) return dep;
        return "command " + byteStr(c) + " (" + std::to_string(n) + " bytes)";
    }
    // the card's answer
    if (n == 1 && bits == 4) {
        if (d[0] == 0x0A) return "ACK";
        if (d[0] == 0x00 || d[0] == 0x01 || d[0] == 0x04 || d[0] == 0x05) return "NAK " + std::to_string(d[0]);
        return "4-bit answer " + byteStr(d[0]);
    }
    if ((lastPoll == 0x26 || lastPoll == 0x52) && n == 2) {
        const CardInfo ci = cardInfo(d[0], d[1], 0);
        char b[64]; snprintf(b, sizeof b, "ATQA %02X %02X (UID %d bytes%s)", d[0], d[1], ci.uidBytes, (d[0] & 0x1F) ? ", anticollision" : "");
        return b;
    }
    if (lastPoll == 0x93 || lastPoll == 0x95 || lastPoll == 0x97) {
        const int level = (lastPoll - 0x93) / 2 + 1;
        if (n == 5) {
            const bool bcc = (uint8_t)(d[0] ^ d[1] ^ d[2] ^ d[3]) == d[4];
            return "UID CL" + std::to_string(level) + " " + hex(d, 4) + (d[0] == 0x88 ? " (cascade tag, more follows)" : "") + (bcc ? "" : " (bad BCC)");
        }
        if (n == 3) {
            char b[96];
            snprintf(b, sizeof b, "SAK %02X%s%s%s", d[0], (d[0] & 0x04) ? " (UID not complete)" : "", (d[0] & 0x20) ? " (ISO 14443-4)" : "", crcAOk(d, n) ? "" : " (bad CRC)");
            return b;
        }
    }
    if (lastPoll == 0xE0 && n >= 3) { char b[48]; snprintf(b, sizeof b, "ATS (%d bytes%s)", (int)n - 2, crcAOk(d, n) ? "" : ", bad CRC"); return b; }
    if ((lastPoll & 0xF0) == 0xD0 && lastPoll >= 0 && n == 3) return "PPS response";
    if ((lastPoll == 0x60 || lastPoll == 0x61) && n == 4) return "card nonce nT (" + hex(d, 4) + ")";
    if (lastPoll == 0x30 && n == 18) return std::string("16 data bytes") + (crcAOk(d, n) ? "" : " (bad CRC)");
    if (lastPoll == 0x3A && n >= 6) return std::to_string(n - 2) + " data bytes (fast read)";
    if (lastPoll == 0x60 && n == 10) return "version: " + hex(d, 8);
    if (lastPoll == 0x3C && n == 34) return "signature (32 bytes)";
    if (lastPoll == 0x39 && n == 5) return "counter " + hex(d, 3);
    if (lastPoll == 0x1B && n == 4) return "PACK " + hex(d, 2);
    const std::string dep = isoDep(d, n);
    if (!dep.empty()) return dep;
    return std::to_string(n) + " bytes";
}

std::string nameB(const uint8_t* d, size_t n, bool poll) {
    if (n == 0) return "empty";
    if (poll) {
        if (d[0] == 0x05 && n == 5) { char b[64]; snprintf(b, sizeof b, "%s (AFI %02X, %d slots)", (d[2] & 0x08) ? "WUPB" : "REQB", d[1], 1 << (d[2] & 0x07)); return b; }
        if (d[0] == 0x1D && n >= 11) return "ATTRIB PUPI " + hex(d + 1, 4);
        if (d[0] == 0x50 && n == 7) return "HLTB PUPI " + hex(d + 1, 4);
        const std::string dep = isoDep(d, n);
        if (!dep.empty()) return dep;
        return "command " + byteStr(d[0]) + " (" + std::to_string(n) + " bytes)";
    }
    if (d[0] == 0x50 && n >= 14) return "ATQB PUPI " + hex(d + 1, 4) + (crcBOk(d, n) ? "" : " (bad CRC)");
    const std::string dep = isoDep(d, n);
    if (!dep.empty()) return dep;
    return std::to_string(n) + " bytes";
}

std::string nameF(const uint8_t* d, size_t n, bool poll) {
    if (n < 2) return n ? "1 byte" : "empty";
    const uint8_t cmd = d[1];   // d[0] is the length
    if (poll) {
        if (cmd == 0x00 && n >= 6) { char b[64]; snprintf(b, sizeof b, "SENSF_REQ (system code %02X%02X)", d[2], d[3]); return b; }
        if (cmd == 0x04) return "Request Response";
        if (cmd == 0x06) return "Read Without Encryption";
        if (cmd == 0x08) return "Write Without Encryption";
        if (cmd == 0x0C) return "Request System Code";
        return "command " + byteStr(cmd) + " (" + std::to_string(n) + " bytes)";
    }
    if (cmd == 0x01 && n >= 18) return "SENSF_RES IDm " + hex(d + 2, 8);
    if (cmd == 0x07) return "Read response";
    if (cmd == 0x09) return "Write response";
    return "response " + byteStr(cmd) + " (" + std::to_string(n) + " bytes)";
}

std::string nameV(const uint8_t* d, size_t n, bool poll) {
    if (n < 2) return n ? "1 byte" : "empty";
    if (poll) {
        switch (d[1]) {
        case 0x01: return "Inventory";
        case 0x02: return "Stay quiet";
        case 0x20: return n >= 3 ? "Read single block " + std::to_string(d[n >= 11 ? 10 : 2]) : "Read single block";
        case 0x21: return "Write single block";
        case 0x22: return "Lock block";
        case 0x23: return "Read multiple blocks";
        case 0x24: return "Write multiple blocks";
        case 0x25: return "Select";
        case 0x26: return "Reset to ready";
        case 0x27: return "Write AFI";
        case 0x29: return "Write DSFID";
        case 0x2B: return "Get system information";
        case 0x2C: return "Get multiple block security status";
        default: { char b[48]; snprintf(b, sizeof b, "command %02X (flags %02X)", d[1], d[0]); return b; }
        }
    }
    if (d[0] & 0x01) return "error " + (n > 1 ? byteStr(d[1]) : std::string("?"));
    if (n >= 12) {   // an inventory answer: flags, DSFID, UID (8 bytes, least significant first), CRC
        uint8_t u[8];
        for (int i = 0; i < 8; i++) u[i] = d[9 - i];
        if (u[0] == 0xE0) return "UID " + hex(u, 8) + " (DSFID " + byteStr(d[1]) + ")";
    }
    return "response (" + std::to_string(n) + " bytes)";
}

CardInfo cardInfo(uint8_t atqa0, uint8_t atqa1, uint8_t sak) {
    CardInfo c;
    const int sz = (atqa0 >> 6) & 3;
    c.uidBytes = sz == 0 ? 4 : sz == 1 ? 7 : sz == 2 ? 10 : 0;
    c.iso14443_4 = sak & 0x20;
    c.uidIncomplete = sak & 0x04;
    const unsigned a = atqa0 | (unsigned)atqa1 << 8;
    switch (sak) {
    case 0x00: c.type = a == 0x0044 ? "MIFARE Ultralight / Ultralight C / EV1 or NTAG" : "NFC Forum type 2 tag"; break;
    case 0x08: c.type = a == 0x0004 ? "MIFARE Classic 1K (or Plus 2K SL1, or a Classic-compatible)" : a == 0x0044 ? "MIFARE Classic 1K with a 7-byte UID" : "MIFARE Classic 1K-compatible"; break;
    case 0x09: c.type = "MIFARE Mini"; break;
    case 0x10: c.type = "MIFARE Plus 2K SL2"; break;
    case 0x11: c.type = "MIFARE Plus 4K SL2"; break;
    case 0x18: c.type = a == 0x0002 ? "MIFARE Classic 4K (or Plus 4K SL1)" : "MIFARE Classic 4K-compatible"; break;
    case 0x20: c.type = a == 0x0344 ? "MIFARE DESFire (EV1 / EV2 / EV3) or DESFire Light" : (a == 0x0004 || a == 0x0002) ? "MIFARE Plus SL3, or another ISO 14443-4 card" : a == 0x0048 ? "ISO 14443-4 card (SmartMX / JCOP)" : "ISO 14443-4 card"; break;
    case 0x28: c.type = "ISO 14443-4 card with MIFARE Classic 1K emulation (SmartMX / JCOP)"; break;
    case 0x38: c.type = "ISO 14443-4 card with MIFARE Classic 4K emulation (SmartMX)"; break;
    case 0x88: c.type = "Infineon MIFARE Classic 1K"; break;
    case 0x98: case 0xB8: c.type = "Gemplus MPCOS"; break;
    case 0x01: c.type = "TNP3xxx (Tag-it)"; break;
    default: c.type = c.uidIncomplete ? "" : c.iso14443_4 ? "ISO 14443-4 card" : "ISO 14443-3 card (type not in the table)"; break;
    }
    return c;
}

const char* uidManufacturer(uint8_t b) {
    switch (b) {   // ISO/IEC 7816-6 registered IC manufacturers, the ones met on NFC cards
    case 0x01: return "Motorola";
    case 0x02: return "STMicroelectronics";
    case 0x03: return "Hitachi";
    case 0x04: return "NXP Semiconductors";
    case 0x05: return "Infineon";
    case 0x06: return "Cylink";
    case 0x07: return "Texas Instruments";
    case 0x08: return "random UID";
    case 0x09: return "Fujitsu";
    case 0x0A: return "Matsushita";
    case 0x0B: return "NEC";
    case 0x0C: return "Oki";
    case 0x0D: return "Toshiba";
    case 0x0E: return "Mitsubishi";
    case 0x0F: return "Samsung";
    case 0x10: return "Hynix";
    case 0x11: return "LG";
    case 0x12: return "Emosyn";
    case 0x13: return "INSIDE Technology";
    case 0x15: return "Sharp";
    case 0x16: return "Atmel";
    case 0x17: return "EM Microelectronic";
    case 0x1B: return "Sony";
    case 0x1D: return "Shanghai Fudan Microelectronics";
    case 0x1F: return "Melexis";
    case 0x20: return "Renesas";
    case 0x28: return "Ricoh";
    case 0x2B: return "Maxim";
    case 0x2C: return "Impinj";
    case 0x33: return "AMIC";
    case 0x34: return "Mikron";
    case 0x3D: return "HID Global";
    case 0x40: return "Gemalto";
    case 0x47: return "Shanghai Feiju";
    case 0x5C: return "Shenzhen Goodix";
    default: return "";
    }
}

void Anticollision::reset() {
    uid_.clear(); atqa_[0] = atqa_[1] = 0; sak_ = 0;
    lastPoll_ = -1; lastNvb_ = 0; level_ = 0;
    haveAtqa_ = false; complete_ = false;
}

bool Anticollision::frame(const uint8_t* d, size_t n, bool poll) {
    if (n == 0) return false;
    if (poll) {
        const uint8_t c = d[0];
        if (c == 0x26 || c == 0x52) { reset(); lastPoll_ = c; return false; }
        lastPoll_ = c;
        if ((c == 0x93 || c == 0x95 || c == 0x97) && n >= 2) {
            level_ = (c - 0x93) / 2;
            lastNvb_ = d[1];
            if (level_ == 0 && d[1] == 0x20) { uid_.clear(); complete_ = false; }
            // a reader that already knows the UID selects it without asking: the UID part is in the command
            if (d[1] == 0x70 && n >= 7) {
                const size_t have = (size_t)(level_ == 0 ? 0 : level_ == 1 ? 3 : 6);
                if (uid_.size() <= have) {
                    if (level_ == 0) uid_.clear();
                    if (d[2] == 0x88) uid_.insert(uid_.end(), d + 3, d + 6); else uid_.insert(uid_.end(), d + 2, d + 6);
                }
            }
        }
        return false;
    }
    if ((lastPoll_ == 0x26 || lastPoll_ == 0x52) && n == 2) { atqa_[0] = d[0]; atqa_[1] = d[1]; haveAtqa_ = true; return false; }
    if (lastPoll_ == 0x93 || lastPoll_ == 0x95 || lastPoll_ == 0x97) {
        if (lastNvb_ == 0x20 && n == 5) {
            const size_t have = (size_t)(level_ == 0 ? 0 : level_ == 1 ? 3 : 6);
            if (level_ == 0) uid_.clear();
            if (uid_.size() == have) {
                if (d[0] == 0x88) uid_.insert(uid_.end(), d + 1, d + 4); else uid_.insert(uid_.end(), d, d + 4);
            }
            return false;
        }
        if (lastNvb_ == 0x70 && n == 3) {
            sak_ = d[0];
            if (!(sak_ & 0x04) && !uid_.empty()) { complete_ = true; return true; }
            return false;
        }
    }
    return false;
}

} // namespace nfc
} // namespace dect2
