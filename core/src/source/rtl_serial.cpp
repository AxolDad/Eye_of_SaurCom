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
    if (at + 2 > img.size()) { err = std::string("The EEPROM has no ") + what + " string."; return false; }
    const size_t len = img[at];
    if (len < 2 || (len & 1) || img[at + 1] != 0x03 || at + len > img.size()) { err = std::string("The ") + what + " string in the EEPROM is not a USB string descriptor."; return false; }
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
    std::ifstream f(u8file(path), std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
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
    if (ok) { fs::rename(u8file(tmp), u8file(path), ec); ok = !ec; }
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
    if (newEnd > kRtlEepromSize) { err = "The new serial does not fit in this dongle's EEPROM (the manufacturer and product names leave too little room)."; return false; }
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

// ---- the write ----

bool rtlWriteSerialIo(RtlEepromIo& io, const std::string& newSerial, const std::string& backupDir, time_t now, std::string& backupPath, std::string& err) {
    err.clear();
    backupPath.clear();
    std::string rerr;
    std::vector<uint8_t> cur;
    if (!readImage(io, cur, rerr)) { err = "The dongle's EEPROM could not be read (" + rerr + "). Nothing was written."; return false; }
    RtlEepromInfo info;
    if (!rtlEepromParse(cur, info, rerr)) { err = "This dongle's EEPROM is not in the layout this tool knows (" + rerr + "). Nothing was written."; return false; }
    std::vector<uint8_t> want = cur;   // worked out before the backup, so a serial that cannot be written leaves no file behind
    if (!rtlEepromSetSerial(want, newSerial, rerr)) { err = rerr + " Nothing was written."; return false; }
    if (sameBytes(want, cur)) return true;   // the dongle already has it
    const std::string bp = rtlBackupPath(backupDir, info.serial, now);
    if (!writeBackup(backupDir, bp, cur, err)) return false;
    backupPath = bp;
    return writeAndVerify(io, want, cur, 6, bp, "Writing to the dongle failed", "The write did not verify.", err);
}

bool rtlRestoreEepromIo(RtlEepromIo& io, const std::string& backupPath, std::string& err) {
    err.clear();
    std::vector<uint8_t> bk;
    if (!readFile(backupPath, bk)) { err = "Cannot read the backup file " + backupPath + "."; return false; }
    if (bk.size() != kRtlEepromSize) { err = "The backup file is " + std::to_string(bk.size()) + " bytes, not 256: it is not an EEPROM backup."; return false; }
    if (bk[0] != 0x28 || bk[1] != 0x32) { err = "The backup file does not start with the RTL2832U header (28 32): it is not an EEPROM backup."; return false; }
    std::vector<uint8_t> cur;
    std::string rerr;
    if (!readImage(io, cur, rerr)) { err = "The dongle's EEPROM could not be read (" + rerr + "). Nothing was written."; return false; }
    if (sameBytes(cur, bk)) return true;
    return writeAndVerify(io, bk, cur, 6, backupPath, "Restoring failed", "The restore did not verify.", err);
}

bool rtlWriteSerial(const DeviceInfo& d, const std::string& newSerial, const std::string& backupDir, std::string& backupPath, std::string& err) {
    backupPath.clear();
    if (!rtlIsRtl2832(d)) { err = "Only an RTL2832U dongle (RTL-SDR) can be given a serial here."; return false; }
    ClaimScope claim(d);
    if (!claim.ok) { err = claim.err; return false; }
    auto io = openRtlEepromIo(d, err);
    if (!io) return false;
    return rtlWriteSerialIo(*io, newSerial, backupDir, time(nullptr), backupPath, err);
}

bool rtlRestoreEeprom(const DeviceInfo& d, const std::string& backupPath, std::string& err) {
    if (!rtlIsRtl2832(d)) { err = "Only an RTL2832U dongle (RTL-SDR) can be restored here."; return false; }
    ClaimScope claim(d);
    if (!claim.ok) { err = claim.err; return false; }
    auto io = openRtlEepromIo(d, err);
    if (!io) return false;
    return rtlRestoreEepromIo(*io, backupPath, err);
}

// ---- the name follows the dongle ----

std::string rtlKeyWithSerial(const DeviceInfo& d, const std::string& newSerial) {
    DeviceInfo n = d;
    n.serial = newSerial;
    return radioKey(n);
}

bool rtlCarryLabel(RadioLabels& labels, const RadioRow& row, const std::string& newSerial) {
    const std::string newKey = rtlKeyWithSerial(row.dev, newSerial);
    if (newKey == row.key) return false;
    const std::string name = labels.get(row.key);
    if (name.empty()) return false;
    if (labels.get(newKey).empty()) labels.set(newKey, name);
    // a name tied to the USB port belonged to the dongle in it, which no longer needs it; an "sn:" name stays: another dongle with that serial may use it
    if (row.key.find(":usb:") != std::string::npos) labels.remove(row.key);
    return true;
}

}  // namespace dect2
