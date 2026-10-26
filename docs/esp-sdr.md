# ESP32 boards as radios (ESP-SDR)

The Wi-Fi radio of an ESP32 can be read as raw I/Q samples, and three open-source projects turn that into a SoftwareDefined Radio
that SoapySDR applications see as a receiver. Eye of SaurCom uses them through SoapySDR like any other radio; what it adds is that it
recognises the boards, names them, shows their specified range and sample rates before they have been started (the drivers say
nothing until then), and lets the Ethernet board be added by address. Everything here is written from the projects' own
documentation and has not been tried on a board.

| Project | Board | Driver and arguments | Sample rates | Frequency | Transmit |
|---|---|---|---|---|---|
| [SoapyESPSDR](https://github.com/ESPARGOS/SoapyESPSDR) (ESPARGOS) with the [ESP-SDR](https://github.com/ESPARGOS/esp-sdr) streaming firmware | ESP32-S31 "Function-CoreBoard", USB or Ethernet | `driver=espsdr,usb=1` (`usb_serial=` picks one), or `driver=espsdr,host=<ip>` (`interface=`, `http_port=80`) | 4, 8, 16, 20 Msps; 40 Msps over Ethernet | 2150 - 2800 MHz in 1 MHz steps | no |
| [SoapyESPSDR](https://github.com/mkbodanu4/SoapyESPSDR) (serial) | ESP32-S3-WROOM-1 over its USB Serial/JTAG port | `driver=espsdr,device=/dev/ttyACM0` (`mode=iqs`, or `mode=burst` for snapshots) | 15.625, 31.25, 62.5, 125, 250, 312.5, 625 ksps | advertised 100 - 6000 MHz, useful around 2.4 GHz | no |
| [esp-sdr-zephyr](https://github.com/cyber-murmel/esp-sdr-zephyr) (GPL-3.0-or-later) | XIAO ESP32S3 on USB; ESP32-C6 on a serial link | `driver=esdr` (`serial=`), or `driver=esdr,port=/dev/...` | 15.625, 31.25, 62.5, 125, 250 ksps | not stated; the S3's PLL locks from about 1.8 to 2.8 GHz | yes, half duplex (TX power step 0 - 17) |

In all three the gain is an index into the chip's PHY table, not decibels (0 - 82 where the documentation says), and hardware AGC is
available. Samples are 8 bits. A board takes one connection at a time.

## Setting up

1. Flash the firmware the project names for your board (the ESPARGOS one is the *streaming* firmware "for SoapyESPSDR", not the
   ESP-WebSDR one) and build its SoapySDR module. Check with `SoapySDRUtil --find="driver=espsdr"` (or `driver=esdr`).
2. Put the module where this computer's SoapySDR finds it (`SoapySDRUtil --info` lists the module path; on Windows Eye of SaurCom
   loads the modules in its own `lib\SoapySDR\modules0.8` folder).
3. A board on USB or a serial port is then in Eye of SaurCom's radio list by itself, as **ESP-SDR ESP32-S31 USB**, **ESP-SDR ESP32-S3
   on /dev/ttyACM0** or **ESP-SDR Zephyr ESP32**.
4. The ESPARGOS board on Ethernet is added from the source list: **Add network radio…**, type **ESP-SDR (ESP32 on Ethernet)**, the
   address its DHCP server gave it, and the port of its HTTP control server (80 unless the firmware was told otherwise). Connect
   checks that the port answers; the radio is then listed as **ESP-SDR ESP32-S31 \<address\> (network)**.

The frequency range and the rates shown for an unstarted board are the documented ones; once the board has been started the driver's
own figures replace them. Eye of SaurCom never limits the frequency you type: outside the range it still asks the driver.

## What to expect

- 2.4 GHz only, in practice: Wi-Fi, Bluetooth LE, Zigbee and the other 2.4 GHz modes. The Bluetooth LE and Wi-Fi modes need
  the wide rates of the ESPARGOS board; the serial boards' 250 - 625 ksps suit the narrow modes and the spectrum analyser.
- The receivers are not characterised wideband SDRs. esp-sdr-zephyr's maintainer warns that its firmware may permanently change a
  board's analogue radio characteristics, and advises coax with a 30 - 60 dB attenuator for any test with its transmitter.
- Transmitting (esp-sdr-zephyr only) goes through the same Transmit window as a HackRF or a PlutoSDR; the gain number is the chip's
  power step 0 - 17, and the driver stops receiving while it transmits.

## Where it lives

`core/src/source/esp_sdr.h` holds the facts above as plain functions (`espSdrDescribe` recognises the arguments and fills the
device entry, `espSdrEthernetArgs` builds the Ethernet board's), used by the SoapySDR listing (`source_soapy.cpp`), the network
radio types (`netradio.cpp`, type `EspSdr`, settings key `espsdr`) and the Add network radio window. `tests/source/test_esp_sdr.cpp`
checks them.
