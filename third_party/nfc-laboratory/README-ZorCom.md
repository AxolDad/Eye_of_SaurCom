# nfc-laboratory in Eye of ZorCom

The NFC / RFID sniffer mode (`core/src/nfc`) demodulates and decodes with the decoder library of
**[nfc-laboratory](https://github.com/josevcm/nfc-laboratory)** by Jose Vicente Campos Martinez, GPL-3.0-or-later
(`LICENSE` here is its licence file). It is the part of that program that turns the magnitude of the 13.56 MHz field into
ISO/IEC 14443-A, 14443-B, 15693 (NFC-V) and 18092 (NFC-F) frames.

Taken from commit `e9b054a6dd1440a16b57e8068165f24c7d96aa93` (2026-10-23, the 3.4.2 line), unchanged, into this layout:

| here | upstream |
|---|---|
| `include/rt/*.h`, `src/rt/Format.cpp` | `src/nfc-lib/lib-rt/rt-lang` (only what the decoder needs: Buffer, ByteBuffer, Variant, Format, Logger's header) |
| `include/hw/*.h`, `src/hw/SignalBuffer.cpp` | `src/nfc-lib/lib-hw/hw-dev` (the sample buffer and the device interface its debug recorder is declared against) |
| `include/lab/data`, `src/lab-data` | `src/nfc-lib/lib-lab/lab-data` (RawFrame, CRC) |
| `include/lab/nfc`, `src/lab-radio` | `src/nfc-lib/lib-lab/lab-radio` (NfcDecoder, NfcTech, the four technologies) |

Two files are Eye of ZorCom's own, in `zorcom/`, standing in for upstream files that were not taken:

- `Logger.cpp` replaces `rt-lang/src/main/cpp/Logger.cpp`, which writes its own log file through pthread and `sys/time.h`. Same
  interface; WARN and ERROR lines go to the sink Eye of ZorCom installs (`nfc_lab_log.h`), the rest is dropped.
- `RecordDevice.cpp` replaces `hw-dev/src/main/cpp/hw/RecordDevice.cpp`, the WAV writer of the decoder's signal-debug recording,
  which Eye of ZorCom never turns on. It keeps the parameters it is given and records nothing.

Eye of ZorCom feeds the decoder the magnitude of its complex samples (`hw::SignalType::SIGNAL_TYPE_RADIO_SAMPLES`, one float per sample),
as nfc-laboratory's own radio task does, and reads the `lab::RawFrame` list back. The naming of frames, the UID assembly and the card
table are Eye of ZorCom's (`core/include/dect2/nfc/nfc_proto.h`).

To update: copy the same files from a newer upstream commit, keep `zorcom/` and this note, and re-run `ctest -R nfc`.
