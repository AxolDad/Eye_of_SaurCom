# Wireless M-Bus (EN 13757-4)

The mode **Wireless M-Bus (meters)** (IoT & sensors group) lists the utility meters (water, heat, gas, electricity, heat cost allocators) that send their
readings by radio in the 868 MHz and 169 MHz bands, and decodes the readings of the frames that are not encrypted. It is receive only and written
clean-room from the public descriptions of EN 13757-3 and EN 13757-4 (no code of wmbusmeters, rtl-wmbus or other decoders).

## What is received

| Mode | Where | Chips | Coding |
|---|---|---|---|
| S1 | 868.3 MHz | 32.768 kchip/s, 2-FSK | Manchester, sync `7696` |
| T1 (meter to other) | 868.95 MHz | 100 kchip/s, GFSK | 3-out-of-6, sync `543D` |
| C1 format A and B | 868.95 MHz | 100 kchip/s, GFSK | NRZ, sync `543D` and the marker `CD` (A) or `3D` (B) |
| R2 | 868.03, 868.09, 868.15 MHz | 4.8 kchip/s | Manchester, sync `7696` |
| N1a - f | 169.40625 - 169.46875 MHz, 12.5 kHz apart | 4.8 kchip/s, GFSK | NRZ, sync `F68D` |

Every channel that lies inside the capture is mixed to baseband, filtered, FM demodulated and run through a chip clock recovery and a sync search (the 32 chips of
preamble before the sync word must alternate). Tune to **868.625 MHz** with 2 Msps (1 Msps and up works) for S1, T1, C1 and R2 together, or to **169.4375 MHz** for N1.
A file is read as an 868.625 MHz capture.

Frames: format A (blocks of 16 data bytes, each with a CRC; the first block is 10 bytes) and format B (a block of up to 126 bytes, and the rest, each with a CRC). The CRC is the
EN 13757 CRC-16 (polynomial 0x3D65, inverted result). A frame with a failed CRC is counted and logged, never listed as a meter.

## What is shown

- Meters: manufacturer (the three letters), ID (8 digits), version, device type, mode, how often and when heard, level.
- Readings of unencrypted frames (CI 7A, 72, 78 and the extended link layer): the DIF / VIF records with quantity, value, unit, storage number, tariff, subunit and function
  (volume, energy, power, flow, temperatures, dates, operating time, error flags, fabrication number and more; unknown VIFs are shown by their code).
- Frames: every frame with its mode, result and the bytes. Signal: the channels with their level and the demodulated frequency of the last frame.

## Encrypted meters

A frame whose configuration word has an encryption mode other than 0 (or an extended link layer frame with encryption) is listed as **encrypted payload**. The link layer
fields (manufacturer, ID, type) are in the clear and are shown; the payload is never decoded and Eye of SaurCom has no key input.

## Not verified

No real meter was available. The rates, sync words and Manchester polarity of S1, R2 and N1 are from the standard as remembered and checked only against the
Eye of SaurCom test signal (which is written from the same description); the R2 and N1 details in particular need a real capture. Real captures of T1 and C1
are the most useful check. Mode T2 (other to meter, 32.768 kchip/s) and the 433 MHz mode are not received.

## Test signal

Six simulated meters (water on S1, heat and an encrypted electricity meter on T1, gas on C1 format A, cold water with a two-block frame on C1 format B, a heat cost allocator on R2)
send in turn; the 169 MHz plan adds a water meter on N1. `tests/wmbus` checks the CRC, the coding, both frame formats, the records, every mode on the generated signal with
transmitter frequency errors of -12 to +9 kHz, noise, sample rates from 1.2 to 2.56 Msps, and that noise alone gives nothing.
