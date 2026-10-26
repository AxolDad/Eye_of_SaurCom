# Protocol lab

View > Protocol lab... is a workbench for an unknown digital signal: a sensor, a remote, a pager, a telemetry link that none of the
modes decodes. You give it a recording (or packets you already have) and it works out how the signal is built, one stage at a time,
and shows every stage so you can check it or override it. It never decrypts anything: an encrypted payload simply shows up as
high-entropy bytes.

The stages are in `core/src/protolab` (no dependencies, own test: `tests/protolab/test_protolab.cpp`); the window is
`app/panels/protolab_ui.cpp`.

## Using it

1. **Recording.** Pick an IQ file (complex float32, int16, uint8 as from an RTL-SDR, or int8 as from a HackRF). A file name written by
   the recorder ("..._10Msps.cs8") fills in the format and the rate. Read the offset of the signal from the centre and its width off
   the waterfall of the recording and type them in; the lab mixes the signal to the centre, filters and decimates, so a 10 Msps capture
   of a 25 kHz signal is no problem. Up to 30 million samples are read.
2. **Packets.** If you already have packets (from another tool, from a log), paste them one per line as hex bytes or as bits. The
   stages that work on the signal are skipped; everything from the line coding on runs as usual.
3. **Demo signals.** Five made-up signals with a known structure (listed below) to see what the lab does.
4. Leave the demodulator on *Auto* to try them all, or pick one. Type a baud rate if you know it. *Gap* is the pause that ends a
   packet; raise it when a packet contains long silences (OOK with long runs of zeros).

The result is a list of readings, best first. Click one to see its bytes, fields, the CRC details and the export.

## What each stage does

| Stage | How |
| --- | --- |
| Bursts | The magnitude, smoothed, against a level between the noise floor and the strong part; a pause shorter than *Gap* does not split. |
| Demodulators | OOK/ASK and AM (magnitude), 2FSK/GFSK, 4FSK and FM (frequency discriminator after taking the mean frequency out and a low-pass sized from the deviation), BPSK and QPSK (carrier from the 2nd / 4th power, then a Costas loop; the polarity or quadrant is ambiguous and is resolved by the later stages). PSK is only tried when nothing simpler explained the packets. |
| Baud rate | The transition intervals, measured at several smoothing widths; the largest unit that explains the intervals as whole multiples is taken, then refined by fitting a line through all edges of all bursts. |
| Bit slicer | Run-length clock: each run between transitions is a whole number of symbols; the symbol length is nudged by long clean runs; glitches shorter than 0.4 symbol are absorbed. 4FSK uses three thresholds and Gray bits. Both polarities are tried. |
| Line coding | NRZ, NRZI, Manchester (both conventions, either pair phase, chosen packet by packet), differential Manchester, HDLC (NRZI plus flags plus bit-stuffing removal). Codings that cannot fit (no pairs, no flags) are not tried. |
| Preamble and sync | A repeating pattern at the start (0xAA, 0x55, 0xFF, ...) is the preamble; the bits the packets share after it (up to 32) are the sync word. Without a preamble, a 16-bit word that most packets contain is used. |
| Byte grid and bit order | The byte boundary and the bit order inside a byte (MSB or LSB first) are found together with the CRC; the byte boundary is a guess from the preamble when there is no CRC. |
| CRC search | See below. |
| Whitening | PN9 (x^9+x^5+1, seed 0x1FF, the sequence used by common sub-GHz radios) and PN7 are undone after the sync word and the CRC search is repeated; accepted only with a 16 or 32 bit CRC that holds for every packet. |
| Fields | Per byte column over the packets: constant, counter (a fixed non-zero step per packet), check value (the CRC bytes), few values, varying; Shannon entropy per byte; columns that follow each other (|r| >= 0.9). A selection inspector shows the bytes as u8/i8/u16/u32/float (LE and BE), ASCII and BCD. |
| Compare | Two packets as bits, the differing bits marked. |
| Export | Protocol definition (JSON), packets as bits or hex, and an rtl_433 "flex decoder" line. |

### The CRC search

* **Catalog.** About 50 named CRC models (8, 16 and 32 bit: CRC-8 variants, CCITT-FALSE, XMODEM, KERMIT, X-25, MODBUS, USB, DNP, wM-Bus,
  CRC-32 and others), each also with init and xorout of 0 or all ones, on every start offset, every number of trailing bytes after the CRC,
  both byte orders, both bit orders and the 8 possible byte grids. The check value of every named model ("123456789") is tested.
* **Unknown polynomial.** For packets of equal length the init and xorout cancel in the XOR of two messages, so the polynomial can be
  solved by trying every polynomial against the XOR of the first packet with each of the others (8 and 16 bit; with the four reflect
  combinations), then init and xorout are fitted. This is the difference method of CRC RevEng, written from the published description.
* **Checksums.** Plain sum, XOR and two's-complement sum of the bytes.
* A result needs at least three different packets, and at least 60 % of them must agree. Identical packets count once.

Limits worth knowing:

* With packets that all have the same length, init and xorout cannot be told apart; the lab shows the first plain pair. This is a property
  of CRCs, not of the search. With packets of two or more lengths they are fixed.
* A polynomial divisible by (x + 1) leaves one more degree of freedom between init and xorout, for the same reason.
* An 8-bit CRC with only three or four packets can fit by chance (1 in 256 per packet per model); the tab shows "matches / packets" so
  you can judge. Eight packets are comfortable.
* Because the search tries many readings, a reading with an unnamed (fitted) CRC ranks below one that matches a named catalog model.
* 32-bit CRCs are only found from the catalog (the polynomial is not solved at that size).
* Spread spectrum, frequency hopping, OFDM, and bit streams that are scrambled with a self-synchronising scrambler other than the plain
  NRZI/HDLC are not handled.

## The demo signals

Generated in `core/src/protolab/gen.cpp`: none is a real protocol.

1. 2FSK, 4800 baud, preamble 0xAA..., sync 2D D4, length, 8 data bytes, CRC-16/CCITT-FALSE.
2. OOK, Manchester at 1000 baud, CRC-8 (poly 0x31, init 0).
3. 2FSK 9600 baud, NRZI + HDLC framing with stuffing, CRC-16/X-25.
4. 2FSK 9600 baud, PN9 whitening, CRC-16 (0x8005, init 0xFFFF).
5. 4FSK 4800 symbols per second, CRC-8 (poly 0x31, init 0xFF).

## Tested how

`tests/protolab/test_protolab.cpp` (ctest `protolab`, about 10 s, and `protolab_sweep`, marked slow):

* each stage alone: CRC check values of all catalog models, PN9 sequence, line codings, HDLC stuffing, byte/bit conversions, preamble
  and sync search with packets that start with different amounts of preamble, baud estimate and slicer on square waves with jitter at
  several samples per symbol, BPSK and QPSK demodulation with a carrier offset, field analysis (constant / counter / correlated /
  enum / check value), interpretations, bit diff;
* the CRC search: catalog models with equal and unequal lengths and both byte orders, unnamed 8 and 16 bit polynomials found from the
  packets alone, a reflected 16-bit one, simple checksums, and random data (no CRC may be claimed, also through the whole pipeline);
* the whole pipeline on the five demo signals: the demodulator, line coding, baud rate (within 3 %), CRC model and whitening must be
  the right ones, and the payload bytes come back exactly in the clean case. The sweep repeats this with a carrier offset of -9 kHz and
  14 dB SNR, and adds a transmitter clock error of -3 to +4 %, sample rates from 48 kHz to 1 Msps, an off-centre signal, and one signal
  inside a 2 Msps capture next to a strong carrier.

Not tested: any real recording. All test signals are synthetic, so fading, multipath, drifting clocks beyond a few percent, bursts that
overlap, and radios whose preamble or whitening differ from the ones above are unverified.

## Export formats

* **Protocol JSON** (`"format": "saurcom-protocol-lab"`): name, modulation, symbol rate, levels, inversion, line coding, byte order,
  preamble, sync bits, whitening, length range, the CRC (model, polynomial, init, xorout, reflection, byte order, covered range,
  packets verified) and the fields as runs of bytes with their kind.
* **Bits** (one packet per line) and **hex**. Tools such as Universal Radio Hacker can open protocol text; this has not been tried here.
* **rtl_433 flex decoder line**: only for plain two-level signals (PCM or Manchester). The option names follow the documented flex
  decoder; the line has not been run against rtl_433 and is meant as a starting point.
