// ESP32 boards as radios (esp_sdr.h): each of the three drivers is recognised from its SoapySDR arguments and given the name, the
// ranges and the transmit facts its documentation states; the ranges a started radio reported are left alone; other drivers are not
// touched; the Ethernet board's arguments and its network-radio entry.
#include "../../core/src/source/esp_sdr.h"
#include "dect2/net/netradio.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static bool hasRate(const DeviceInfo& d, double r) {
    for (const auto& p : d.rateRanges) if (p.first <= r && r <= p.second) return true;
    return false;
}

int main() {
    // ESPARGOS SoapyESPSDR over USB
    {
        DeviceInfo d;
        CHECK(espSdrDescribe("driver=espsdr,usb=1,usb_serial=A1B2", d), "USB board not recognised");
        CHECK(d.name == "ESP-SDR ESP32-S31 USB A1B2", "name '%s'", d.name.c_str());
        CHECK(!d.network, "USB board marked as network");
        CHECK(d.minFreqHz == 2150e6 && d.maxFreqHz == 2800e6, "range %.0f-%.0f", d.minFreqHz, d.maxFreqHz);
        CHECK(hasRate(d, 4e6) && hasRate(d, 20e6) && !hasRate(d, 40e6) && !hasRate(d, 10e6), "USB rates (%zu ranges)", d.rateRanges.size());
        CHECK(d.minRateHz == 4e6 && d.maxRateHz == 20e6, "rate span %.0f-%.0f", d.minRateHz, d.maxRateHz);
        CHECK(!d.canTransmit, "receive-only board can transmit");
    }
    // ESPARGOS SoapyESPSDR over Ethernet: 40 Msps too, a network radio
    {
        DeviceInfo d;
        CHECK(espSdrDescribe("driver=espsdr,host=192.168.1.100,interface=eth0", d), "Ethernet board not recognised");
        CHECK(d.name == "ESP-SDR ESP32-S31 192.168.1.100 (network)" && d.network && d.isNetwork(), "name '%s' network %d", d.name.c_str(), d.network);
        CHECK(hasRate(d, 40e6) && d.maxRateHz == 40e6, "Ethernet rates");
        CHECK(d.minFreqHz == 2150e6 && d.maxFreqHz == 2800e6, "range %.0f-%.0f", d.minFreqHz, d.maxFreqHz);
    }
    // the serial SoapyESPSDR for an ESP32-S3
    {
        DeviceInfo d;
        CHECK(espSdrDescribe("driver=espsdr,device=/dev/ttyACM0,mode=iqs", d), "serial board not recognised");
        CHECK(d.name == "ESP-SDR ESP32-S3 on /dev/ttyACM0", "name '%s'", d.name.c_str());
        CHECK(hasRate(d, 15625) && hasRate(d, 250000) && hasRate(d, 625000) && !hasRate(d, 4e6), "serial rates");
        CHECK(d.minFreqHz == 100e6 && d.maxFreqHz == 6000e6, "range %.0f-%.0f", d.minFreqHz, d.maxFreqHz);
        CHECK(d.gainMinDb == 0 && d.gainMaxDb == 82, "gain index %.0f-%.0f", d.gainMinDb, d.gainMaxDb);
        CHECK(!d.canTransmit, "receive-only board can transmit");
    }
    // esp-sdr-zephyr: USB by serial, or an ESP32-C6 on a serial link; receive and transmit
    {
        DeviceInfo d;
        CHECK(espSdrDescribe("driver=esdr,serial=0123", d), "esdr not recognised");
        CHECK(d.name == "ESP-SDR Zephyr ESP32 0123", "name '%s'", d.name.c_str());
        CHECK(hasRate(d, 15625) && hasRate(d, 250000) && !hasRate(d, 625000), "esdr rates");
        CHECK(d.canTransmit && d.txChannels == 1 && d.txGainNote.find("0 (weakest) to 17") != std::string::npos, "tx %d note '%s'", d.canTransmit, d.txGainNote.c_str());
        CHECK(d.minFreqHz == 0 && d.maxFreqHz == 0, "esdr states no RF range, got %.0f-%.0f", d.minFreqHz, d.maxFreqHz);
        DeviceInfo e;
        CHECK(espSdrDescribe("driver=esdr,port=/dev/ttyUSB0", e) && e.name == "ESP-SDR Zephyr ESP32 on /dev/ttyUSB0", "serial link name '%s'", e.name.c_str());
    }
    // the driver's own label stays; the ranges a started radio reported stay (only the name is filled)
    {
        DeviceInfo d;
        d.name = "My ESP board";
        d.minRateHz = 4e6; d.maxRateHz = 20e6; d.rateRanges = {{4e6, 20e6}}; d.minFreqHz = 2.2e9; d.maxFreqHz = 2.7e9; d.gainMinDb = 1; d.gainMaxDb = 5;
        d.canTransmit = false;
        CHECK(espSdrDescribe("driver=espsdr,host=10.0.0.9", d), "not recognised with a label");
        CHECK(d.name == "My ESP board", "label replaced: '%s'", d.name.c_str());
        CHECK(d.rateRanges.size() == 1 && d.rateRanges[0].second == 20e6 && d.minFreqHz == 2.2e9 && d.gainMaxDb == 5, "reported ranges overwritten");
        CHECK(d.network, "network flag not set with known ranges");
    }
    // other drivers: untouched
    {
        DeviceInfo d;
        d.name = "x";
        CHECK(!espSdrDescribe("driver=rtlsdr,serial=1", d) && d.name == "x" && d.maxRateHz == 0 && d.rateRanges.empty(), "rtlsdr touched");
        CHECK(!espSdrDescribe("", d), "empty args recognised");
        CHECK(!espSdrDescribe("host=1.2.3.4", d), "no driver recognised");
    }
    // the Ethernet board's arguments
    CHECK(espSdrEthernetArgs("192.168.1.100", 80) == "driver=espsdr,host=192.168.1.100", "args '%s'", espSdrEthernetArgs("192.168.1.100", 80).c_str());
    CHECK(espSdrEthernetArgs("192.168.1.100", 0) == "driver=espsdr,host=192.168.1.100", "args port 0 '%s'", espSdrEthernetArgs("192.168.1.100", 0).c_str());
    CHECK(espSdrEthernetArgs("esp.local", 8080) == "driver=espsdr,host=esp.local,http_port=8080", "args '%s'", espSdrEthernetArgs("esp.local", 8080).c_str());
    CHECK(std::string(espSdrHelp()).find("SoapyESPSDR") != std::string::npos, "help");
    // the network-radio type: its name, port, settings text and device name
    {
        CHECK(std::string(netRadioTypeName(NetRadio::EspSdr)) == "ESP-SDR (ESP32 on Ethernet)", "type name '%s'", netRadioTypeName(NetRadio::EspSdr));
        CHECK(netRadioDefaultPort(NetRadio::EspSdr) == 80, "default port %d", netRadioDefaultPort(NetRadio::EspSdr));
        NetRadio r; r.type = NetRadio::EspSdr; r.host = "192.168.1.100";
        CHECK(netRadioPort(r) == 80, "port %d", netRadioPort(r));
        CHECK(netRadioDeviceName(r) == "ESP-SDR ESP32-S31 192.168.1.100 (network)", "device name '%s'", netRadioDeviceName(r).c_str());
        const std::string txt = netRadiosToString({r});
        CHECK(txt == "espsdr,192.168.1.100,0", "settings text '%s'", txt.c_str());
        const auto back = netRadiosFromString("espsdr,192.168.1.100,0;espsdr,esp.local,8080");
        CHECK(back.size() == 2 && back[0].type == NetRadio::EspSdr && back[0].port == 0 && back[1].port == 8080, "%zu radios back", back.size());
        const auto devs = netRadioDevices(back);
        if (soapySupported()) {
            CHECK(devs.size() == 2, "%zu devices", devs.size());
            if (devs.size() == 2) {
                CHECK(devs[0].kind == DeviceInfo::Soapy && devs[0].board == "espsdr" && devs[0].soapyArgs == "driver=espsdr,host=192.168.1.100", "device 0 '%s' args '%s'", devs[0].name.c_str(), devs[0].soapyArgs.c_str());
                CHECK(devs[0].name == "ESP-SDR ESP32-S31 192.168.1.100 (network)" && devs[0].network, "device 0 name '%s'", devs[0].name.c_str());
                CHECK(devs[0].maxRateHz == 40e6 && devs[0].minFreqHz == 2150e6, "device 0 ranges from the documentation");
                CHECK(devs[1].soapyArgs == "driver=espsdr,host=esp.local,http_port=8080", "device 1 args '%s'", devs[1].soapyArgs.c_str());
            }
        } else {
            CHECK(devs.empty(), "%zu devices without SoapySDR", devs.size());
            std::string err;
            CHECK(!probeNetRadio(r, 100, err) && err.find("SoapySDR") != std::string::npos, "probe without SoapySDR: '%s'", err.c_str());
        }
        // a board that is not there: the probe says so (a closed port on this machine)
        NetRadio none; none.type = NetRadio::EspSdr; none.host = "127.0.0.1"; none.port = 1;
        std::string err;
        CHECK(!probeNetRadio(none, 300, err) && !err.empty(), "probe of a closed port: '%s'", err.c_str());
        printf("  closed port: %s\n", err.c_str());
    }
    return testSummary("esp_sdr");
}
