// ESP32 boards as radios: see esp_sdr.h
#include "esp_sdr.h"
#include <map>

namespace dect2 {
namespace {

std::map<std::string, std::string> kwargs(const std::string& args) {
    std::map<std::string, std::string> m;
    size_t pos = 0;
    while (pos <= args.size()) {
        size_t e = args.find(',', pos);
        if (e == std::string::npos) e = args.size();
        const std::string kv = args.substr(pos, e - pos);
        const size_t q = kv.find('=');
        if (q != std::string::npos) {
            auto trim = [](std::string s) { while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1); while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back(); return s; };
            m[trim(kv.substr(0, q))] = trim(kv.substr(q + 1));
        }
        pos = e + 1;
    }
    return m;
}

} // namespace

bool espSdrDescribe(const std::string& soapyArgs, DeviceInfo& d) {
    const auto kw = kwargs(soapyArgs);
    auto get = [&](const char* k) { auto it = kw.find(k); return it == kw.end() ? std::string() : it->second; };
    const std::string driver = get("driver");
    if (driver != "espsdr" && driver != "esdr") return false;
    const bool known = d.maxRateHz > 0;   // a started radio reported its ranges: those stay
    auto rates = [&](std::initializer_list<double> rs) {
        if (known) return;
        d.rateRanges.clear();
        for (double r : rs) d.rateRanges.push_back({r, r});
        d.minRateHz = *rs.begin(); d.maxRateHz = *(rs.end() - 1);
    };
    const std::string host = get("host"), device = get("device"), port = get("port"), usbSerial = get("usb_serial"), serial = get("serial");
    if (!known) { d.canTransmit = false; d.txChannels = 1; }
    if (driver == "esdr") {   // esp-sdr-zephyr: USB, or an ESP32-C6 on a serial link (port=)
        if (d.name.empty()) d.name = "ESP-SDR Zephyr ESP32" + (port.empty() ? (serial.empty() ? std::string() : " " + serial) : " on " + port);
        rates({15625, 31250, 62500, 125000, 250000});
        if (!known) { d.gainMinDb = 0; d.gainMaxDb = 82; d.canTransmit = true; }   // half duplex: the driver stops receiving while it transmits
        if (d.canTransmit) d.txGainNote = "the ESP32 PHY's TX power step, 0 (weakest) to 17: not dB";
        return true;
    }
    if (!device.empty()) {   // the serial SoapyESPSDR for an ESP32-S3
        if (d.name.empty()) d.name = "ESP-SDR ESP32-S3 on " + device;
        rates({15625, 31250, 62500, 125000, 250000, 312500, 625000});
        if (!known) { d.minFreqHz = 100e6; d.maxFreqHz = 6000e6; d.gainMinDb = 0; d.gainMaxDb = 82; }
        return true;
    }
    // ESPARGOS SoapyESPSDR: the ESP32-S31 board over USB (usb=1) or Ethernet (host=)
    if (!host.empty()) {
        if (d.name.empty()) d.name = "ESP-SDR ESP32-S31 " + host + " (network)";
        d.network = true;
        rates({4e6, 8e6, 16e6, 20e6, 40e6});
    } else {
        if (d.name.empty()) d.name = "ESP-SDR ESP32-S31 USB" + (usbSerial.empty() ? std::string() : " " + usbSerial);
        rates({4e6, 8e6, 16e6, 20e6});
    }
    if (!known) { d.minFreqHz = 2150e6; d.maxFreqHz = 2800e6; }
    return true;
}

std::string espSdrEthernetArgs(const std::string& host, int port) {
    std::string a = "driver=espsdr,host=" + host;
    if (port > 0 && port != 80) a += ",http_port=" + std::to_string(port);
    return a;
}

const char* espSdrHelp() {
    return "An ESP32 board running an ESP-SDR firmware is a receiver for the 2.4 GHz band through its SoapySDR module: SoapyESPSDR (ESPARGOS) for the "
           "ESP32-S31 board over USB or Ethernet at 4 to 20 Msps (40 over Ethernet), 2150 to 2800 MHz; the serial SoapyESPSDR for an ESP32-S3 at up to "
           "625 ksps; esp-sdr-zephyr (driver esdr) for a XIAO ESP32S3 or an ESP32-C6 at up to 250 ksps. The gain is a PHY table index, not dB, and the "
           "board takes one connection at a time. Install the module next to SoapySDR and the board is listed by itself (USB), or add the Ethernet "
           "board by address here.";
}

} // namespace dect2
