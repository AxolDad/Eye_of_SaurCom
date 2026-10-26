# DXpatrol SDR

The DXpatrol SDR (MK4 and older units, https://dxpatrol.pt/produto/dxpatrol-sdr/) is a USB receiver, 100 kHz to 2 GHz, built on the RTL2832U (the RTL-SDR family). Eye of SaurCom drives it through the RTL-SDR driver.

## What Eye of SaurCom does

- **Detection.** A radio whose USB manufacturer or product string contains "DXpatrol" (any case) is listed as `DXpatrol SDR (RTL2832U) <serial>`. If yours shows up under a generic RTL name, open its radio settings and tick **Treat as DXpatrol SDR**; it takes effect on the next start.
- **HF input.** The shortwave antenna input is sampled directly by the RTL2832U (Q branch). With **HF input (direct sampling)** on Auto (the default), Eye of SaurCom uses it below 28 MHz and the tuner above. On forces the HF input below 28 MHz, Off uses the tuner only (from 24 MHz). It is not an RTL-SDR Blog V4: there is no upconverter, so the V4 handling is not used.
- **Coverage.** Direct sampling 0.1 to 28 MHz, tuner about 24 to 1766 MHz (R820T/R828D limit; the product page says 2 GHz, a tuner cannot reach 1766 to 2000 MHz so such frequencies are passed to the driver with a warning). 24 to 28 MHz is covered by both.
- **Sample rate** up to 3.2 Msps (0.9 to 3.2, and 0.225 to 0.3 Msps). Gain uses the tuner's steps as on other RTL-SDR radios.

## Not verified

Developed without the hardware. The USB strings ("DXpatrol"), which input the shortwave jack uses (Q branch assumed) and the range above 1766 MHz are guesses from the product page and the RTL-SDR family. Please report the lines starting with `RTL-SDR:` from the log, and the output of `lsusb -v` (or the Windows device manager) of your unit.

The librtlsdr library must be installed as for any RTL-SDR (see the RTL-SDR section of DEVICES.md); on Windows use Zadig to install the WinUSB driver.
