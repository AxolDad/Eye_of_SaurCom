// Radio identity: keys, duplicate numbering, user names (label file) and diffing two radio lists.
#include "../common/test_util.h"
#include "dect2/source/radio_id.h"
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
    return testSummary("radio_id");
}
