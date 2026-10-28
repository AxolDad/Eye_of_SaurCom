// Giving an RTL-SDR dongle a unique USB serial: the EEPROM image (parse, change the serial string, nothing else), a new serial that no attached radio
// has, and the write with its safeguards (claim the radio, read and check the whole image, save a backup file, write only what changes, read back and
// compare). The hardware is behind RtlEepromIo, so the tests use a simulated EEPROM that can fail at byte N; the real one is openRtlEepromIo().
#pragma once
#include "dect2/source/radio_id.h"
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kRtlEepromSize = 256;
constexpr size_t kRtlSerialLen = 8;   // digits

// The EEPROM of an RTL2832U dongle in the layout of librtlsdr's rtl_eeprom tool (rtl_eeprom.c; written here from the format, not copied):
//   0x00 0x28, 0x01 0x32   header (a blank or foreign EEPROM does not have it)
//   0x02..03 vendor id, 0x04..05 product id (little endian)
//   0x06 0xA5 when the serial string is in use ("have_serial"), 0x07 remote-wakeup / IR flags, 0x08 not touched here
//   0x09...  three USB string descriptors one after the other: manufacturer, product, serial. Each is [length][0x03][UTF-16LE text], length counting
//            the two header bytes. All three must end before offset 0x100. Whatever follows (padding) is not interpreted.
struct RtlEepromInfo {
    uint16_t vid = 0, pid = 0;
    bool haveSerial = false;            // byte 6 is 0xA5
    std::string manufacturer, product, serial;   // ASCII part of the strings (non-ASCII code units show as '?')
    size_t mfrAt = 0, prodAt = 0, serialAt = 0;  // offsets of the three descriptors (serialAt is where it would go when there is none)
    size_t stringsEnd = 0;              // offset after the last descriptor present
};
bool rtlEepromParse(const std::vector<uint8_t>& img, RtlEepromInfo& out, std::string& err);
// The serial must be exactly 8 decimal digits. Manufacturer and product stay byte for byte; the serial descriptor is re-encoded, byte 6 becomes 0xA5.
// Refused (img untouched) when the new strings would end after offset 0xFF, or would overwrite bytes after the old strings that are not padding
// (0x00 or 0xFF). Bytes outside [0x09, end of strings) and byte 6 are never changed; when the serial gets shorter the freed bytes become 0x00.
bool rtlEepromSetSerial(std::vector<uint8_t>& img, const std::string& serial, std::string& err);

// 8 digits, none of them in `taken` (serials of all attached radios of every kind), never 00000000 or 00000001. Deterministic for a seed.
std::string rtlPickSerial(const std::vector<std::string>& taken, uint32_t seed);
// A serial the user typed: 8 digits, not a factory default, not in taken. err is a short sentence for the dialog.
bool rtlSerialUsable(const std::string& serial, const std::vector<std::string>& taken, std::string& err);

// The hardware, 256 bytes at offsets. Both return false with err on failure; a write may have changed some bytes before it failed.
struct RtlEepromIo {
    virtual ~RtlEepromIo() = default;
    virtual bool read(uint32_t offset, uint8_t* buf, size_t len, std::string& err) = 0;
    virtual bool write(uint32_t offset, const uint8_t* buf, size_t len, std::string& err) = 0;
};
// Opens the dongle of d for the EEPROM (source_rtlsdr.cpp: by librtlsdr's index, checked against the serial and USB port). nullptr with err on failure.
std::unique_ptr<RtlEepromIo> openRtlEepromIo(const DeviceInfo& d, std::string& err);

// An RTL2832U dongle that this tool may touch: a Native "rtlsdr" radio (not a Soapy, HackRF or other radio).
bool rtlIsRtl2832(const DeviceInfo& d);
// "<dir>/eeprom-backup-<old serial>-<YYYYmmdd-HHMMSS>.bin" (the serial cleaned to letters, digits, '.', '_' and '-'), a number is added when it exists.
std::string rtlBackupPath(const std::string& dir, const std::string& oldSerial, time_t when);
// The directory and extension of the backups, newest first (file names only matching eeprom-backup-*.bin).
std::vector<std::string> rtlListBackups(const std::string& dir);

// The write, over any RtlEepromIo (rtlWriteSerial wraps it with the claim and the real device):
//  1 read the whole image   2 parse it (refuse when it does not)   3 write the backup (0600, temp file + rename), read it back and compare
//  4 compute the new image   5 write only the bytes that changed (the serial text, then byte 6 last)   6 read back and compare.
// backupPath is set as soon as the backup exists. After step 3 every failure says where the original is and to use "Restore from backup…".
bool rtlWriteSerialIo(RtlEepromIo& io, const std::string& newSerial, const std::string& backupDir, time_t now, std::string& backupPath, std::string& err);
// Puts a backup file back: validates it (256 bytes, header 28 32), reads the current image, writes what differs, reads back and compares.
bool rtlRestoreEepromIo(RtlEepromIo& io, const std::string& backupPath, std::string& err);

// The same on a real radio (it claims the dongle under kEepromClaimOwner, device_claims.h), in this order: refuse unless RTL2832U, claim it (refused with "in use by RX n"), open, rtlWriteSerialIo, release.
bool rtlWriteSerial(const DeviceInfo& d, const std::string& newSerial, const std::string& backupDir, std::string& backupPath, std::string& err);
bool rtlRestoreEeprom(const DeviceInfo& d, const std::string& backupPath, std::string& err);

// After the dongle has its new serial: its name follows it. The label under row.key (a "usb:" key, or the "sn:" key of the old serial) is moved to
// the "sn:" key of the new serial (unless that key already has a label); an old "sn:" label shared with another dongle stays where it is.
// Returns true when labels changed (the caller saves the file). `newKey` is what radioKey gives the dongle with the new serial.
std::string rtlKeyWithSerial(const DeviceInfo& d, const std::string& newSerial);
bool rtlCarryLabel(RadioLabels& labels, const RadioRow& row, const std::string& newSerial);

}  // namespace dect2
