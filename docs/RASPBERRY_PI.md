# Eye of ZorCom on a Raspberry Pi

**Status: untested on real hardware.** Eye of ZorCom ships Linux arm64 builds that pass the automated tests on a GitHub arm64 runner, but nobody on the team owns a Raspberry Pi. Everything below is expected to work, not known to work. If you try it, please report what happened (issue tracker).

## Which Pi

- Raspberry Pi 5 and Pi 4 (4 GB or more) on **64-bit** Raspberry Pi OS (Bookworm or Trixie).
- Pi 3, Zero 2 W and 32-bit Pi OS are not built (no armhf package).

## Install

1. Download `zorcom_<version>_arm64.deb` from the releases page and run `sudo apt install ./zorcom_<version>_arm64.deb`. The package installs the udev rules and keeps the kernel's TV driver off RTL-SDR dongles.
2. No apt: unpack `zorcom-<version>-linux-aarch64-portable.tar.gz`, run `sudo ./install-udev-rules.sh` once, then start `zorcom`.
3. Replug the radio. If it is still not found over an SSH login, add yourself to the radio group: `sudo usermod -aG plugdev $USER`, then log in again.

The rules cover RTL-SDR (RTL2832U, including RTL-SDR Blog and Nooelec), Airspy (R2, Mini, HF+), HackRF (One, Pro) and several others (see `packaging/linux/60-zorcom-sdr.rules`).

## Headless use (roof or attic receiver)

Run `zorcom-server --lan` on the Pi and open the printed address in a browser on another device. See [headless-server.md](headless-server.md). To start it at boot, use `packaging/linux/zorcom-server.service` as a systemd unit. This is the recommended way to use a Pi: no desktop, less CPU, the spectrum and audio come to your phone or laptop.

## What should fit

| Pi 5 | Pi 4 |
| --- | --- |
| Narrow modes (FM, AM, airband, ADS-B, APRS, AIS, pagers, FT8/WSPR, LoRa, DMR, most data modes): expected fine. | Narrow modes (up to a few hundred kHz of bandwidth): expected fine. |
| DVB-T/T2, DTMB, ATSC, DVB-S2: may run at low rates, unverified. | DVB-T2, DTMB, ATSC and other wide TV modes: **not expected** to keep up in real time. |

## Tips

- Use the lowest sample rate the mode needs; the Pi spends most of its CPU on the spectrum and the DSP.
- Close the GUI when the receiver runs unattended (headless server).
- Power the radio from a good supply; a powered USB hub helps HackRF and Airspy on a Pi 4.
- Use a fast SD card or an SSD for recordings, and a heatsink or fan: sustained decoding throttles an uncooled Pi.
- If audio is silent on the desktop, pick the output in the Pi OS sound menu (PipeWire/ALSA).
- If the window is black or slow, the GUI needs OpenGL: run it on the desktop session (not over plain SSH), or use the headless server.
