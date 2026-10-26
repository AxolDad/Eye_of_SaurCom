# e-Czas (Poland, 225 kHz long wave)

e-Czas ("e-CzasPL") is the official time service of the Polish Central Office of Measures (GUM). It rides on the 225 kHz long-wave carrier of Polish Radio 1
from Solec Kujawski (about 52.9 N, 18.2 E) as a phase keying of the carrier: 50 bit/s, the carrier phase moves between two states 36 degrees apart.
The mode **e-Czas (225 kHz, Poland)** (Weather & Science group) decodes it and compares the received time with the computer clock.

## What the signal is (GUM description v1.0)

- Time slots of 3 s, 20 of them in a minute frame. A frame is 12 bytes, 96 bits, 1.92 s, and starts on a second boundary.
- Bytes: sync `55 55`, `60` (time packet), then 40 bits: marker `101`, 30 bits counting periods of 3 s since 2000-01-01 (the stamp is the start of the slot, UTC), TZ0 TZ1
  (local offset 0 to +3 h, TZ0 the low bit), LS (leap second announced), LSS (sign of the leap second), TZC (the local time changes next Sunday at 01:00 UTC), SK0 SK1
  (state of the transmitter), 3 Reed-Solomon bytes, and a CRC-8 (polynomial 0x07) over the five data bytes after the scrambling with `0A 47 55 4D 2B` ("\nGUM+").
- Other slots carry other packets (they are shown in the frame log as "other slots"). Real recordings show time packets in many slots of the minute, not only in slot 0.

Eye of ZorCom checks the sync word, the header, the marker and the CRC-8. A frame with a wrong check byte is dropped; the Reed-Solomon bytes are not used yet.
Once two packets agree with each other and with the elapsed signal time the time counts as confirmed, and a later packet that disagrees is kept apart until a second one confirms it
(one damaged frame in 256 passes an 8-bit check by chance).

## Receiving it

225 kHz is below the range of most SDRs (a HackRF or an RTL-SDR start at 24 MHz or 1 MHz). You need one of:

- a radio that tunes below 1 MHz: SDRplay RSP (from 1 kHz), Airspy HF+ (from 9 kHz), RX888 / other direct-sampling radios, an RTL-SDR v3 in direct sampling mode (Q branch, set under the radio's settings), or a Perseus-class receiver;
- an upconverter (for instance 125 MHz or 100 MHz LO) in front of an HF-capable radio: set it under the radio's settings (converter), and Eye of ZorCom tunes the radio to the converted frequency;
- a recording (below).

Antenna: long wave is mostly noise-limited by the local interference, not by the receiver. A long wire (10 m and more) or an active ferrite loop away from computers and switching supplies;
indoors, a ferrite rod antenna turned for the best level. The transmitter is a ground-wave station, so most of Poland and its neighbours get a stable signal day and night.
The status bar shows Carrier (the carrier is found), Sync (frames are cut) and Time (a packet has been decoded).

Expected signal: the SNR Eye of ZorCom reports is the carrier to the noise in 100 Hz. Above about 25 dB nearly every time packet decodes; the recordings sent with the feature request had 18 to 19 dB,
where about one time packet in four passes the check byte. Below 17 dB expect nothing. The carrier frequency can be off by up to 180 Hz (radio and crystal errors): it is found and followed.

Tune to **225 kHz** (the default). The radio is tuned 12 kHz above, away from its DC spike; the receiver mixes it out.

## The screens

- **Time**: the received time in large type (green when two packets agree), the Polish local time and the UTC offset from the packet, summer-time change and leap second announcements, the state of the transmitter, the computer clock against the received time (ms, averaged over the last packets) with its history, SNR and level history.
- **Signal**: the carrier phase as a plot (two clusters 36 degrees apart on a clean signal) and the signal the bit slicer sees (the phase change over each 20 ms bit: plus or minus 1 is a bit that changes the keying state).
- **Frames**: every frame behind a sync word with its bytes and what it is, and the counters: syncs, frames, time packets, CRC failures, other slots, false syncs.
- **Receiver**: the carrier numbers, and "Audio, not IQ" for BFO recordings.

The computer clock offset assumes that the stamp is the time at the start of the frame. The GUM description says the receiver enters the time 0.5 s after the start of the frame without saying which instant
the number itself stands for, and no signal with a known clock has been compared yet: treat the offset as an indication until someone checks it against GPS or another time source. The radio's buffering shows in it too (a few ms to tens of ms).

## Recordings

- An IQ file (`.cs8`, `.cu8`, `.cf32`, SigMF, a stereo WAV with I and Q) with the carrier on the centre: open it as usual. A file holds the channel as recorded, no 12 kHz offset is applied.
- A recording of a receiver's audio (SSB or CW tuned a little off 225 kHz, the carrier is then a tone; 1000 Hz when the dial was 224 kHz upper sideband): save it as a stereo WAV with the audio on the left and silence on the right, open it and tick **Audio, not IQ** with the tone in the Receiver tab.
  A mono WAV can be read with the command line tool: `eczastool rx file.wav --tone 1000`. `--offline` runs the first, whole-file chain for comparison.
- `eczastool gen out.cf32 --snr 25` writes the test signal.

The test signal (the built-in simulator) carries the computer's time, with a time packet every 12 s; its clock offset is near zero (the simulator's own buffering shows in it).

## Tests

`tests/eczas`: the frame layer (every field, the calendar edges, the largest counter value, single-bit errors), the receiver on the generated signal across carrier offsets from -150 to +140 Hz, noise down to 17 dB, level, fading,
input rates from an 8 kHz audio file to 2 Msps IQ, sample clock errors of 60 ppm, noise alone and a bare carrier (no packets), the engine path with the simulator and with a looped IQ file,
and the sample recordings (`scratchpad/eczas_recordings/`, not in the repository).

## Credits

The frame layout, the CRC and the first sync search and bit slicer come from [sp5wwp/e-Czas](https://github.com/sp5wwp/e-Czas) (GPL-3.0, like Eye of ZorCom); see THIRD_PARTY.md.
The streaming receiver, the generator and the screens are new.
