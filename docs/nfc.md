# NFC / RFID sniffer (13.56 MHz)

The exchange between a contactless reader and a card, heard from an antenna beside them. It is there to check what your own cards
and readers give away: which UID a card hands out, what type it is, what the reader asks it, whether anything passes in the clear that
should not. Receive only: Eye of SaurCom makes no field, sends nothing and emulates no card. What a reader and a card encrypt between
them (a MIFARE Classic authentication and everything after it, an ISO 14443-4 secure channel) is marked as such and not decoded.

## What it shows

- **Frames**, as they pass: time, who sent it (reader or card), technology and bit rate, the bytes as on the air, and what the frame
  is: REQA / WUPA, ATQA, the SELECT cascade levels with the UID and BCC, the SAK, HLTA, RATS and ATS, PPS, the MIFARE Classic
  commands (AUTH, READ, WRITE, INCREMENT ...), the Ultralight / NTAG ones (GET_VERSION, FAST_READ, PWD_AUTH, READ_SIG ...), ISO-DEP
  I / R / S blocks, the NFC-B REQB / ATQB / ATTRIB, the NFC-V inventory and block commands, FeliCa polling. Parity and CRC errors
  are flagged; an encrypted frame is marked and shown only by length.
- **Cards**: every card a reader selected, keyed by UID (4, 7 or 10 bytes for NFC-A; the PUPI, IDm or ISO 15693 UID for the
  others), with its ATQA and SAK, the type the public ATQA / SAK tables give (MIFARE Classic 1K / 4K / Mini, Ultralight / NTAG,
  DESFire, Plus, an ISO 14443-4 card ...: a guess, several products share the same values), the chip maker from the UID's first
  byte (ISO/IEC 7816-6), whether it speaks ISO 14443-4 (APDUs), how often it was selected and when.
- **Field and settings**: whether a reader field is present, the input level, frame counts per technology, which technologies the
  decoder looks for (NFC-A, -B, -F, -V) and the field threshold.

## Setting up

1. Tune to **13.560 MHz** (the mode's default) with the mode's sample rate, 10 Msps. The radio has to reach 13.56 MHz: a HackRF,
   Airspy HF+ / R2, SDRplay, LimeSDR, BladeRF or USRP does; an RTL-SDR only in direct-sampling mode, where 2.4 Msps is the most it
   gives and that is marginal (2.5 Msps is the least the decoder accepts: both subcarrier sidebands at ±847.5 kHz have to fit).
2. Put the antenna near the reader: a small loop, or a few turns of wire on the radio's input, within a few centimetres. The
   reader's field is strong; turn the gain down if the level reads near 0 dBFS.
3. Start the receiver and hold a card to the reader. The field shows as present, the frames appear, and the card is listed.

The field's carrier sits on the radio's centre on purpose, and the engine leaves the DC spike alone for this mode: the carrier *is*
the signal. The reader's commands are 100 % amplitude pauses in it (modified Miller code), the card's answers are a small load
modulation on an 847.5 kHz subcarrier. The decoder works on the magnitude of the samples, so a frequency error in the radio does
not matter.

## What it does not do

- It does not read a card by itself: there has to be a reader. Nothing is transmitted.
- It does not decrypt anything, does not guess keys and does not emulate a card. Encrypted exchanges are counted and marked.
- The card type is read from public tables and is a guess where products share ATQA and SAK values.

## Where it comes from

Demodulation and frame decoding are **[nfc-laboratory](https://github.com/josevcm/nfc-laboratory)**'s (Jose Vicente Campos
Martinez, GPL-3.0-or-later), vendored in `third_party/nfc-laboratory` as its decoder libraries at commit e9b054a (2026-10-23);
`third_party/nfc-laboratory/README-SaurCom.md` lists what was taken and the two files Eye of SaurCom replaced (the logger and the debug recorder
stub). Eye of SaurCom feeds it the magnitude of each sample and reads its frames back. The frame names, the anticollision tracking
to the UID, the ATQA / SAK and manufacturer tables and the screens are Eye of SaurCom's (`core/include/dect2/nfc/nfc_proto.h`).

Tests: `nfc_proto` (CRC_A and CRC_B vectors of ISO/IEC 14443-3 Annex B, the frame names, the anticollision tracker on a 4-byte and a
7-byte UID, the card table), `nfc_rx` (the generator's reader-and-card exchange through nfc-laboratory's decoder at 10 and 2.5 Msps, the
7-byte card, noise alone, the settings reaching the decoder), `nfc_engine` (through the engine with the test signal, and the mode's
registrations). The test signal (`core/src/nfc/nfc_gen.cpp`) is written from ISO/IEC 14443-2 and -3; **no real reader or card has
been decoded with this build yet**, so the field threshold and the modulation depths a real set-up gives are unverified.
