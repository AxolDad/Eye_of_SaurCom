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
constexpr size_t kRtlStringLimit = 78;   // the strings end at or before this offset (rtl_eeprom.c)

// The EEPROM of an RTL2832U dongle in the layout of librtlsdr's rtl_eeprom tool (rtl_eeprom.c; written here from the format, not copied):
//   0x00 0x28, 0x01 0x32   header (a blank or foreign EEPROM does not have it)
//   0x02..03 vendor id, 0x04..05 product id (little endian)
//   0x06 0xA5 when the serial string is in use ("have_serial"), 0x07 remote-wakeup / IR flags, 0x08 not touched here
//   0x09...  three USB string descriptors one after the other: manufacturer, product, serial. Each is [length][0x03][UTF-16LE text], length counting
//            the two header bytes. All three must end at or before offset 78 (rtl_eeprom's limit): byte 78 is the length of the IR configuration
//            and the IR configuration follows it, up to 0xFF. Bytes 78..255 are never changed by this tool.
struct RtlEepromInfo {
    uint16_t vid = 0, pid = 0;
    bool haveSerial = false;            // byte 6 is 0xA5
    std::string manufacturer, product, serial;   // ASCII part of the strings (non-ASCII code units show as '?')
    size_t mfrAt = 0, prodAt = 0, serialAt = 0;  // offsets of the three descriptors (serialAt is where it would go when there is none)
    size_t stringsEnd = 0;              // offset after the last descriptor present
};
bool rtlEepromParse(const std::vector<uint8_t>& img, RtlEepromInfo& out, std::string& err);
// The serial must be exactly 8 decimal digits. Manufacturer and product stay byte for byte; the serial descriptor is re-encoded, byte 6 becomes 0xA5.
// Refused (img untouched) when the new strings would end after offset 78, or would overwrite bytes after the old strings that are not padding
// (0x00 or 0xFF). Bytes outside [0x09, end of strings) and byte 6 are never changed (so 78..255 never); when the serial gets shorter the freed bytes become 0x00.
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
    // The USB vendor / product id the dongle enumerated with (from the USB walk), when known.
    virtual bool liveUsbId(uint16_t& vid, uint16_t& pid) { (void)vid; (void)pid; return false; }
};
// True when librtlsdr takes this VID/PID (its table): a dongle with another id is invisible to the app and to rtl_eeprom.
bool rtlKnownUsbId(uint16_t vid, uint16_t pid);
// Whether librtlsdr's index of d (d.nativeArgs) can be trusted to be d's dongle, worked out without opening any dongle: n is librtlsdr's count now,
// paths the fresh USB walk in librtlsdr's order (usbPathsInOrder: one per dongle, or empty when it cannot be mapped), listed the radio list the
// tool was opened from (its look-alikes, the RTL-SDRs with d's serial, are counted there). With d's port known and the walk mapped, the port at the
// index must be d's; otherwise exactly one RTL dongle may be attached (n == 1), and the list must still have it. False with err when not.
bool rtlEepromTargetOk(const DeviceInfo& d, const std::vector<DeviceInfo>& listed, uint32_t n, const std::vector<std::string>& paths, std::string& err);
// Opens the dongle of d for the EEPROM (source_rtlsdr.cpp: by librtlsdr's index, checked with rtlEepromTargetOk, then the serial of that one
// dongle, which the caller has claimed; no other dongle is opened). After opening, the count, the serial and the port at the index are checked
// again. nullptr with err on failure. listed: the radio list the tool was opened from.
std::unique_ptr<RtlEepromIo> openRtlEepromIo(const DeviceInfo& d, const std::vector<DeviceInfo>& listed, std::string& err);

// An RTL2832U dongle that this tool may touch: a Native "rtlsdr" radio (not a Soapy, HackRF or other radio).
bool rtlIsRtl2832(const DeviceInfo& d);
// "<dir>/eeprom-backup-<old serial>-<YYYYmmdd-HHMMSS>.bin" (the serial cleaned to letters, digits, '.', '_' and '-'), a number is added when it exists.
std::string rtlBackupPath(const std::string& dir, const std::string& oldSerial, time_t when);
// The directory and extension of the backups, newest first (file names only matching eeprom-backup-*.bin).
std::vector<std::string> rtlListBackups(const std::string& dir);

// What librtlsdr's EEPROM calls return: rtlsdr_read_eeprom gives the result of its last 1-byte transfer (1 on success, negative on error), rtlsdr_write_eeprom
// gives 0 on success and negative on error. The Rust port (saurcom_usb) follows both.
inline bool rtlEepromReadOk(int r) { return r >= 0; }
inline bool rtlEepromWriteOk(int r) { return r == 0; }

// Reads the image twice and requires both reads to be identical (a transient misread must never become a backup or a diff).
bool rtlReadStable(RtlEepromIo& io, std::vector<uint8_t>& img, std::string& err);
// A backup file chosen by the user: only a regular file of exactly 256 bytes is read (never a FIFO, a device or a huge file), with a bounded read.
bool rtlReadBackupFile(const std::string& path, std::vector<uint8_t>& out, std::string& err);

// The write, over any RtlEepromIo (rtlWriteSerial wraps it with the claim and the real device). expectedSerial is the serial the radio list shows for
// the dongle; the EEPROM must say the same (a dongle without the serial flag: a factory default).
//  1 read the whole image twice (identical)   2 parse it (refuse when it does not) and compare its serial   3 work out the new image (a dry run)
//  4 write the backup (0600, temp file + rename), read it back and compare   5 write only the bytes that changed (the serial text, then byte 6 last)
//  6 read back and compare.
// backupPath: when set on entry, that is the file to make (it must not exist), else a name is made; it is set once the backup exists.
// After step 4 every failure says where the original is and to use "Restore from backup…".
bool rtlWriteSerialIo(RtlEepromIo& io, const std::string& expectedSerial, const std::string& newSerial, const std::string& backupDir, time_t now,
                      std::string& backupPath, std::string& err);
// Puts a backup file back. restoredSerial (when given) is set on success to the serial the dongle reports from now on: the backup's serial string,
// or "" when the backup has the serial turned off. The file is read safely (rtlReadBackupFile), must parse as an RTL-SDR EEPROM and have the VID/PID the dongle has now.
// The dongle's current contents are read twice and saved (a new backup in backupDir, its path in safetyBackup) before anything is written; then
// what differs is written, and the dongle is read back and compared. When the dongle has no valid header, the backup's VID/PID must be a known RTL-SDR
// id and equal the dongle's live USB id when that is known.
bool rtlRestoreEepromIo(RtlEepromIo& io, const std::string& backupFile, const std::string& backupDir, time_t now, std::string& safetyBackup, std::string& err,
                        std::string* restoredSerial = nullptr);

// The same on a real radio (it claims the dongle under kEepromClaimOwner, device_claims.h; the key is the one receivers use for that DeviceInfo), in order:
// refuse unless RTL2832U, claim it (refused with "in use by RX n"), open (openRtlEepromIo), rtlWriteSerialIo, release. listed: the radio list the
// tool was opened from (for the look-alikes; see rtlEepromTargetOk).
bool rtlWriteSerial(const DeviceInfo& d, const std::vector<DeviceInfo>& listed, const std::string& newSerial, const std::string& backupDir, std::string& backupPath,
                    std::string& err);
bool rtlRestoreEeprom(const DeviceInfo& d, const std::vector<DeviceInfo>& listed, const std::string& backupFile, const std::string& backupDir, std::string& safetyBackup,
                      std::string& err, std::string* restoredSerial = nullptr);

// After the dongle has its new serial: its name follows it. The label under row.key (a "usb:" key, or the "sn:" key of the old serial) is moved to
// the "sn:" key of the new serial (unless that key already has a label); an old "sn:" label shared with another dongle stays where it is.
// Returns true when labels changed (the caller saves the file). `newKey` is what radioKey gives the dongle with the new serial.
std::string rtlKeyWithSerial(const DeviceInfo& d, const std::string& newSerial);
bool rtlCarryLabel(RadioLabels& labels, const RadioRow& row, const std::string& newSerial);
// The key d gets with that serial among the other radios attached (identifyRadios over `attached` with d's entry given the serial): an "sn:" key,
// or its "usb:" key when the serial is a factory default or another attached radio of its kind has it. d's own entry in attached (same kind, board,
// serial, usbPath and nativeArgs) is the one changed; when it is not there, it is added.
std::string rtlKeyWithSerial(const std::vector<DeviceInfo>& attached, const DeviceInfo& d, const std::string& newSerial);
// After a restore put the old serial back: the same move as rtlCarryLabel, to the key rtlKeyWithSerial(attached, row.dev, restoredSerial) gives
// (a "usb:" key when that serial is shared again). The name under row.key is removed when that key was a "usb:" one or the serial was this
// dongle's alone (it no longer has it). Returns true when labels changed.
bool rtlCarryLabelBack(RadioLabels& labels, const RadioRow& row, const std::vector<DeviceInfo>& attached, const std::string& restoredSerial);

}  // namespace dect2
