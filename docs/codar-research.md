# CODAR / HF ocean radar: what is publicly documented (research note, 2026-10-13)

Sources: sigidwiki.com/wiki/CODAR; CODAR patent "Call-sign implementation optimized for FMCW HF oceanographic radars" (US 10627509,
Justia 20180074192); CODAR patent "Multi-station HF FMCW radar frequency sharing with GPS time modulation multiplexing" (US 6856276);
ITU-R Report M.2321 (guidelines for HF ocean radars, hosted by IOOS; PDF not readable by the tools used); UC Davis BOON sensor page.

Waveform
- Linear FM sweep (FMCW). Sigidwiki calls it ILFM: FMCW chopped into pulses to a 50% duty cycle; bandwidth 50 kHz, listened to in USB.
- SeaSonde: sweep repetition about 0.5 s (about 2 per second; the UC Davis page also says 2 Hz). A 1 Hz rate is NOT confirmed anywhere found.
- Sweep width 25 - 300 kHz is the user's figure (range resolution c/2B: 50 kHz gives 3 km); the patent example is about 50 kHz at 13 MHz.
- Sweep span, repetition, blanking and carrier are digital, every repetition identical; multi-station sites share a frequency by GPS-timed
  slots, each station starting its sweep at its own offset (so overlapping radars on one frequency are separated by time, not by sweep shape).
- Chirp direction (up or down) is not documented in what was found: the receiver measures it and reports it, nothing is assumed.
- Bands (sigidwiki, 2012 chart; varies by ITU region): 4.438-4.488, 13.450-13.550, 42.0-42.5 MHz; examples 4.438, 12.47, 12.92, 13.5, 42.5 MHz.
  User's list of the oceanographic bands: 4.4-4.65, 5.25 (some), 12-14, 24.45-24.65, 41.5-42.5 MHz. Range of the equipment 4.4 - 50 MHz.

Identification
- ITU Res. 612 (rev. WRC-12) wants a call sign in international Morse (A1A), about 15 wpm, 6 characters alphanumeric, at least once per 20 minutes,
  at the end of a data cycle, on the working frequency; a plain SSB receiver is meant to read it.
- The patent: a separate "call-sign mode": data capture (receiver) is off and the RF is modulated (keyed) with the Morse call sign; the sweep
  bandwidth matches the data mode but the sweep repetition differs; the slot is at the end of a sweep period and staggered between stations.
  As of 2016 the patent says no oceanographic radar had started broadcasting call signs, so many stations send none.
- Discord hint (unverified): the ID looks like a wide CW: the whole sweep bandwidth keys on and off together, like many carriers keyed in parallel.
  Eye of ZorCom therefore decodes the keyed energy of the whole sweep width, not one tone. Every decoded ID is labelled unverified (no recording checked).

Consequences for Eye of ZorCom: identification by sweep parameters (centre, width, period) is only a "possible" hint; the call sign is the only real ID.
Receive only; broadcast radar signals, nothing is decrypted.
