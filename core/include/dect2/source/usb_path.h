// The USB port path of a radio ("<bus>-<port>.<port>...", as Linux sysfs names it: 1-2.3 is bus 1, port 2 of the root hub, port 3 of the hub
// there). Two identical radios (same VID/PID/serial) differ only by the port they are plugged into, so this is what tells them apart.
// Enumeration only: no device is ever opened to read it. A platform or library that cannot supply a path gives an empty string.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

struct libusb_device;

namespace dect2 {

// "<bus>-<p1>.<p2>..." from the bus number and the port chain from the root hub down. Empty when the bus is below 1 or there are no ports
// (a device that reports no chain, e.g. a root hub).
std::string usbPathFormat(int bus, const uint8_t* ports, int nPorts);

// The path of one libusb device (libusb_get_bus_number + libusb_get_port_numbers); empty where libusb cannot say.
std::string usbPathOf(libusb_device* dev);

// One USB device as libusb lists it.
struct UsbNode {
    uint16_t vid = 0, pid = 0;
    int bus = 0, address = 0;
    std::string path;
};

// Every USB device in libusb's enumeration order. On Linux and macOS this uses a fresh libusb context for every walk (init, list, exit), exactly as
// librtlsdr and libairspy do: a context kept alive orders a replugged device differently from a fresh one (hotplug backends insert at the head),
// and usbPathsInOrder() is only right when both sides see the same order. Windows keeps one context (it has no such reordering; exiting a context
// there is what source_airspy.cpp warns about). Inside a UsbWalkScope the first walk is reused by the later calls of the same thread.
// Nothing is opened. Empty if libusb is unavailable or on a platform without port numbers (Android hands radios over as descriptors).
std::vector<UsbNode> usbEnumerate();

// A walk that ignores any scope (and reads the USB tree now). For the radios matched by library order (RTL, Airspy, Airspy HF+): take it right
// beside the library's own listing call, because a replug between the two walks changes the order while keeping the counts.
std::vector<UsbNode> usbEnumerateFresh();

// How many times libusb has been walked by this module in the whole process (for tests: reuse versus a fresh walk).
unsigned usbWalkCount();

// One listing = one USB walk: while a scope is alive on this thread, usbEnumerate() walks once and returns that snapshot to every later call.
// Put one around a whole radio listing (listRadios, listNativeDevices). Scopes nest.
class UsbWalkScope {
public:
    UsbWalkScope();
    ~UsbWalkScope();
    UsbWalkScope(const UsbWalkScope&) = delete;
    UsbWalkScope& operator=(const UsbWalkScope&) = delete;
};

// The paths of the nodes whose VID/PID is in ids, in libusb order, for a library that numbers its radios in that same order (librtlsdr and
// libairspy do). Only when exactly `expected` such nodes exist: any other count means the library skipped a radio or saw one libusb does not,
// the order could not be trusted, and the result is empty.
std::vector<std::string> usbPathsInOrder(const std::vector<UsbNode>& nodes, const std::vector<std::pair<uint16_t, uint16_t>>& ids, size_t expected);

// The path of the node at this bus and address (libusb_get_bus_number / libusb_get_device_address); empty when there is none.
std::string usbPathByBusAddress(const std::vector<UsbNode>& nodes, int bus, int address);

// "usb:<bus>.<address>[.<interface>]" (a libiio USB URI, e.g. a PlutoSDR) to its bus number and device address; false for anything else.
bool usbParseIioUri(const std::string& uri, int& bus, int& address);

// A path out of a SoapySDR-style key/value set: a "usb" or "path" value that has the sysfs shape, else "bus" + "address"/"addr" looked up in
// nodes. Empty otherwise. keys: the value for a name, or "" when absent.
template <class Get> std::string usbPathFromKeys(const std::vector<UsbNode>& nodes, Get get);
bool usbPathLooksValid(const std::string& s);   // digits "-" digits ("." digits)*

template <class Get> std::string usbPathFromKeys(const std::vector<UsbNode>& nodes, Get get) {
    for (const char* k : {"usb", "path"}) {
        const std::string v = get(k);
        if (usbPathLooksValid(v)) return v;
    }
    const std::string b = get("bus");
    std::string a = get("address");
    if (a.empty()) a = get("addr");
    if (b.empty() || a.empty()) return {};
    char* e1 = nullptr; char* e2 = nullptr;
    const long bus = std::strtol(b.c_str(), &e1, 10), adr = std::strtol(a.c_str(), &e2, 10);
    if (*e1 || *e2) return {};
    return usbPathByBusAddress(nodes, (int)bus, (int)adr);
}

}  // namespace dect2
