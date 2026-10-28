// Radio identity: keys, duplicate numbering, user names (label file) and diffing two radio lists.
#include "../common/test_util.h"
#include "dect2/source/radio_id.h"
#include "dect2/source/usb_path.h"
#include <map>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace dect2;

static DeviceInfo rtl(const char* serial, const char* usb, const char* name = "Generic RTL2832U") {
    DeviceInfo d;
    d.kind = DeviceInfo::Native;
    d.board = "rtlsdr";
    d.name = name;
    d.serial = serial;
    d.usbPath = usb;
    return d;
}

int main() {
    namespace fs = std::filesystem;

    // unique serial: keyed by serial
    {
        DeviceInfo d = rtl("A1B2C3", "1-1");
        CHECK(radioKey(d) == std::to_string((int)DeviceInfo::Native) + ":rtlsdr:sn:A1B2C3", "sn key: %s", radioKey(d).c_str());
        auto rows = identifyRadios({d}, RadioLabels{});
        CHECK(rows.size() == 1 && !rows[0].sharedSerial && rows[0].dupIndex == 0, "unique row");
    }
    // two RTLs with the same factory serial
    {
        std::vector<DeviceInfo> v = {rtl("00000001", "1-1"), rtl("00000001", "1-2")};
        auto rows = identifyRadios(v, RadioLabels{});
        CHECK(rows.size() == 2, "two rows");
        CHECK(rows[0].key.find(":usb:1-1") != std::string::npos, "usb key 0: %s", rows[0].key.c_str());
        CHECK(rows[1].key.find(":usb:1-2") != std::string::npos, "usb key 1: %s", rows[1].key.c_str());
        CHECK(rows[0].key != rows[1].key, "keys differ");
        CHECK(rows[0].sharedSerial && rows[1].sharedSerial, "shared");
        CHECK(rows[0].dupIndex == 1 && rows[1].dupIndex == 2, "dup %d %d", rows[0].dupIndex, rows[1].dupIndex);
        CHECK(radioDisplayName(rows[0]).size() > 2 && radioDisplayName(rows[0]).substr(radioDisplayName(rows[0]).size() - 3) == " #1", "name #1: %s", radioDisplayName(rows[0]).c_str());
        CHECK(radioDisplayName(rows[1]).substr(radioDisplayName(rows[1]).size() - 3) == " #2", "name #2: %s", radioDisplayName(rows[1]).c_str());
    }
    // a duplicate order does not depend on the list order
    {
        auto rows = identifyRadios({rtl("00000001", "1-2"), rtl("00000001", "1-1")}, RadioLabels{});
        CHECK(rows[0].dupIndex == 2 && rows[1].dupIndex == 1, "dup by path, not list order");
    }
    // the same non-default serial on two radios is shared too
    {
        auto rows = identifyRadios({rtl("77", "1-1"), rtl("77", "1-2")}, RadioLabels{});
        CHECK(rows[0].sharedSerial && rows[1].dupIndex == 2 && rows[0].key.find(":usb:") != std::string::npos, "equal serials shared");
    }
    // factory default alone is shared; not a duplicate
    {
        for (const char* sn : {"00000001", "0", ""}) {
            auto rows = identifyRadios({rtl(sn, "1-1")}, RadioLabels{});
            CHECK(rows[0].sharedSerial && rows[0].dupIndex == 0, "default '%s' shared", sn);
            CHECK(rows[0].key.find(":usb:1-1") != std::string::npos, "default key by usb: %s", rows[0].key.c_str());
        }
        auto rows = identifyRadios({rtl("00000001", "")}, RadioLabels{});
        CHECK(rows[0].key.find(":name:Generic RTL2832U") != std::string::npos, "name fallback: %s", rows[0].key.c_str());
    }
    // different kinds/boards do not make each other shared
    {
        DeviceInfo a = rtl("55", "1-1"), b = rtl("55", "1-2");
        b.board = "airspy";
        auto rows = identifyRadios({a, b}, RadioLabels{});
        CHECK(!rows[0].sharedSerial && !rows[1].sharedSerial, "different boards not shared");
    }
    // display name: details without label, label with label
    {
        RadioRow r;
        r.dev = rtl("A1B2C3", "1-4", "Nooelec NESDR");
        CHECK(radioDisplayName(r) == "Nooelec NESDR \xC2\xB7 SN A1B2C3 \xC2\xB7 USB 1-4", "detail: %s", radioDisplayName(r).c_str());
        r.dev.usbPath.clear();
        CHECK(radioDisplayName(r) == "Nooelec NESDR \xC2\xB7 SN A1B2C3", "no usb: %s", radioDisplayName(r).c_str());
        r.dev.serial.clear();
        CHECK(radioDisplayName(r) == "Nooelec NESDR", "name only: %s", radioDisplayName(r).c_str());
        r.label = "Roof antenna";
        CHECK(radioDisplayName(r) == "Roof antenna", "label: %s", radioDisplayName(r).c_str());
    }
    // a labelled radio shows its label, keyed by usb path for duplicates
    {
        std::vector<DeviceInfo> v = {rtl("00000001", "1-1"), rtl("00000001", "1-2")};
        auto first = identifyRadios(v, RadioLabels{});
        RadioLabels L;
        L.set(first[1].key, "Garage");
        auto rows = identifyRadios(v, L);
        CHECK(rows[0].label.empty() && rows[1].label == "Garage", "label on the right one");
        CHECK(radioDisplayName(rows[1]) == "Garage", "shows label: %s", radioDisplayName(rows[1]).c_str());
    }
    // save/load round trip, mode 0600, no tmp left
    {
        fs::path dir = fs::temp_directory_path() / "radio_id_test";
        fs::remove_all(dir);
        std::string path = (dir / "sub" / "labels.json").string();
        RadioLabels L;
        L.set("k1", "Roof \"north\" \xC3\xA9");
        L.set("k:2", "Garage");
        L.set("k3", "x");
        L.remove("k3");
        std::string err;
        CHECK(L.save(path, err), "save: %s", err.c_str());
        CHECK(!fs::exists(path + ".tmp"), "tmp renamed away");
#ifndef _WIN32
        struct stat st {};
        CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "mode %o", (unsigned)(st.st_mode & 0777));
#endif
        RadioLabels M;
        CHECK(M.load(path, err), "load: %s", err.c_str());
        CHECK(M.get("k1") == "Roof \"north\" \xC3\xA9" && M.get("k:2") == "Garage" && M.get("k3").empty(), "round trip");
        // overwrite again keeps working
        L.set("k:2", "Shed");
        CHECK(L.save(path, err), "save 2: %s", err.c_str());
        RadioLabels N;
        N.load(path, err);
        CHECK(N.get("k:2") == "Shed", "second save");

        // corrupt file: empty with an error
        { std::ofstream o(path, std::ios::binary | std::ios::trunc); o << "{ not json ["; }
        RadioLabels C;
        C.set("stale", "x");
        err.clear();
        CHECK(!C.load(path, err) && !err.empty(), "corrupt reports error");
        CHECK(C.get("stale").empty() && C.get("k1").empty(), "corrupt loads empty");
        // wrong shapes
        { std::ofstream o(path, std::ios::binary | std::ios::trunc); o << "[1,2,3]"; }
        err.clear();
        CHECK(!C.load(path, err) && !err.empty(), "array root rejected");
        { std::ofstream o(path, std::ios::binary | std::ios::trunc); o << "{\"labels\":{\"a\":5,\"b\":\"ok\"}}"; }
        err.clear();
        C.load(path, err);
        CHECK(C.get("a").empty() && C.get("b") == "ok", "non-string value skipped");
        // missing file: empty with an error
        err.clear();
        RadioLabels Z;
        Z.set("stale", "x");
        CHECK(!Z.load((dir / "none.json").string(), err) && !err.empty() && Z.get("stale").empty(), "missing file");
        fs::remove_all(dir);
    }
    // long names are cut to 64
    {
        RadioLabels L;
        L.set("k", std::string(100, 'a'));
        CHECK(L.get("k").size() == 64, "cut to 64: %zu", L.get("k").size());
        std::string u;
        for (int i = 0; i < 40; i++) u += "\xC3\xA9";   // 80 bytes of 2-byte chars
        L.set("u", u);
        std::string g = L.get("u");
        CHECK(g.size() == 64 && g == u.substr(0, 64), "utf-8 cut on a boundary: %zu", g.size());
        L.set("odd", "a" + u);   // byte 64 would land inside a character
        std::string h = L.get("odd");
        CHECK(h.size() == 63 && (static_cast<unsigned char>(h.back()) & 0xC0) == 0x80, "no split character: %zu", h.size());
        L.set("k", "");   // an empty name removes the label
        CHECK(L.get("k").empty(), "empty removes");
    }
    // diff: a swap is one added and one removed
    {
        auto a = rtl("S1", "1-1"), b = rtl("S2", "1-2"), c = rtl("S3", "1-3");
        RadioDiff d = diffRadios({a, b}, {a, c});
        CHECK(d.added.size() == 1 && d.added[0].serial == "S3", "added");
        CHECK(d.removed.size() == 1 && d.removed[0].serial == "S2", "removed");
        RadioDiff e = diffRadios({a, b}, {b, a});
        CHECK(e.added.empty() && e.removed.empty(), "reorder is no change");
        // identical default-serial radios on different ports are told apart by port
        RadioDiff f = diffRadios({rtl("00000001", "1-1")}, {rtl("00000001", "1-2")});
        CHECK(f.added.size() == 1 && f.removed.size() == 1, "move to another port");
    }
    // USB port paths: formatting, order matching, key lookup (no hardware)
    {
        const uint8_t p1[] = {2, 3}, p0[] = {1};
        CHECK(usbPathFormat(1, p1, 2) == "1-2.3", "path 1-2.3: %s", usbPathFormat(1, p1, 2).c_str());
        CHECK(usbPathFormat(12, p0, 1) == "12-1", "path 12-1");
        CHECK(usbPathFormat(0, p0, 1).empty() && usbPathFormat(1, p0, 0).empty() && usbPathFormat(1, nullptr, 1).empty(), "no bus or ports: empty");
        CHECK(usbPathLooksValid("1-2.3.4") && usbPathLooksValid("3-1") && !usbPathLooksValid("1-") && !usbPathLooksValid("1-2.") &&
              !usbPathLooksValid("/dev/ttyUSB0") && !usbPathLooksValid("") && !usbPathLooksValid("usb:1.2"), "path shape");

        std::vector<UsbNode> nodes = {{0x1d6b, 0x0002, 1, 1, ""}, {0x0bda, 0x2838, 1, 7, "1-2"}, {0x1d50, 0x60a1, 1, 8, "1-3"}, {0x0bda, 0x2838, 2, 4, "2-1.4"}};
        const std::vector<std::pair<uint16_t, uint16_t>> rtlIds = {{0x0bda, 0x2832}, {0x0bda, 0x2838}};
        auto v = usbPathsInOrder(nodes, rtlIds, 2);
        CHECK(v.size() == 2 && v[0] == "1-2" && v[1] == "2-1.4", "two dongles in libusb order");
        CHECK(usbPathsInOrder(nodes, rtlIds, 3).empty() && usbPathsInOrder(nodes, rtlIds, 1).empty(), "a count that disagrees gives no path");
        CHECK(usbPathsInOrder(nodes, {{0x1d50, 0x60a1}}, 1).at(0) == "1-3", "one airspy");
        CHECK(usbPathByBusAddress(nodes, 2, 4) == "2-1.4" && usbPathByBusAddress(nodes, 2, 5).empty(), "bus and address lookup");

        int b = 0, a = 0;
        CHECK(usbParseIioUri("usb:1.7.5", b, a) && b == 1 && a == 7 && usbParseIioUri("usb:2.4", b, a) && b == 2 && a == 4, "iio uri");
        CHECK(!usbParseIioUri("ip:192.168.2.1", b, a) && !usbParseIioUri("usb:", b, a) && !usbParseIioUri("usb:x.1", b, a), "iio uri rejects");

        auto kv = [](std::map<std::string, std::string> m) { return [m](const char* k) { auto it = m.find(k); return it == m.end() ? std::string() : it->second; }; };
        CHECK(usbPathFromKeys(nodes, kv({{"usb", "1-2.3"}})) == "1-2.3", "usb key");
        CHECK(usbPathFromKeys(nodes, kv({{"path", "/dev/x"}})).empty(), "a path key that is no USB path");
        CHECK(usbPathFromKeys(nodes, kv({{"bus", "2"}, {"address", "4"}})) == "2-1.4", "bus + address keys");
        CHECK(usbPathFromKeys(nodes, kv({{"bus", "2"}, {"addr", "4"}})) == "2-1.4", "bus + addr keys");
        CHECK(usbPathFromKeys(nodes, kv({{"bus", "2"}})).empty() && usbPathFromKeys(nodes, kv({})).empty(), "nothing to go on");
        // enumeration never throws, and each path it gives has the shape (or is empty)
        for (const auto& n : usbEnumerate()) CHECK(n.path.empty() || usbPathLooksValid(n.path), "live path shape: %s", n.path.c_str());
    }
    // one walk per listing: inside a scope every call returns the same snapshot, nested scopes included; outside, each call is a fresh walk
    // (a hotplug during the scope would not show, which is the point). The fresh-context order itself needs real hardware to check.
    {
        const unsigned w0 = usbWalkCount();
        usbEnumerate(); usbEnumerate();
        CHECK(usbWalkCount() == w0 + 2, "outside a scope every call walks: %u", usbWalkCount() - w0);
        const unsigned w1 = usbWalkCount();
        UsbWalkScope outer;
        const auto a = usbEnumerate();
        usbEnumerate();
        CHECK(usbWalkCount() == w1 + 1, "one scope, one walk: %u", usbWalkCount() - w1);
        usbEnumerateFresh(); usbEnumerateFresh();
        CHECK(usbWalkCount() == w1 + 3, "a fresh walk always walks, scope or not: %u", usbWalkCount() - w1);
        usbEnumerate();
        CHECK(usbWalkCount() == w1 + 3, "the scope snapshot is still reused afterwards");
        { UsbWalkScope inner; CHECK(usbEnumerate().size() == a.size(), "nested scope reuses the walk"); }
        const auto b = usbEnumerate();
        bool same = a.size() == b.size();
        for (size_t i = 0; same && i < a.size(); i++) same = a[i].bus == b[i].bus && a[i].address == b[i].address && a[i].path == b[i].path;
        CHECK(same, "same snapshot in one scope");
    }
    // a name from the user: control characters and newlines go, the ends are trimmed, text and UTF-8 stay
    {
        CHECK(cleanRadioName("Bench\nRTL\t1\r") == "BenchRTL1", "controls stripped: %s", cleanRadioName("Bench\nRTL\t1\r").c_str());
        CHECK(cleanRadioName("  Roof  ") == "Roof", "trimmed");
        CHECK(cleanRadioName(std::string("a\x01" "b\x7f" "c")) == "abc", "DEL and low controls");
        CHECK(cleanRadioName("Ant\xC2\x85" "enna") == "Antenna", "C1 control (NEL) stripped");
        CHECK(cleanRadioName("Ma\xC3\xAD" "n %s %n ##x") == "Ma\xC3\xAD" "n %s %n ##x", "text kept as it is (the shower escapes it)");
        CHECK(cleanRadioName("\n\t ").empty(), "only controls gives empty");
    }
    // a second radio with the same serial turns sn: keys into usb: keys; the label of the first one follows to a usb: key (once)
    {
        RadioLabels L;
        const std::string snKey = radioKey(rtl("A1B2C3", "1-1"));
        L.set(snKey, "Roof");
        // one radio: nothing to move
        auto one = identifyRadios({rtl("A1B2C3", "1-1")}, L);
        CHECK(!migrateLabels(one, L) && one[0].label == "Roof", "unique radio keeps its label, no change");
        // two radios, same serial: the label moves to exactly one usb: key
        auto two = identifyRadios({rtl("A1B2C3", "1-2"), rtl("A1B2C3", "1-1")}, L);
        CHECK(two[0].key.find(":usb:") != std::string::npos && two[0].label.empty() && two[1].label.empty(), "usb keys, no label yet");
        CHECK(migrateLabels(two, L), "migration reports a change");
        int named = 0; for (auto& r : two) if (r.label == "Roof") named++;
        CHECK(named == 1, "exactly one radio got the label: %d", named);
        CHECK(two[1].label == "Roof", "the lowest USB path (#1) gets it");
        CHECK(L.get(two[1].key) == "Roof" && L.get(two[0].key).empty(), "stored under the usb key");
        CHECK(L.get(snKey).empty(), "the old sn: label was moved, not copied");
        // second call and a rebuilt list: no change, the other radio is not named too
        CHECK(!migrateLabels(two, L), "idempotent");
        auto again = identifyRadios({rtl("A1B2C3", "1-2"), rtl("A1B2C3", "1-1")}, L);
        CHECK(!migrateLabels(again, L), "rebuilt list: no change");
        CHECK(again[0].label.empty() && again[1].label == "Roof", "still one name");
        // the user clears the migrated name: it does not come back, on this list or a rebuilt one, however the radios move
        L.remove(two[1].key);
        auto cleared = identifyRadios({rtl("A1B2C3", "1-2"), rtl("A1B2C3", "1-1")}, L);
        CHECK(!migrateLabels(cleared, L) && cleared[0].label.empty() && cleared[1].label.empty(), "cleared name stays cleared");
        auto moved = identifyRadios({rtl("A1B2C3", "1-5"), rtl("A1B2C3", "1-1")}, L);
        CHECK(!migrateLabels(moved, L) && moved[0].label.empty() && moved[1].label.empty(), "a port change does not bring it back");
        // a radio that already has its own label is not touched; other kinds/boards/serials do not borrow
        RadioLabels M;
        M.set(radioKey(rtl("ZZ", "1-1")), "Other");
        auto r3 = identifyRadios({rtl("A1B2C3", "1-1"), rtl("A1B2C3", "1-2")}, M);
        CHECK(!migrateLabels(r3, M), "no label under this serial: nothing");
        // a factory-serial radio never had an sn: key
        RadioLabels F;
        F.set(std::to_string((int)DeviceInfo::Native) + ":rtlsdr:sn:00000001", "stale");
        auto r4 = identifyRadios({rtl("00000001", "1-1"), rtl("00000001", "1-2")}, F);
        CHECK(!migrateLabels(r4, F), "factory serial: no migration");
    }
    // identifySources: one row per entry, only radios get keys and numbers (a sound card or the test signal keeps its own name), labels migrate
    {
        DeviceInfo syn; syn.kind = DeviceInfo::Synthetic; syn.name = "Test signal";
        DeviceInfo snd; snd.kind = DeviceInfo::Audio; snd.name = "USB Audio";
        RadioLabels L;
        L.set(radioKey(rtl("A1B2C3", "1-1")), "Roof");
        bool moved = false;
        auto rows = identifySources({syn, snd, snd, rtl("A1B2C3", "1-2"), rtl("A1B2C3", "1-1")}, L, &moved);
        CHECK(rows.size() == 5 && rows[0].key.empty() && rows[1].key.empty() && rows[1].dupIndex == 0 && rows[2].dupIndex == 0, "non-radios: no key, no number");
        CHECK(rows[3].dupIndex == 2 && rows[4].dupIndex == 1 && rows[4].label == "Roof" && moved, "radios numbered and the label moved");
        CHECK(rows[0].dev.name == "Test signal" && rows[4].dev.usbPath == "1-1", "rows keep their devices in order");
    }
    // findRadioRow: a receiver keeps its exact radio when a look-alike comes or goes (its key flips sn: <-> usb:)
    {
        RadioLabels L;
        DeviceInfo syn; syn.kind = DeviceInfo::Synthetic; syn.name = "Test signal";
        const DeviceInfo mine = rtl("A1B2C3", "1-4"), twin = rtl("A1B2C3", "1-2");
        const std::string aloneKey = identifyRadios({mine}, L)[0].key;
        auto two = identifySources({syn, twin, mine}, L);
        CHECK(two[2].key != aloneKey, "the key flipped");
        CHECK(findRadioRow(two, aloneKey, &mine, mine.name, 1) == 2, "found by port and serial, not the first look-alike");
        CHECK(findRadioRow(two, aloneKey, nullptr, mine.name, 1) == kRadioAmbiguous, "by name alone: ambiguous, none picked");
        CHECK(findRadioRow(two, two[2].key, nullptr, "", 1) == 2, "by key");
        auto back = identifySources({syn, mine}, L);   // the look-alike unplugged: usb: -> sn:
        CHECK(findRadioRow(back, two[2].key, &mine, mine.name, 1) == 1, "back to sn: found by port");
        DeviceInfo other = rtl("ZZ9", "1-4");   // another radio in that port is not it
        auto o = identifySources({syn, other}, L);
        CHECK(findRadioRow(o, aloneKey, &mine, mine.name, 1) == -1, "a different serial in the port: not found");
        DeviceInfo moved = mine; moved.usbPath = "2-1";
        auto m = identifySources({syn, moved}, L);
        CHECK(findRadioRow(m, aloneKey, &mine, mine.name, 1) == 1, "moved port, alone: by key");
        CHECK(findRadioRow(m, "", nullptr, mine.name, 1) == 1, "a unique name still finds it");
        CHECK(findRadioRow(m, "", nullptr, "Test signal", 1) == -1, "rows before first are skipped");
    }
    // short names: the model without driver notes and serials, the user's label first, look-alikes numbered
    {
        DeviceInfo hk; hk.kind = DeviceInfo::HackRF; hk.board = "HackRF One"; hk.serial = "0000000000000000a06063c82d6b63c3"; hk.name = "HackRF One (2d6b63c3)"; hk.usbPath = "1-1";
        DeviceInfo r1 = rtl("00000001", "1-2", "RTL2838UHIDIR 00000001 (native, experimental)");
        DeviceInfo r2 = rtl("00000001", "1-3", "NESDR SMArt v5 00000001 (native, experimental)");
        CHECK(radioModelName(hk) == "HackRF One", "hackrf model: %s", radioModelName(hk).c_str());
        CHECK(radioModelName(r1) == "RTL-SDR", "generic realtek: %s", radioModelName(r1).c_str());
        CHECK(radioModelName(r2) == "NESDR SMArt v5", "nesdr: %s", radioModelName(r2).c_str());
        CHECK(radioModelName(rtl("00000001", "", "Generic RTL2832U OEM :: 00000001")) == "RTL-SDR", "soapy label");
        DeviceInfo net; net.kind = DeviceInfo::Native; net.board = "rtltcp"; net.name = "rtl_tcp 10.0.0.5:1234 (network)";
        CHECK(radioModelName(net) == "rtl_tcp 10.0.0.5:1234 (network)", "a network radio keeps its address");
        DeviceInfo odd; odd.kind = DeviceInfo::Native; odd.name = "(native, experimental)";
        CHECK(!radioModelName(odd).empty(), "never empty");
        DeviceInfo syn; syn.kind = DeviceInfo::Synthetic; syn.name = "Test signal";
        RadioLabels L;
        auto rows = identifySources({syn, hk, r1, r2}, L);
        auto n = radioShortNames(rows);
        CHECK(n.size() == 4 && n[0] == "Test signal" && n[1] == "HackRF One" && n[2] == "RTL-SDR" && n[3] == "NESDR SMArt v5", "short names: %s | %s | %s", n[1].c_str(), n[2].c_str(), n[3].c_str());
        auto ns = radioShortNames(rows, true);
        CHECK(ns[1] == "HackRF One (SN 2d6b63c3)" && ns[2] == "RTL-SDR (SN 00000001)", "with serial: %s, %s", ns[1].c_str(), ns[2].c_str());
        auto twins = radioShortNames(identifySources({syn, rtl("00000001", "1-2", "RTL2838UHIDIR"), rtl("00000001", "1-3", "RTL2838UHIDIR")}, L));
        CHECK(twins[1] == "RTL-SDR #1" && twins[2] == "RTL-SDR #2", "twins numbered: %s, %s", twins[1].c_str(), twins[2].c_str());
        rows[3].label = "Trunk control";
        CHECK(radioShortNames(rows)[3] == "Trunk control", "label wins");
        CHECK(radioScanSummary({"HackRF One", "RTL-SDR (SN 00000001)", "NESDR SMArt v5"}, 3) ==
              "Found 3 radios: HackRF One, RTL-SDR (SN 00000001), NESDR SMArt v5 \xC2\xB7 3 sound cards", "summary: %s",
              radioScanSummary({"HackRF One", "RTL-SDR (SN 00000001)", "NESDR SMArt v5"}, 3).c_str());
        CHECK(radioScanSummary({"HackRF One"}, 1) == "Found 1 radio: HackRF One \xC2\xB7 1 sound card", "one");
        CHECK(radioScanSummary({}, 0) == "No radios found", "none");
        CHECK(radioScanSummary({"100%s %d"}, 0) == "Found 1 radio: 100%s %d", "a name is not a format");
        ScanWords w; w.many = "%s (%d)";
        CHECK(radioScanSummary({"a", "b"}, 0, w) == "a, b (2)", "own words, any order");
    }
    return testSummary("radio_id");
}
