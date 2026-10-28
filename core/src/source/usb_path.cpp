// USB port paths of radios (usb_path.h). Enumeration only.
#include "dect2/source/usb_path.h"
#include <cctype>
#include <atomic>
#include <mutex>

#if !defined(__ANDROID__) && (defined(__linux__) || defined(__APPLE__) || defined(_WIN32))
#define DECT2_USB_PATHS 1
#include <libusb.h>
#endif

namespace dect2 {

std::string usbPathFormat(int bus, const uint8_t* ports, int nPorts) {
    if (bus < 1 || !ports || nPorts < 1) return {};
    std::string s = std::to_string(bus) + "-";
    for (int i = 0; i < nPorts; i++) { if (i) s += '.'; s += std::to_string((int)ports[i]); }
    return s;
}

bool usbPathLooksValid(const std::string& s) {
    size_t i = 0;
    auto digits = [&] { const size_t b = i; while (i < s.size() && isdigit((unsigned char)s[i])) i++; return i > b; };
    if (!digits() || i >= s.size() || s[i++] != '-' || !digits()) return false;
    while (i < s.size()) { if (s[i++] != '.' || !digits()) return false; }
    return true;
}

std::string usbPathOf(libusb_device* dev) {
#ifdef DECT2_USB_PATHS
    if (!dev) return {};
    uint8_t ports[16] = {0};
    const int n = libusb_get_port_numbers(dev, ports, (int)sizeof ports);
    if (n <= 0) return {};
    return usbPathFormat(libusb_get_bus_number(dev), ports, n);
#else
    (void)dev;
    return {};
#endif
}

namespace {
std::atomic<unsigned> gWalks{0};
thread_local int tScopeDepth = 0;
thread_local bool tHave = false;
thread_local std::vector<UsbNode> tSnap;

std::vector<UsbNode> walk() {
    gWalks++;
    std::vector<UsbNode> out;
#ifdef DECT2_USB_PATHS
#ifdef _WIN32
    static std::mutex mu;
    static libusb_context* shared = nullptr;
    static bool failed = false;
    std::lock_guard<std::mutex> lk(mu);
    if (!shared && !failed && libusb_init(&shared) != 0) { shared = nullptr; failed = true; }
    libusb_context* ctx = shared;
#else
    libusb_context* ctx = nullptr;   // fresh for this walk, like the radio libraries' own (see usb_path.h)
    if (libusb_init(&ctx) != 0) return out;
#endif
    libusb_device** list = nullptr;
    const ssize_t n = ctx ? libusb_get_device_list(ctx, &list) : -1;
    if (n < 0) {
#ifndef _WIN32
        if (ctx) libusb_exit(ctx);
#endif
        return out;
    }
    for (ssize_t i = 0; i < n; i++) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
        UsbNode u;
        u.vid = d.idVendor; u.pid = d.idProduct;
        u.bus = libusb_get_bus_number(list[i]); u.address = libusb_get_device_address(list[i]);
        u.path = usbPathOf(list[i]);
        out.push_back(u);
    }
    libusb_free_device_list(list, 1);
#ifndef _WIN32
    libusb_exit(ctx);
#endif
#endif
    return out;
}
}  // namespace

std::vector<UsbNode> usbEnumerateFresh() { return walk(); }
unsigned usbWalkCount() { return gWalks.load(); }

UsbWalkScope::UsbWalkScope() { if (tScopeDepth++ == 0) tHave = false; }
UsbWalkScope::~UsbWalkScope() { if (--tScopeDepth == 0) { tHave = false; tSnap.clear(); } }

std::vector<UsbNode> usbEnumerate() {
    if (tScopeDepth == 0) return walk();
    if (!tHave) { tSnap = walk(); tHave = true; }
    return tSnap;
}

std::vector<std::string> usbPathsInOrder(const std::vector<UsbNode>& nodes, const std::vector<std::pair<uint16_t, uint16_t>>& ids, size_t expected) {
    std::vector<std::string> out;
    for (const auto& n : nodes)
        for (const auto& id : ids)
            if (n.vid == id.first && n.pid == id.second) { out.push_back(n.path); break; }
    if (out.size() != expected) out.clear();
    return out;
}

bool usbParseIioUri(const std::string& uri, int& bus, int& address) {
    if (uri.rfind("usb:", 0) != 0) return false;
    const char* p = uri.c_str() + 4;
    char* e = nullptr;
    const long b = std::strtol(p, &e, 10);
    if (e == p || *e != '.') return false;
    p = e + 1;
    const long a = std::strtol(p, &e, 10);
    if (e == p || (*e != '\0' && *e != '.') || b < 1 || a < 1) return false;
    bus = (int)b; address = (int)a;
    return true;
}

std::string usbPathByBusAddress(const std::vector<UsbNode>& nodes, int bus, int address) {
    for (const auto& n : nodes) if (n.bus == bus && n.address == address) return n.path;
    return {};
}

}  // namespace dect2
