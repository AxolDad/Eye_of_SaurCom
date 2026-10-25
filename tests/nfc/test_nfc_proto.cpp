// NFC frame naming and card tables (nfc_proto.h): the CRC_A and CRC_B vectors of ISO/IEC 14443-3 Annex B, the names of the selection
// exchange's frames, the anticollision tracker on a 4-byte and a 7-byte UID (asked for, and selected straight away), ATQA / SAK readings.
#include "dect2/nfc/nfc_proto.h"
#include <cstdio>
#include <string>
using namespace dect2;
using namespace dect2::nfc;
#include "../common/test_util.h"

static bool has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

int main() {
    // ISO/IEC 14443-3 Annex B: CRC_A of 00 00 is A0 1E, of 12 34 is 26 CF; CRC_B of 0A 12 34 56 is 2C F6 (sent LSB first)
    {
        const uint8_t a[] = {0x00, 0x00}, b[] = {0x12, 0x34}, c[] = {0x0A, 0x12, 0x34, 0x56};
        CHECK(crcA(a, 2) == 0x1EA0, "CRC_A(00 00) %04X", crcA(a, 2));
        CHECK(crcA(b, 2) == 0xCF26, "CRC_A(12 34) %04X", crcA(b, 2));
        CHECK(crcB(c, 4) == 0xF62C, "CRC_B %04X", crcB(c, 4));
        const uint8_t ok[] = {0x12, 0x34, 0x26, 0xCF}, bad[] = {0x12, 0x34, 0x26, 0xCE};
        CHECK(crcAOk(ok, 4) && !crcAOk(bad, 4) && !crcAOk(ok, 2), "crcAOk");
        const uint8_t hlta[] = {0x50, 0x00, 0x57, 0xCD};   // the HLTA every reader sends
        CHECK(crcAOk(hlta, 4), "HLTA CRC %04X", crcA(hlta, 2));
        CHECK(hex(hlta, 4) == "50 00 57 CD" && hex(hlta, 2, 0) == "5000", "hex '%s'", hex(hlta, 4).c_str());
    }
    // the names of a selection exchange
    {
        const uint8_t reqa[] = {0x26}, wupa[] = {0x52}, atqa[] = {0x04, 0x00}, sel20[] = {0x93, 0x20}, uid[] = {0x04, 0x7B, 0xC2, 0x1A, 0xA7};
        CHECK(nameA(reqa, 1, true, -1, 7) == "REQA" && nameA(wupa, 1, true, -1, 7) == "WUPA", "REQA / WUPA");
        CHECK(has(nameA(atqa, 2, false, 0x26), "ATQA 04 00") && has(nameA(atqa, 2, false, 0x26), "4 bytes"), "ATQA: '%s'", nameA(atqa, 2, false, 0x26).c_str());
        CHECK(has(nameA(sel20, 2, true, -1), "SELECT CL1"), "SEL: '%s'", nameA(sel20, 2, true, -1).c_str());
        CHECK(nameA(uid, 5, false, 0x93) == "UID CL1 04 7B C2 1A", "UID: '%s'", nameA(uid, 5, false, 0x93).c_str());
        const uint8_t badBcc[] = {0x04, 0x7B, 0xC2, 0x1A, 0x00};
        CHECK(has(nameA(badBcc, 5, false, 0x93), "bad BCC"), "bad BCC not flagged");
        uint8_t sel70[9] = {0x93, 0x70, 0x04, 0x7B, 0xC2, 0x1A, 0xA7};
        const uint16_t c = crcA(sel70, 7); sel70[7] = (uint8_t)c; sel70[8] = (uint8_t)(c >> 8);
        CHECK(nameA(sel70, 9, true, -1) == "SELECT CL1 UID 04 7B C2 1A", "SEL70: '%s'", nameA(sel70, 9, true, -1).c_str());
        uint8_t sak[3] = {0x08}; const uint16_t s = crcA(sak, 1); sak[1] = (uint8_t)s; sak[2] = (uint8_t)(s >> 8);
        CHECK(nameA(sak, 3, false, 0x93) == "SAK 08", "SAK: '%s'", nameA(sak, 3, false, 0x93).c_str());
        uint8_t sak20[3] = {0x20}; const uint16_t s2 = crcA(sak20, 1); sak20[1] = (uint8_t)s2; sak20[2] = (uint8_t)(s2 >> 8);
        CHECK(has(nameA(sak20, 3, false, 0x93), "ISO 14443-4"), "SAK 20: '%s'", nameA(sak20, 3, false, 0x93).c_str());
        const uint8_t hlta[] = {0x50, 0x00, 0x57, 0xCD};
        CHECK(nameA(hlta, 4, true, -1) == "HLTA", "HLTA: '%s'", nameA(hlta, 4, true, -1).c_str());
        const uint8_t auth[] = {0x60, 0x04, 0xD1, 0x3D}, rd[] = {0x30, 0x04, 0x26, 0xEE}, rats[] = {0xE0, 0x80, 0x31, 0x73};
        CHECK(nameA(auth, 4, true, -1) == "MIFARE AUTH key A, block 4", "AUTH: '%s'", nameA(auth, 4, true, -1).c_str());
        CHECK(nameA(rd, 4, true, -1) == "READ block 4", "READ: '%s'", nameA(rd, 4, true, -1).c_str());
        CHECK(nameA(rats, 4, true, -1) == "RATS (FSDI 8, CID 0)", "RATS: '%s'", nameA(rats, 4, true, -1).c_str());
        const uint8_t ack[] = {0x0A}, nak[] = {0x04}, ib[] = {0x02, 0x00, 0xA4, 0x04, 0x00, 0x11, 0x22};
        CHECK(nameA(ack, 1, false, 0xA2, 4) == "ACK" && nameA(nak, 1, false, 0xA2, 4) == "NAK 4", "ACK / NAK");
        CHECK(has(nameA(ib, 7, true, -1), "I-block"), "I-block: '%s'", nameA(ib, 7, true, -1).c_str());
        const uint8_t gv[] = {0x60, 0x6A, 0x57};
        CHECK(nameA(gv, 3, true, -1) == "GET_VERSION", "GET_VERSION vs AUTH: '%s'", nameA(gv, 3, true, -1).c_str());
    }
    // ATQA / SAK: the public table
    {
        CardInfo c = cardInfo(0x04, 0x00, 0x08);
        CHECK(c.uidBytes == 4 && !c.iso14443_4 && !c.uidIncomplete && has(c.type, "MIFARE Classic 1K"), "Classic 1K: %d '%s'", c.uidBytes, c.type.c_str());
        c = cardInfo(0x44, 0x00, 0x00);
        CHECK(c.uidBytes == 7 && has(c.type, "NTAG"), "NTAG: %d '%s'", c.uidBytes, c.type.c_str());
        c = cardInfo(0x44, 0x03, 0x20);
        CHECK(c.uidBytes == 7 && c.iso14443_4 && has(c.type, "DESFire"), "DESFire: '%s'", c.type.c_str());
        c = cardInfo(0x02, 0x00, 0x18);
        CHECK(has(c.type, "Classic 4K"), "4K: '%s'", c.type.c_str());
        c = cardInfo(0x44, 0x00, 0x04);
        CHECK(c.uidIncomplete && c.type.empty(), "incomplete SAK: '%s'", c.type.c_str());
        c = cardInfo(0x84, 0x00, 0x20);
        CHECK(c.uidBytes == 10 && c.iso14443_4, "10-byte UID: %d", c.uidBytes);
        CHECK(std::string(uidManufacturer(0x04)) == "NXP Semiconductors" && std::string(uidManufacturer(0x08)) == "random UID" && std::string(uidManufacturer(0xFE)).empty(), "manufacturers");
        CHECK(std::string(techName(TechA)) == "NFC-A" && std::string(techName(TechV)) == "NFC-V", "tech names");
    }
    // the anticollision tracker: a 4-byte UID asked for and selected
    {
        Anticollision ac;
        const uint8_t reqa[] = {0x26}, atqa[] = {0x04, 0x00}, sel20[] = {0x93, 0x20}, uid[] = {0x04, 0x7B, 0xC2, 0x1A, 0xA7};
        uint8_t sel70[9] = {0x93, 0x70, 0x04, 0x7B, 0xC2, 0x1A, 0xA7}; const uint16_t c = crcA(sel70, 7); sel70[7] = (uint8_t)c; sel70[8] = (uint8_t)(c >> 8);
        uint8_t sak[3] = {0x08}; const uint16_t s = crcA(sak, 1); sak[1] = (uint8_t)s; sak[2] = (uint8_t)(s >> 8);
        CHECK(!ac.frame(reqa, 1, true) && !ac.frame(atqa, 2, false) && !ac.frame(sel20, 2, true) && !ac.frame(uid, 5, false) && !ac.frame(sel70, 9, true), "completed early");
        CHECK(ac.frame(sak, 3, false), "not completed on the SAK");
        CHECK(ac.complete() && ac.uid() == std::vector<uint8_t>({0x04, 0x7B, 0xC2, 0x1A}) && ac.atqa0() == 0x04 && ac.atqa1() == 0x00 && ac.sak() == 0x08,
              "uid %s atqa %02X %02X sak %02X", hex(ac.uid().data(), ac.uid().size()).c_str(), ac.atqa0(), ac.atqa1(), ac.sak());
        // the next REQA starts over
        CHECK(!ac.frame(reqa, 1, true) && !ac.complete() && ac.uid().empty(), "not reset by REQA");
    }
    // a 7-byte UID over two cascade levels, and a reader that selects it straight away (WUPA, then SELECT with the UID, no 0x20 step)
    {
        Anticollision ac;
        const uint8_t wupa[] = {0x52}, atqa[] = {0x44, 0x00}, sel1[] = {0x93, 0x20}, cl1[] = {0x88, 0x04, 0x11, 0x22, 0xBF}, sel2[] = {0x95, 0x20}, cl2[] = {0x33, 0x44, 0x55, 0x66, 0x44};
        uint8_t s1[9] = {0x93, 0x70, 0x88, 0x04, 0x11, 0x22, 0xBF}; uint16_t c = crcA(s1, 7); s1[7] = (uint8_t)c; s1[8] = (uint8_t)(c >> 8);
        uint8_t k1[3] = {0x04}; c = crcA(k1, 1); k1[1] = (uint8_t)c; k1[2] = (uint8_t)(c >> 8);
        uint8_t s2[9] = {0x95, 0x70, 0x33, 0x44, 0x55, 0x66, 0x44}; c = crcA(s2, 7); s2[7] = (uint8_t)c; s2[8] = (uint8_t)(c >> 8);
        uint8_t k2[3] = {0x00}; c = crcA(k2, 1); k2[1] = (uint8_t)c; k2[2] = (uint8_t)(c >> 8);
        ac.frame(wupa, 1, true); ac.frame(atqa, 2, false); ac.frame(sel1, 2, true); ac.frame(cl1, 5, false); ac.frame(s1, 9, true);
        CHECK(!ac.frame(k1, 3, false), "completed on an incomplete SAK");
        ac.frame(sel2, 2, true); ac.frame(cl2, 5, false); ac.frame(s2, 9, true);
        CHECK(ac.frame(k2, 3, false) && ac.uid() == std::vector<uint8_t>({0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66}) && ac.sak() == 0x00,
              "7-byte uid %s", hex(ac.uid().data(), ac.uid().size()).c_str());
        Anticollision direct;
        direct.frame(wupa, 1, true); direct.frame(atqa, 2, false); direct.frame(s1, 9, true); direct.frame(k1, 3, false); direct.frame(s2, 9, true);
        CHECK(direct.frame(k2, 3, false) && direct.uid() == ac.uid(), "direct select uid %s", hex(direct.uid().data(), direct.uid().size()).c_str());
    }
    // the other technologies' names
    {
        const uint8_t reqb[] = {0x05, 0x00, 0x00, 0x71, 0xFF}, atqb[] = {0x50, 0x11, 0x22, 0x33, 0x44, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        CHECK(has(nameB(reqb, 5, true), "REQB"), "REQB: '%s'", nameB(reqb, 5, true).c_str());
        CHECK(has(nameB(atqb, 14, false), "ATQB PUPI 11 22 33 44"), "ATQB: '%s'", nameB(atqb, 14, false).c_str());
        const uint8_t inv[] = {0x26, 0x01, 0x00, 0, 0}, invr[] = {0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x07, 0xE0, 0, 0};
        CHECK(nameV(inv, 5, true) == "Inventory", "Inventory: '%s'", nameV(inv, 5, true).c_str());
        CHECK(has(nameV(invr, 12, false), "UID E0 07 66 55 44 33 22 11"), "NFC-V UID: '%s'", nameV(invr, 12, false).c_str());
        const uint8_t sensf[] = {0x06, 0x00, 0xFF, 0xFF, 0x00, 0x00}, sensfr[] = {0x12, 0x01, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0};
        CHECK(has(nameF(sensf, 6, true), "SENSF_REQ"), "SENSF_REQ: '%s'", nameF(sensf, 6, true).c_str());
        CHECK(has(nameF(sensfr, 18, false), "IDm 01 02 03 04 05 06 07 08"), "SENSF_RES: '%s'", nameF(sensfr, 18, false).c_str());
    }
    return testSummary("nfc_proto");
}
