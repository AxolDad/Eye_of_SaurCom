// ESP32 boards as radios: three SoapySDR drivers turn the Wi-Fi radio of an ESP32 into a receiver (the chips' raw I/Q capture), and
// each one tells Eye of ZorCom nothing about itself until it has been opened once. These are the facts their own READMEs give, so the
// radio list can name the board and show its range and rates before that. Written from the documents, not tested against a board.
//
//   driver=espsdr, usb=1 / host=<ip>   ESPARGOS SoapyESPSDR (github.com/ESPARGOS/SoapyESPSDR) for the ESP32-S31 "Function-CoreBoard"
//                                       with its streaming firmware, over USB or Ethernet: 4, 8, 16, 20 Msps (40 over Ethernet),
//                                       2150-2800 MHz in 1 MHz steps, 8-bit samples, the gain a PHY table index. Receive only. The
//                                       Ethernet board is addressed by host= (its DHCP address), optional interface= and http_port=
//                                       (80) for its control port; one connection at a time.
//   driver=espsdr, device=/dev/ttyACM0  the serial SoapyESPSDR (github.com/mkbodanu4/SoapyESPSDR) for an ESP32-S3 over its USB
//                                       Serial/JTAG port: 15.625 to 625 ksps continuous (mode=iqs), 100-6000 MHz as the firmware
//                                       advertises it, useful around 2.4 GHz; the gain an index 0-82. Receive only.
//   driver=esdr, serial= / port=/dev/   esp-sdr-zephyr (github.com/cyber-murmel/esp-sdr-zephyr, GPL-3.0-or-later) on a XIAO ESP32S3
//                                       (USB) or an ESP32-C6 over a serial link: 15.625 to 250 ksps, receive and transmit (half
//                                       duplex), the RX gain an index 0-82, the TX power a step 0-17. Its documentation states no RF
//                                       tuning range; the S3's PLL locks from about 1.8 to 2.8 GHz.
#pragma once
#include "dect2/source/source.h"
#include <string>

namespace dect2 {

// Fills d from the SoapySDR arguments of one of these boards: a name when the driver gave no label, the specified frequency range and
// the sample rates it offers (left alone when d already has them: the ranges a started radio reported win), the transmit facts and
// whether it is on the network. false when args are not one of these drivers (nothing changes).
bool espSdrDescribe(const std::string& soapyArgs, DeviceInfo& d);

// The arguments of an ESPARGOS board on Ethernet: "driver=espsdr,host=<host>", with ",http_port=<port>" when the port is not 80
std::string espSdrEthernetArgs(const std::string& host, int port);

// One line on how to connect such a board, for the radio list and the network-radio window (the driver's own words)
const char* espSdrHelp();

} // namespace dect2
