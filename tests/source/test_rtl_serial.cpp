// The RTL-SDR serial tool over a simulated EEPROM (nothing here touches a dongle): the image layout, the new serial, the safeguards of the write
// (backup first, nothing written when the backup fails, a failed write leaves the backup and says how to restore) and the label that follows.
#include "../common/test_util.h"
#include "dect2/source/rtl_serial.h"
#include "dect2/engine/device_claims.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace dect2;
namespace fs = std::filesystem;
using Img = std::vector<uint8_t>;

static void putStr(Img& img, size_t& at, const std::string& s) {
    img[at++] = (uint8_t)(2 + 2 * s.size());
    img[at++] = 0x03;
    for (char c : s) { img[at++] = (uint8_t)c; img[at++] = 0; }
}
// 0x28 0x32, VID 0x0bda, PID 0x2838, serial in use, wakeup/IR byte 0x02, byte 8 = 0x00; strings from 0x09; padding 0xFF after (as a blank EEPROM)
static Img realImage(const std::string& mfr = "Realtek", const std::string& prod = "RTL2838UHIDIR", const std::string& serial = "00000001", bool have = true) {
    Img img(256, 0xFF);
    img[0] = 0x28; img[1] = 0x32; img[2] = 0xda; img[3] = 0x0b; img[4] = 0x38; img[5] = 0x28;
    img[6] = have ? 0xA5 : 0x00; img[7] = 0x02; img[8] = 0x00;
    size_t at = 9;
    putStr(img, at, mfr); putStr(img, at, prod);
    if (have) putStr(img, at, serial);
    return img;
}

struct SimIo : RtlEepromIo {
    Img mem;
    int failAfterBytes = -1;     // a write stops (with an error) once this many bytes have been written in all
    bool dropWrites = false;     // writes report success but change nothing (a dongle that does not take them)
    bool failRead = false;
    int writeCalls = 0;
    size_t bytesWritten = 0;
    explicit SimIo(Img m) : mem(std::move(m)) {}
    bool read(uint32_t off, uint8_t* buf, size_t len, std::string& err) override {
        if (failRead || off + len > mem.size()) { err = "sim read error"; return false; }
        std::copy(mem.begin() + off, mem.begin() + off + len, buf);
        return true;
    }
    bool write(uint32_t off, const uint8_t* buf, size_t len, std::string& err) override {
        writeCalls++;
        if (off + len > mem.size()) { err = "sim range"; return false; }
        for (size_t i = 0; i < len; i++) {
            if (failAfterBytes >= 0 && (int)bytesWritten >= failAfterBytes) { err = "sim: USB transfer failed"; return false; }
            if (!dropWrites) mem[off + i] = buf[i];
            bytesWritten++;
        }
        return true;
    }
};

static fs::path tmpDir(const char* name) {
    fs::path p = fs::temp_directory_path() / (std::string("rtl_serial_test_") + name);
    std::error_code ec; fs::remove_all(p, ec);
    return p;
}
static Img readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return Img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

int main() {
    // ---- parse a real-layout image
    {
        RtlEepromInfo info; std::string err;
        const Img img = realImage();
        CHECK(rtlEepromParse(img, info, err), "real layout parses: %s", err.c_str());
        CHECK(info.vid == 0x0bda && info.pid == 0x2838, "vid/pid %04x:%04x", info.vid, info.pid);
        CHECK(info.haveSerial && info.manufacturer == "Realtek" && info.product == "RTL2838UHIDIR" && info.serial == "00000001", "strings: %s|%s|%s", info.manufacturer.c_str(), info.product.c_str(), info.serial.c_str());
        CHECK(info.mfrAt == 9 && info.prodAt == 9 + 2 + 14 && info.serialAt == info.prodAt + 2 + 26 && info.stringsEnd == info.serialAt + 18, "offsets");
        RtlEepromInfo noSerial;
        CHECK(rtlEepromParse(realImage("Realtek", "RTL2838UHIDIR", "", false), noSerial, err) && !noSerial.haveSerial && noSerial.serial.empty(), "no serial string");
    }
    // ---- refused images
    {
        RtlEepromInfo info; std::string err;
        CHECK(!rtlEepromParse(Img(100, 0), info, err) && !err.empty(), "too small");
        CHECK(!rtlEepromParse(Img(300, 0), info, err), "too large");
        Img bad = realImage(); bad[0] = 0xFF;
        CHECK(!rtlEepromParse(bad, info, err) && !err.empty(), "bad header");
        CHECK(!rtlEepromParse(Img(256, 0xFF), info, err), "blank EEPROM");
        Img badStr = realImage(); badStr[10] = 0x04;   // the 0x03 type byte of the first string
        CHECK(!rtlEepromParse(badStr, info, err), "a string without type 3");
        Img odd = realImage(); odd[9] = 0x0f;   // odd length
        CHECK(!rtlEepromParse(odd, info, err), "odd string length");
        Img runs = realImage(); runs[9] = 0xFE;   // the first string claims to run to the end
        CHECK(!rtlEepromParse(runs, info, err), "a string that runs out of the image");
        Img zero = realImage(); zero[9] = 0;
        CHECK(!rtlEepromParse(zero, info, err), "a zero length string");
    }
    // ---- set a serial, parse it back, keep everything else
    {
        for (bool have : {true, false}) {
            const Img orig = realImage("Realtek", "RTL2838UHIDIR", "00000001", have);
            Img img = orig; std::string err;
            CHECK(rtlEepromSetSerial(img, "48213907", err), "set: %s", err.c_str());
            RtlEepromInfo before, after;
            rtlEepromParse(orig, before, err);
            CHECK(rtlEepromParse(img, after, err), "parse back: %s", err.c_str());
            CHECK(after.serial == "48213907" && after.haveSerial && img[6] == 0xA5, "round trip");
            CHECK(after.manufacturer == before.manufacturer && after.product == before.product, "manufacturer and product");
            CHECK(std::equal(orig.begin() + 9, orig.begin() + before.serialAt, img.begin() + 9), "manufacturer and product bytes identical");
            // every byte other than the string area and the have_serial flag is unchanged
            const size_t strEnd = std::max(before.stringsEnd, after.stringsEnd);
            size_t changedOutside = 0;
            for (size_t i = 0; i < 256; i++)
                if ((i < 9 || i >= strEnd) && i != 6 && img[i] != orig[i]) changedOutside++;
            CHECK(changedOutside == 0, "%zu bytes changed outside the strings", changedOutside);
            CHECK(img.size() == 256, "size");
        }
        // a different manufacturer / product (not the sample) stay exactly
        Img odd = realImage("RTLSDRBlog", "Blog V4", "00000001"), orig = odd; std::string err;
        CHECK(rtlEepromSetSerial(odd, "10000002", err) && std::equal(orig.begin() + 9, orig.begin() + 9 + 22 + 16, odd.begin() + 9), "Blog strings kept");
        // setting the same serial again changes nothing
        Img again = odd;
        CHECK(rtlEepromSetSerial(again, "10000002", err) && again == odd, "idempotent");
    }
    // ---- a shorter old serial is padded with zero, a longer one needs padding after the strings
    {
        std::string err;
        Img shortSer = realImage("Realtek", "RTL2838UHIDIR", "12345");
        Img grown = shortSer;
        CHECK(rtlEepromSetSerial(grown, "48213907", err), "grows into the padding: %s", err.c_str());
        Img longer = realImage("Realtek", "RTL2838UHIDIR", "123456789012"), shrunk = longer;
        CHECK(rtlEepromSetSerial(shrunk, "48213907", err), "shrinks: %s", err.c_str());
        RtlEepromInfo i2; CHECK(rtlEepromParse(shrunk, i2, err) && i2.serial == "48213907", "shrunk parses");
        bool tailZero = true; for (size_t i = i2.stringsEnd; i < i2.stringsEnd + 8; i++) tailZero &= shrunk[i] == 0x00;
        CHECK(tailZero, "freed bytes are zero");
        // data after the strings that is not padding is not overwritten
        Img data = shortSer; RtlEepromInfo i3; rtlEepromParse(data, i3, err);
        data[i3.stringsEnd + 2] = 0x42;
        const Img keep = data;
        CHECK(!rtlEepromSetSerial(data, "48213907", err) && data == keep && !err.empty(), "refuses to overwrite other data");
    }
    // ---- an overflowing serial, a non-digit, too long, too short
    {
        std::string err;
        Img full(256, 0xFF); full[0] = 0x28; full[1] = 0x32; full[6] = 0xA5; size_t at = 9;
        putStr(full, at, std::string(58, 'm')); putStr(full, at, std::string(58, 'p')); putStr(full, at, "1");   // 9+118+118+4 = 249; needs 18 -> 263
        const Img keep = full; Img t = full;
        RtlEepromInfo fi; CHECK(rtlEepromParse(full, fi, err) && fi.stringsEnd == 249, "overflow image parses (end %zu)", fi.stringsEnd);
        CHECK(!rtlEepromSetSerial(t, "48213907", err) && t == keep && has(err, "fit"), "overflow refused, image untouched: %s", err.c_str());
        Img r = realImage();
        CHECK(!rtlEepromSetSerial(r, "4821390A", err) && !err.empty(), "non-digit");
        CHECK(!rtlEepromSetSerial(r, "482139071", err), "9 digits");
        CHECK(!rtlEepromSetSerial(r, "4821390", err), "7 digits");
        CHECK(!rtlEepromSetSerial(r, "", err), "empty");
        CHECK(!rtlEepromSetSerial(r, "4821 907", err), "space");
        Img small(100, 0); CHECK(!rtlEepromSetSerial(small, "48213907", err), "too small image");
        Img badh = realImage(); badh[1] = 0; CHECK(!rtlEepromSetSerial(badh, "48213907", err), "bad header");
        CHECK(r == realImage(), "refusals left the image alone");
    }
    // ---- picking a serial
    {
        std::vector<std::string> taken = {"00000001", "12345678", "", "ABC"};
        std::set<std::string> seen;
        for (uint32_t s = 0; s < 500; s++) {
            const std::string p = rtlPickSerial(taken, s);
            CHECK(p.size() == 8 && std::all_of(p.begin(), p.end(), [](char c) { return c >= '0' && c <= '9'; }), "8 digits: %s", p.c_str());
            CHECK(p != "00000000" && p != "00000001" && p != "12345678", "avoids taken and defaults: %s", p.c_str());
            seen.insert(p);
        }
        CHECK(seen.size() > 400, "different seeds give different serials (%zu)", seen.size());
        CHECK(rtlPickSerial(taken, 7) == rtlPickSerial(taken, 7), "deterministic");
        // the first candidate is taken: the next one is not
        const std::string first = rtlPickSerial({}, 99);
        CHECK(rtlPickSerial({first}, 99) != first, "skips a taken one");
        std::string err;
        CHECK(rtlSerialUsable("48213907", taken, err), "usable");
        CHECK(!rtlSerialUsable("12345678", taken, err) && has(err, "already"), "taken: %s", err.c_str());
        CHECK(!rtlSerialUsable("00000001", {}, err) && !rtlSerialUsable("00000000", {}, err), "defaults");
        CHECK(!rtlSerialUsable("1234567", {}, err) && !rtlSerialUsable("1234567a", {}, err), "format");
    }
    // ---- the write: all good
    {
        const fs::path dir = tmpDir("ok");
        SimIo io(realImage());
        const Img orig = io.mem;
        std::string bp, err;
        CHECK(rtlWriteSerialIo(io, "48213907", dir.string(), 1700000000, bp, err), "write: %s", err.c_str());
        CHECK(fs::exists(bp) && readFile(bp) == orig && readFile(bp).size() == 256, "backup holds the original");
        CHECK(has(bp, "eeprom-backup-00000001-") && bp.size() > 4 && bp.substr(bp.size() - 4) == ".bin", "backup name: %s", bp.c_str());
        RtlEepromInfo info; rtlEepromParse(io.mem, info, err);
        CHECK(info.serial == "48213907", "the dongle has the new serial");
        CHECK(io.bytesWritten <= 20 + 1 && io.bytesWritten > 0, "wrote only what changed (%zu bytes)", io.bytesWritten);
        CHECK(io.mem[6] == 0xA5, "flag");
#ifndef _WIN32
        struct stat st{}; stat(bp.c_str(), &st);
        CHECK((st.st_mode & 0777) == 0600, "backup mode %o", (unsigned)(st.st_mode & 0777));
#endif
        CHECK(!fs::exists(bp + ".tmp"), "no temp file left");
        // the same second again: a new file, the first is kept
        SimIo io2(realImage()); std::string bp2;
        CHECK(rtlWriteSerialIo(io2, "48213908", dir.string(), 1700000000, bp2, err) && bp2 != bp && fs::exists(bp) && fs::exists(bp2), "a second backup does not replace the first");
        CHECK(rtlListBackups(dir.string()).size() == 2, "listed");
        // a serial that is already the dongle's: nothing written
        SimIo io3(io.mem); std::string bp3;
        CHECK(rtlWriteSerialIo(io3, "48213907", dir.string(), 1700000100, bp3, err) && io3.writeCalls == 0, "same serial: no write");
        fs::remove_all(dir);
    }
    // ---- a write that fails at byte N: the backup stays, the restore hint is given, the restore works
    {
        for (int n : {0, 1, 5, 10, 14}) {
            const fs::path dir = tmpDir("fail");
            SimIo io(realImage());
            const Img orig = io.mem;
            io.failAfterBytes = n;
            std::string bp, err;
            CHECK(!rtlWriteSerialIo(io, "48213907", dir.string(), 1700000000, bp, err), "fails at %d", n);
            CHECK(!bp.empty() && fs::exists(bp) && readFile(bp) == orig, "backup in place (n=%d)", n);
            CHECK(has(err, "Restore from backup") && has(err, bp.c_str()), "restore hint with the path (n=%d): %s", n, err.c_str());
            io.failAfterBytes = -1;
            std::string rerr;
            CHECK(rtlRestoreEepromIo(io, bp, rerr), "restore: %s", rerr.c_str());
            CHECK(io.mem == orig, "restored to the original (n=%d)", n);
            fs::remove_all(dir);
        }
    }
    // ---- a dongle without a serial string: the strings go first and the flag byte last, so a failure before the flag leaves it off
    {
        const fs::path dir = tmpDir("flag");
        SimIo io(realImage("Realtek", "RTL2838UHIDIR", "", false));
        const Img orig = io.mem;
        io.failAfterBytes = 18;   // all 18 bytes of the descriptor are in, the flag is not
        std::string bp, err;
        CHECK(!rtlWriteSerialIo(io, "48213907", dir.string(), 1700000000, bp, err) && io.mem[6] == 0x00 && has(err, "Restore from backup"), "flag is written last: %s", err.c_str());
        io.failAfterBytes = -1;
        CHECK(rtlRestoreEepromIo(io, bp, err) && io.mem == orig, "restored: %s", err.c_str());
        fs::remove_all(dir);
    }
    // ---- a dongle that accepts the write but does not keep it: "did not verify"
    {
        const fs::path dir = tmpDir("verify");
        SimIo io(realImage()); io.dropWrites = true;
        std::string bp, err;
        CHECK(!rtlWriteSerialIo(io, "48213907", dir.string(), 1700000000, bp, err), "no verify");
        CHECK(has(err, "The write did not verify.") && has(err, "Your original EEPROM is saved at ") && has(err, bp.c_str()) && has(err, "Use Restore from backup… to put it back."), "text: %s", err.c_str());
        fs::remove_all(dir);
    }
    // ---- a failed backup means no EEPROM write at all
    {
        const fs::path dir = tmpDir("nobackup");
        fs::create_directories(dir);
        const fs::path blocker = dir / "file";
        { std::ofstream f(blocker); f << "x"; }
        SimIo io(realImage());
        std::string bp, err;
        CHECK(!rtlWriteSerialIo(io, "48213907", (blocker / "sub").string(), 1700000000, bp, err) && !err.empty(), "backup dir cannot be made");
        CHECK(io.writeCalls == 0 && io.bytesWritten == 0 && io.mem == realImage(), "nothing written to the EEPROM");
        // a directory in the way of the backup file itself
        const fs::path dir2 = dir / "d2"; fs::create_directories(dir2);
        const std::string taken = rtlBackupPath(dir2.string(), "00000001", 1700000000);
        fs::create_directories(taken); fs::create_directories(taken + "-2");
        // (the name is also taken by directories: another name is found, or the write is refused; either way not without a backup)
        std::string bp2, err2; SimIo io2(realImage());
        const bool ok = rtlWriteSerialIo(io2, "48213907", dir2.string(), 1700000000, bp2, err2);
        CHECK(!ok ? io2.writeCalls == 0 : (!bp2.empty() && fs::is_regular_file(bp2) && readFile(bp2) == realImage()), "never written without a backup file");
        fs::remove_all(dir);
    }
    // ---- an EEPROM that does not parse, a failed read: refused, nothing written, no backup
    {
        const fs::path dir = tmpDir("noparse");
        Img foreign(256, 0xFF);
        SimIo io(foreign); std::string bp, err;
        CHECK(!rtlWriteSerialIo(io, "48213907", dir.string(), 1700000000, bp, err) && io.writeCalls == 0 && bp.empty(), "blank EEPROM refused");
        SimIo io2(realImage()); io2.failRead = true;
        CHECK(!rtlWriteSerialIo(io2, "48213907", dir.string(), 1700000000, bp, err) && io2.writeCalls == 0, "unreadable refused");
        SimIo io3(realImage()); std::string bp3;
        CHECK(!rtlWriteSerialIo(io3, "4821390x", dir.string(), 1700000000, bp3, err) && io3.writeCalls == 0 && bp3.empty(), "bad serial: no backup, no write");
        CHECK(!fs::exists(dir) || fs::is_empty(dir), "no file made");
        fs::remove_all(dir);
    }
    // ---- restore: validation
    {
        const fs::path dir = tmpDir("restore"); fs::create_directories(dir);
        auto put = [&](const char* n, const Img& d) { const std::string p = (dir / n).string(); std::ofstream f(p, std::ios::binary); f.write((const char*)d.data(), (std::streamsize)d.size()); return p; };
        SimIo io(realImage("Realtek", "RTL2838UHIDIR", "48213907")); std::string err;
        const Img before = io.mem;
        CHECK(!rtlRestoreEepromIo(io, put("short.bin", Img(100, 0x28)), err) && io.writeCalls == 0, "short backup refused");
        CHECK(!rtlRestoreEepromIo(io, put("big.bin", Img(300, 0x28)), err) && io.writeCalls == 0, "long backup refused");
        Img badh = realImage(); badh[0] = 0;
        CHECK(!rtlRestoreEepromIo(io, put("badh.bin", badh), err) && io.writeCalls == 0, "bad header refused");
        CHECK(!rtlRestoreEepromIo(io, (dir / "missing.bin").string(), err) && io.writeCalls == 0, "missing file");
        CHECK(io.mem == before, "dongle untouched");
        const Img good = realImage();
        CHECK(rtlRestoreEepromIo(io, put("good.bin", good), err) && io.mem == good, "restore: %s", err.c_str());
        SimIo drop(realImage("Realtek", "RTL2838UHIDIR", "48213907")); drop.dropWrites = true;
        CHECK(!rtlRestoreEepromIo(drop, put("good.bin", good), err) && has(err, "did not verify"), "restore verifies: %s", err.c_str());
        fs::remove_all(dir);
    }
    // ---- the real entry points refuse what they must before any hardware
    {
        std::string bp, err;
        DeviceInfo hackrf; hackrf.kind = DeviceInfo::HackRF; hackrf.board = "HackRF One"; hackrf.serial = "0123";
        CHECK(!rtlIsRtl2832(hackrf) && !rtlWriteSerial(hackrf, "48213907", "/nonexistent", bp, err) && has(err, "RTL2832U"), "not an RTL2832U: %s", err.c_str());
        DeviceInfo soapy; soapy.kind = DeviceInfo::Soapy; soapy.board = "rtlsdr";
        CHECK(!rtlIsRtl2832(soapy) && !rtlRestoreEeprom(soapy, "/nonexistent", err), "soapy refused");
        DeviceInfo rtl; rtl.kind = DeviceInfo::Native; rtl.board = "rtlsdr"; rtl.serial = "00000001"; rtl.nativeArgs = "0"; rtl.usbPath = "1-2";
        CHECK(rtlIsRtl2832(rtl), "native rtlsdr");
        CHECK(claimDevice(rtl, 2), "claimed by RX 2");
        CHECK(!rtlWriteSerial(rtl, "48213907", "/nonexistent", bp, err) && has(err, "in use by RX 2"), "claimed: %s", err.c_str());
        CHECK(!rtlRestoreEeprom(rtl, "/nonexistent", err) && has(err, "in use by RX 2"), "restore claimed: %s", err.c_str());
        releaseDevice(2);
        CHECK(!anyDeviceClaimed(), "the tool's claim does not linger after a refusal");
    }
    // ---- the label follows the dongle
    {
        DeviceInfo a; a.kind = DeviceInfo::Native; a.board = "rtlsdr"; a.serial = "00000001"; a.usbPath = "1-2"; a.nativeArgs = "0";
        DeviceInfo b = a; b.usbPath = "1-3"; b.nativeArgs = "1";
        RadioLabels labels;
        std::vector<RadioRow> rows = identifyRadios({a, b}, labels);
        const std::string newKey = rtlKeyWithSerial(a, "48213907");
        CHECK(newKey == std::to_string((int)DeviceInfo::Native) + ":rtlsdr:sn:48213907", "new key %s", newKey.c_str());
        labels.set(rows[0].key, "Kitchen window");
        labels.set(rows[1].key, "Garage");
        CHECK(rtlCarryLabel(labels, rows[0], "48213907"), "carried");
        CHECK(labels.get(newKey) == "Kitchen window", "name under the new serial");
        CHECK(labels.get(rows[0].key).empty() && labels.get(rows[1].key) == "Garage", "the port's name moved, the other radio's stays");
        // a dongle with an old sn: label (unique serial, not shared)
        DeviceInfo c = a; c.serial = "5550001"; RadioLabels l2; auto r2 = identifyRadios({c}, l2);
        l2.set(r2[0].key, "Shack");
        CHECK(rtlCarryLabel(l2, r2[0], "48213907") && l2.get(rtlKeyWithSerial(c, "48213907")) == "Shack", "sn: label carried");
        // nothing to carry
        RadioLabels l3; CHECK(!rtlCarryLabel(l3, r2[0], "48213907"), "no label, no change");
        // the new key already has a name: it is kept
        RadioLabels l4; auto r4 = identifyRadios({a, b}, l4);
        l4.set(r4[0].key, "Old"); l4.set(newKey, "Already");
        rtlCarryLabel(l4, r4[0], "48213907");
        CHECK(l4.get(newKey) == "Already", "existing label kept");
    }
    return testSummary("rtl_serial");
}
