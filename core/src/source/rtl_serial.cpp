#include "dect2/source/rtl_serial.h"
#include "dect2/common/u8file.h"
#include "dect2/engine/device_claims.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dect2 {

namespace fs = std::filesystem;

namespace {

constexpr size_t kStrAt = 9;   // first string descriptor
constexpr size_t kSerialDescLen = 2 + 2 * kRtlSerialLen;

bool isDigits8(const std::string& s) {
    return s.size() == kRtlSerialLen && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// One string descriptor [len][0x03][UTF-16LE] at `at`; false when it does not fit before 0x100 or is not one. `end` = offset after it.
bool readDesc(const std::vector<uint8_t>& img, size_t at, std::string& text, size_t& end, std::string& err, const char* what) {
    if (at + 2 > kRtlStringLimit) { err = std::string("The EEPROM has no ") + what + " string."; return false; }
    const size_t len = img[at];
    if (len < 2 || (len & 1) || img[at + 1] != 0x03 || at + len > kRtlStringLimit) { err = std::string("The ") + what + " string in the EEPROM is not a USB string descriptor."; return false; }
    text.clear();
    for (size_t i = at + 2; i + 1 < at + len; i += 2) text += (img[i + 1] == 0 && img[i] >= 0x20 && img[i] < 0x7F) ? (char)img[i] : '?';
    end = at + len;
    return true;
}

bool sameBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) { return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin()); }

std::string sanitizeForName(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (o.size() >= 32) break;
        o += (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-') ? c : '_';
    }
    return o.empty() || o == "." || o == ".." ? std::string("none") : o;
}

std::string hint(const std::string& what, const std::string& backup) {
    return what + " Your original EEPROM is saved at " + backup + ". Use Restore from backup… to put it back.";
}

// The span [lo, hi] of bytes that differ between two images of equal size; false when equal. Byte `skip` is left out (the flag, written last).
bool diffSpan(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, size_t skip, size_t& lo, size_t& hi) {
    bool any = false;
    for (size_t i = 0; i < a.size(); i++) {
        if (i == skip || a[i] == b[i]) continue;
        if (!any) lo = i;
        hi = i;
        any = true;
    }
    return any;
}

bool readImage(RtlEepromIo& io, std::vector<uint8_t>& img, std::string& err) {
    img.assign(kRtlEepromSize, 0);
    return io.read(0, img.data(), img.size(), err);
}

bool isFactoryName(const std::string& s) { return s.empty() || s == "00000001" || s == "0"; }

// Writes what differs (the span first, byte `flag` last when it differs) and reads the image back. what/backup build the failure texts.
bool writeAndVerify(RtlEepromIo& io, const std::vector<uint8_t>& want, const std::vector<uint8_t>& have, size_t flag, const std::string& backup,
                    const char* failedText, const char* verifyText, std::string& err) {
    size_t lo = 0, hi = 0;
    std::string werr;
    if (diffSpan(want, have, flag, lo, hi) && !io.write((uint32_t)lo, want.data() + lo, hi - lo + 1, werr)) { err = hint(std::string(failedText) + " (" + werr + ").", backup); return false; }
    if (want[flag] != have[flag] && !io.write((uint32_t)flag, want.data() + flag, 1, werr)) { err = hint(std::string(failedText) + " (" + werr + ").", backup); return false; }
    std::vector<uint8_t> back;
    if (!readImage(io, back, werr)) { err = hint(std::string("The dongle could not be read back (") + werr + ").", backup); return false; }
    if (!sameBytes(back, want)) { err = hint(verifyText, backup); return false; }
    return true;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::string e;
    return rtlReadBackupFile(path, out, e);
}

// The backup: a temporary file next to it (mode 0600), flushed, renamed into place, read back and compared with the image.
bool writeBackup(const std::string& dir, const std::string& path, const std::vector<uint8_t>& img, std::string& err) {
    std::error_code ec;
    fs::create_directories(u8file(dir), ec);
    if (ec || !fs::is_directory(u8file(dir), ec)) { err = "Cannot make the backup folder " + dir + (ec ? " (" + ec.message() + ")" : "") + ". Nothing was written to the dongle."; return false; }
    const std::string tmp = path + ".tmp";
    fs::remove(u8file(tmp), ec);
    bool ok = false;
#ifndef _WIN32
    const int fd = ::open(u8file(tmp).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0) {
        size_t done = 0;
        while (done < img.size()) {
            const ssize_t w = ::write(fd, img.data() + done, img.size() - done);
            if (w <= 0) break;
            done += (size_t)w;
        }
        ok = done == img.size() && ::fsync(fd) == 0;
        ok = (::close(fd) == 0) && ok;
    }
#else
    {
        std::ofstream f(u8file(tmp), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(img.data()), (std::streamsize)img.size());
        f.flush();
        ok = f.good();
    }
#endif
    if (ok) {   // put it under its final name without ever replacing a file: a hard link fails when the name exists (atomic), the temp name is then dropped
        fs::create_hard_link(u8file(tmp), u8file(path), ec);
        if (ec == std::errc::file_exists) { fs::remove(u8file(tmp), ec); err = "The backup file " + path + " already exists. Try again. Nothing was written to the dongle."; return false; }
        if (ec) {   // a file system without hard links: check, then rename
            if (fs::exists(u8file(path))) { fs::remove(u8file(tmp), ec); err = "The backup file " + path + " already exists. Try again. Nothing was written to the dongle."; return false; }
            fs::rename(u8file(tmp), u8file(path), ec);
        } else fs::remove(u8file(tmp), ec), ec.clear();
        ok = !ec;
    }
    if (!ok) { fs::remove(u8file(tmp), ec); err = "Cannot write the backup file " + path + ". Nothing was written to the dongle."; return false; }
#ifndef _WIN32
    fs::permissions(u8file(path), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
#endif
    std::vector<uint8_t> again;
    if (!readFile(path, again) || again.size() != kRtlEepromSize || !sameBytes(again, img)) {
        err = "The backup file " + path + " does not hold the dongle's EEPROM when read back. Nothing was written to the dongle.";
        return false;
    }
    return true;
}

struct ClaimScope {   // the dongle is ours until the scope ends
    bool ok = false;
    std::string err;
    explicit ClaimScope(const DeviceInfo& d) {
        int holder = 0;
        ok = claimDevice(d, kEepromClaimOwner, &holder);
        if (!ok) err = "in use by " + deviceHolderText(holder);
    }
    ~ClaimScope() { if (ok) releaseDevice(kEepromClaimOwner); }
    ClaimScope(const ClaimScope&) = delete;
    ClaimScope& operator=(const ClaimScope&) = delete;
};

}  // namespace

bool rtlIsRtl2832(const DeviceInfo& d) { return d.kind == DeviceInfo::Native && d.board == "rtlsdr"; }

// ---- the image ----

bool rtlEepromParse(const std::vector<uint8_t>& img, RtlEepromInfo& out, std::string& err) {
    err.clear();
    out = RtlEepromInfo();
    if (img.size() != kRtlEepromSize) { err = "The EEPROM image is " + std::to_string(img.size()) + " bytes, not 256."; return false; }
    if (img[0] != 0x28 || img[1] != 0x32) { err = "This EEPROM does not start with the RTL2832U header (28 32): it is blank or not an RTL-SDR's."; return false; }
    RtlEepromInfo r;
    r.vid = (uint16_t)(img[2] | img[3] << 8);
    r.pid = (uint16_t)(img[4] | img[5] << 8);
    r.haveSerial = img[6] == 0xA5;
    size_t at = kStrAt, end = 0;
    r.mfrAt = at;
    if (!readDesc(img, at, r.manufacturer, end, err, "manufacturer")) return false;
    at = r.prodAt = end;
    if (!readDesc(img, at, r.product, end, err, "product")) return false;
    r.serialAt = r.stringsEnd = end;
    if (r.haveSerial) {
        if (!readDesc(img, r.serialAt, r.serial, end, err, "serial")) return false;
        r.stringsEnd = end;
    }
    out = r;
    return true;
}

bool rtlEepromSetSerial(std::vector<uint8_t>& img, const std::string& serial, std::string& err) {
    err.clear();
    if (!isDigits8(serial)) { err = "The serial must be exactly 8 digits."; return false; }
    RtlEepromInfo info;
    if (!rtlEepromParse(img, info, err)) return false;
    const size_t newEnd = info.serialAt + kSerialDescLen, oldEnd = info.stringsEnd;
    if (newEnd > kRtlStringLimit) { err = "The new serial does not fit in this dongle's EEPROM (the manufacturer and product names leave too little room)."; return false; }
    for (size_t i = oldEnd; i < newEnd; i++)
        if (img[i] != 0x00 && img[i] != 0xFF) { err = "The EEPROM holds other data right after its strings, which the new serial would overwrite."; return false; }
    std::vector<uint8_t> out = img;
    size_t at = info.serialAt;
    out[at++] = (uint8_t)kSerialDescLen;
    out[at++] = 0x03;
    for (char c : serial) { out[at++] = (uint8_t)c; out[at++] = 0x00; }
    for (size_t i = newEnd; i < oldEnd; i++) out[i] = 0x00;
    out[6] = 0xA5;
    img = std::move(out);
    return true;
}

// ---- a new serial ----

namespace {
bool isDefaultSerial(const std::string& s) { return s == "00000000" || s == "00000001"; }
std::string eightDigits(uint32_t v) { char b[16]; snprintf(b, sizeof b, "%08u", v % 100000000u); return b; }
}

bool rtlSerialUsable(const std::string& serial, const std::vector<std::string>& taken, std::string& err) {
    err.clear();
    if (!isDigits8(serial)) { err = "The serial must be exactly 8 digits."; return false; }
    if (isDefaultSerial(serial)) { err = "That serial is a factory default that many dongles share."; return false; }
    if (std::find(taken.begin(), taken.end(), serial) != taken.end()) { err = "Another attached radio already has that serial."; return false; }
    return true;
}

std::string rtlPickSerial(const std::vector<std::string>& taken, uint32_t seed) {
    uint32_t x = seed * 2654435761u + 0x9E3779B9u;
    if (x == 0) x = 0x1234567u;
    std::string err;
    for (int tries = 0; tries < 2000; tries++) {   // xorshift32
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        const std::string s = eightDigits(x);
        if (rtlSerialUsable(s, taken, err)) return s;
    }
    for (uint32_t v = x % 100000000u, n = 0; n < 100000000u; n++, v = (v + 1) % 100000000u) {   // (practically unreachable) any free number
        const std::string s = eightDigits(v);
        if (rtlSerialUsable(s, taken, err)) return s;
    }
    return std::string();
}

// ---- backups ----

std::string rtlBackupPath(const std::string& dir, const std::string& oldSerial, time_t when) {
    char ts[32];
    struct tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &when);
#else
    localtime_r(&when, &tmv);
#endif
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tmv);
    const std::string base = (dir.empty() ? std::string() : dir + (dir.back() == '/' || dir.back() == '\\' ? "" : "/")) + "eeprom-backup-" + sanitizeForName(oldSerial) + "-" + ts;
    std::error_code ec;
    std::string p = base + ".bin";
    for (int n = 2; n < 1000 && fs::exists(u8file(p), ec); n++) p = base + "-" + std::to_string(n) + ".bin";
    return p;
}

std::vector<std::string> rtlListBackups(const std::string& dir) {
    std::vector<std::pair<fs::file_time_type, std::string>> v;
    std::error_code ec;
    for (fs::directory_iterator it(u8file(dir), ec), e; !ec && it != e; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind("eeprom-backup-", 0) != 0 || name.size() < 4 || name.compare(name.size() - 4, 4, ".bin") != 0) continue;
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        v.emplace_back(it->last_write_time(e2), it->path().string());
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second > b.second; });
    std::vector<std::string> out;
    for (auto& p : v) out.push_back(p.second);
    return out;
}

// ---- reading safely ----

bool rtlReadStable(RtlEepromIo& io, std::vector<uint8_t>& img, std::string& err) {
    std::vector<uint8_t> second;
    std::string e;
    if (!readImage(io, img, e) || !readImage(io, second, e)) { err = "The dongle's EEPROM could not be read (" + e + "). Nothing was written."; return false; }
    if (!sameBytes(img, second)) { err = "The dongle's EEPROM reads differently each time (a bad USB connection?). Nothing was written."; return false; }
    return true;
}

bool rtlReadBackupFile(const std::string& path, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    const std::string notEeprom = "The backup " + path + " is not 256 bytes: it is not an EEPROM backup.";
    const std::string notRegular = "The backup " + path + " is not a regular file.";
#ifndef _WIN32
    // open without blocking (a FIFO would otherwise wait for a writer), then judge the open descriptor, not the path
    const int fd = ::open(u8file(path).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { err = "Cannot read the backup file " + path + "."; return false; }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { ::close(fd); err = notRegular; return false; }
    if (st.st_size != (off_t)kRtlEepromSize) { ::close(fd); err = notEeprom; return false; }
    out.resize(kRtlEepromSize + 1);   // one byte more than expected: a file that grew is caught
    size_t got = 0;
    while (got < out.size()) {
        const ssize_t r = ::read(fd, out.data() + got, out.size() - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    ::close(fd);
    out.resize(got);
#else
    std::error_code ec;
    const fs::path p = u8file(path);
    if (ec || !fs::is_regular_file(fs::status(p, ec))) { err = notRegular; return false; }
    if (fs::file_size(p, ec) != kRtlEepromSize || ec) { err = notEeprom; return false; }
    std::ifstream f(p, std::ios::binary);
    if (!f) { err = "Cannot read the backup file " + path + "."; return false; }
    out.resize(kRtlEepromSize + 1);
    f.read(reinterpret_cast<char*>(out.data()), (std::streamsize)out.size());
    out.resize((size_t)f.gcount());
#endif
    if (out.size() != kRtlEepromSize) { out.clear(); err = notEeprom; return false; }
    return true;
}

// ---- the write ----

bool rtlWriteSerialIo(RtlEepromIo& io, const std::string& expectedSerial, const std::string& newSerial, const std::string& backupDir, time_t now,
                      std::string& backupPath, std::string& err) {
    err.clear();
    const std::string planned = backupPath;
    backupPath.clear();
    std::string rerr;
    std::vector<uint8_t> cur;
    if (!rtlReadStable(io, cur, err)) return false;
    RtlEepromInfo info;
    if (!rtlEepromParse(cur, info, rerr)) { err = "This dongle's EEPROM is not in the layout this tool knows (" + rerr + "). Nothing was written."; return false; }
    if (info.serial != expectedSerial && !(!info.haveSerial && isFactoryName(expectedSerial))) {
        err = "The radio list changed; rescan and try again. (The dongle that was opened has serial \"" + info.serial + "\", not \"" + expectedSerial + "\".) Nothing was written.";
        return false;
    }
    std::vector<uint8_t> want = cur;   // worked out before the backup, so a serial that cannot be written leaves no file behind
    if (!rtlEepromSetSerial(want, newSerial, rerr)) { err = rerr + " Nothing was written."; return false; }
    if (sameBytes(want, cur)) return true;   // the dongle already has it
    std::error_code ec;
    if (!planned.empty() && fs::exists(u8file(planned), ec)) { err = "The backup file " + planned + " already exists. Try again. Nothing was written."; return false; }
    const std::string bp = planned.empty() ? rtlBackupPath(backupDir, info.serial, now) : planned;
    if (!writeBackup(backupDir, bp, cur, err)) return false;
    backupPath = bp;
    return writeAndVerify(io, want, cur, 6, bp, "Writing to the dongle failed", "The write did not verify.", err);
}

bool rtlRestoreEepromIo(RtlEepromIo& io, const std::string& backupFile, const std::string& backupDir, time_t now, std::string& safetyBackup, std::string& err,
                        std::string* restoredSerial) {
    err.clear();
    safetyBackup.clear();
    if (restoredSerial) restoredSerial->clear();
    std::vector<uint8_t> bk;
    if (!rtlReadBackupFile(backupFile, bk, err)) return false;
    RtlEepromInfo bi;
    std::string rerr;
    if (!rtlEepromParse(bk, bi, rerr)) { err = "The backup file is not a usable RTL-SDR EEPROM image (" + rerr + "). Nothing was written."; return false; }
    std::vector<uint8_t> cur;
    if (!rtlReadStable(io, cur, err)) return false;
    RtlEepromInfo ci;
    const bool curParses = rtlEepromParse(cur, ci, rerr);
    if (cur[0] == 0x28 && cur[1] == 0x32) {
        if (cur[2] != bk[2] || cur[3] != bk[3] || cur[4] != bk[4] || cur[5] != bk[5]) {
            err = "The backup is from a different kind of dongle (its USB ID differs from this dongle's). Nothing was written.";
            return false;
        }
    } else {   // blank or foreign EEPROM: the dongle shows its chip ids, so the file's id must be one librtlsdr takes, and the dongle's live id when known
        uint16_t lv = 0, lp = 0;
        const bool live = io.liveUsbId(lv, lp);
        if (!rtlKnownUsbId(bi.vid, bi.pid) || (live && (lv != bi.vid || lp != bi.pid))) {
            char b[160];
            snprintf(b, sizeof b, "This backup is for a different USB ID (%04x:%04x); restoring it could hide the dongle. Nothing was written.", bi.vid, bi.pid);
            err = b;
            return false;
        }
    }
    const std::string serialNow = bi.haveSerial ? bi.serial : std::string();
    if (sameBytes(cur, bk)) { if (restoredSerial) *restoredSerial = serialNow; return true; }
    std::error_code ec;
    const std::string sp = rtlBackupPath(backupDir, curParses ? ci.serial : std::string("unknown"), now);
    if (!writeBackup(backupDir, sp, cur, err)) return false;
    safetyBackup = sp;
    if (!writeAndVerify(io, bk, cur, 6, sp, "Restoring failed", "The restore did not verify.", err)) return false;
    if (restoredSerial) *restoredSerial = serialNow;
    return true;
}

bool rtlWriteSerial(const DeviceInfo& d, const std::vector<DeviceInfo>& listed, const std::string& newSerial, const std::string& backupDir, std::string& backupPath,
                    std::string& err) {
    const std::string planned = backupPath;
    backupPath.clear();
    if (!rtlIsRtl2832(d)) { err = "Only an RTL2832U dongle (RTL-SDR) can be given a serial here."; return false; }
    ClaimScope claim(d);
    if (!claim.ok) { err = claim.err; return false; }
    auto io = openRtlEepromIo(d, listed, err);
    if (!io) return false;
    backupPath = planned;
    return rtlWriteSerialIo(*io, d.serial, newSerial, backupDir, time(nullptr), backupPath, err);
}

bool rtlRestoreEeprom(const DeviceInfo& d, const std::vector<DeviceInfo>& listed, const std::string& backupFile, const std::string& backupDir, std::string& safetyBackup,
                      std::string& err, std::string* restoredSerial) {
    safetyBackup.clear();
    if (!rtlIsRtl2832(d)) { err = "Only an RTL2832U dongle (RTL-SDR) can be restored here."; return false; }
    ClaimScope claim(d);
    if (!claim.ok) { err = claim.err; return false; }
    auto io = openRtlEepromIo(d, listed, err);
    if (!io) return false;
    return rtlRestoreEepromIo(*io, backupFile, backupDir, time(nullptr), safetyBackup, err, restoredSerial);
}

bool rtlEepromTargetOk(const DeviceInfo& d, const std::vector<DeviceInfo>& listed, uint32_t n, const std::vector<std::string>& paths, std::string& err) {
    const char* stale = "The radio list changed; rescan and try again.";
    const std::string& a = d.nativeArgs;
    if (a.empty() || a.size() > 2 || a.find_first_not_of("0123456789") != std::string::npos) { err = stale; return false; }
    const uint32_t idx = (uint32_t)strtoul(a.c_str(), nullptr, 10);
    if (idx >= n || idx >= 16) { err = stale; return false; }
    if (!d.usbPath.empty() && paths.size() == n) {   // the walk says which port the index is: it must be d's (look-alikes are told apart by it)
        if (paths[idx] != d.usbPath) { err = stale; return false; }
        return true;
    }
    // no port to check: a twin swapped in at the index would pass the serial checks, so only a lone dongle is written (n is librtlsdr's count,
    // which opens nothing; the caller checks it again after the open). The listing must still be that one dongle.
    if (n != 1) { err = "Unplug the other RTL-SDR dongles first: this system can't tell them apart."; return false; }
    std::vector<std::string> lookAlike, indices;
    for (const DeviceInfo& x : listed) {
        if (!rtlIsRtl2832(x)) continue;
        if (std::find(indices.begin(), indices.end(), x.nativeArgs) == indices.end()) indices.push_back(x.nativeArgs);
        if (x.serial != d.serial) continue;
        const std::string who = x.usbPath.empty() ? "#" + x.nativeArgs : x.usbPath;
        if (std::find(lookAlike.begin(), lookAlike.end(), who) == lookAlike.end()) lookAlike.push_back(who);
    }
    if (lookAlike.size() > 1) { err = "Can't tell these dongles apart here; unplug the others first."; return false; }
    if (indices.size() != std::min<uint32_t>(n, 16)) { err = stale; return false; }
    return true;
}

// ---- the name follows the dongle ----

std::string rtlKeyWithSerial(const DeviceInfo& d, const std::string& newSerial) {
    DeviceInfo n = d;
    n.serial = newSerial;
    return radioKey(n);
}

namespace {
// the name under row.key goes to newKey (unless that has one). A name tied to the USB port belonged to the dongle in it, which no longer needs it;
// an "sn:" name stays unless dropSoleSerial and the serial was this dongle's alone: another dongle with that serial may use it.
bool moveLabel(RadioLabels& labels, const RadioRow& row, const std::string& newKey, bool dropSoleSerial) {
    if (newKey == row.key) return false;
    const std::string name = labels.get(row.key);
    if (name.empty()) return false;
    if (labels.get(newKey).empty()) labels.set(newKey, name);
    if (row.key.find(":usb:") != std::string::npos || (dropSoleSerial && !row.sharedSerial)) labels.remove(row.key);
    return true;
}
}  // namespace

bool rtlCarryLabel(RadioLabels& labels, const RadioRow& row, const std::string& newSerial) {
    return moveLabel(labels, row, rtlKeyWithSerial(row.dev, newSerial), false);
}

std::string rtlKeyWithSerial(const std::vector<DeviceInfo>& attached, const DeviceInfo& d, const std::string& newSerial) {
    std::vector<DeviceInfo> v = attached;
    size_t at = v.size();
    for (size_t i = 0; i < v.size() && at == v.size(); i++)
        if (v[i].kind == d.kind && v[i].board == d.board && v[i].serial == d.serial && v[i].usbPath == d.usbPath && v[i].nativeArgs == d.nativeArgs) at = i;
    if (at == v.size()) v.push_back(d);
    v[at].serial = newSerial;
    std::vector<DeviceInfo> radios;
    size_t mine = 0;
    for (size_t i = 0; i < v.size(); i++) {
        if (!v[i].isRadio()) continue;
        if (i == at) mine = radios.size();
        radios.push_back(v[i]);
    }
    if (!v[at].isRadio()) return rtlKeyWithSerial(d, newSerial);
    return identifyRadios(radios, RadioLabels())[mine].key;
}

bool rtlCarryLabelBack(RadioLabels& labels, const RadioRow& row, const std::vector<DeviceInfo>& attached, const std::string& restoredSerial) {
    return moveLabel(labels, row, rtlKeyWithSerial(attached, row.dev, restoredSerial), true);   // the serial it had was the tool's unique one
}

}  // namespace dect2
