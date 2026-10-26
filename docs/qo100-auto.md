# QO-100 narrowband: presets, auto decoder, conversation manager

The QO-100 (Es'hail-2) narrowband transponder is 500 kHz wide (downlink 10489.500 - 10490.000 MHz, uplink 2400.000 - 2400.500 MHz). Everything below works on the one capture of the QO-100 mode (2 Msps or more on the narrowband transponder, or the whole downlink at 10 Msps) and needs no extra radio.

## Presets

`data/frequencies/qo100.tsv` lists one **QO-100 NB** entry for every mode that is used on the transponder (CW, FT8, FT4, JS8Call, PSK31, RTTY, WSPR, SSTV, FreeDV, HamDRM, packet / APRS, Olivia, Contestia, MFSK, THOR, DominoEX, Hellschreiber, MT63, SSB, the broadcast and emergency frequencies) and the wideband DATV entries for DVB-S/S2 (the 1.5 Msym/s beacon at 10491.500 MHz and the whole 8 MHz transponder). The frequencies are the **RF downlink dial (USB)**. The plan comes from the AMSAT-DL narrowband band plan (CW 10489.505 - .540, narrow digital modes to 500 Hz .540 - .580, digital modes to 2.7 kHz .580 - .650, SSB .650 - .745 and .755 - .850, beacons at .500, .750 and 10490.000, broadcast .855, emergency .860); the AMSAT-DL plan gives segments, not a frequency per mode, so the per-mode slots are suggestions inside the right segment (JS8Call .548 is the spot JS8Call users use). View > Frequencies (`Ctrl+Shift+F`) tunes an entry: the mode is switched, and the **LNB oscillator chosen on the QO-100 screen** is subtracted, so the radio sits on the first IF (9750 MHz LO: 739.540 MHz for FT8).

Sideband: the transponder does **not** invert (an uplink frequency maps to the downlink 8089.5 MHz higher), so USB stays USB on both links; the notes of every entry give the matching uplink. Only the LNB can mirror the spectrum, when its oscillator is above the downlink (10600 MHz): the QO-100 mode turns that back by itself; other modes need a standard LNB (LO below the downlink) or a converter.

## Auto decoder

QO-100 > **Auto decoder** view. Tick what to decode: SSB conversations, FT8, FT4, JS8, PSK31, CW, SSTV, RTTY.

1. `Qo100Tracker` (`core/src/qo100/qo100_track.cpp`) follows every signal of the narrowband spectrum (about 75 Hz bins, a frame every 0.1 s): frequency, edges, level, on / off, and from the timing a class: beacon, carrier, CW (narrow, level jumping about), SSB (wide, syllabic), FT8 / JS8 style 15 s slots and FT4 7.5 s slots (on-periods of steady level, spaced by whole slots), PSK (narrow, continuous), RTTY (wide pair of tones), SSTV (steady, about 1.3 kHz). A block of FT8 stations is one wide track; its slots tell it apart from voice. A signal the tracker cannot place is passed (on a thread of its own, one at a time) to the signal identifier (`core/src/engine/sig_ident.cpp`), whose opinion is shown in the signal's tooltip and fills the class when nothing better is known.
2. `Qo100AutoDecoder` (`core/src/qo100/qo100_auto.cpp`) gives each ticked decoder its signals. Signals that fit into 3.4 kHz of audio share one **channel**: the ordinary FT8 / FT4 / SSTV / RTTY (HF digital), JS8Call, PSK or CW receiver, fed the one decimated copy of the narrowband channel with its own offset. A channel appears when its signal has been classified and goes away 20 s after its last signal (45 s for slot modes).
3. The **CPU meter** compares the time the channels use with the limit (default 0.7 of a core); above it the weakest channel is dropped (its signals are left alone for 60 s). *channels at most* (default 6) bounds the number of channels.

Results: FT8 / FT4 / JS8 messages go to the decoded-messages list; **Save results** writes CSV, JSON or an **ADIF** log (band 3cm, `PROP_MODE=SAT`, `SAT_NAME=QO-100`). PSK31, CW and RTTY text is listed per signal. SSTV pictures go to the picture gallery (View > Images) as they finish.

## Conversation manager

QO-100 > **Conversations** view. SSB signals are grouped by frequency and time (within 1.2 kHz of a conversation that was heard in the last 90 s) into conversations: start, length, dial, level, drift (Hz/min, from the lower edge), estimated number of stations (voice spurts are clustered by lower edge and level), and, when speech to text is available, a snippet and the callsigns found in it (`qo100CallsFromText`, also understands spoken NATO phonetics). The list updates live; ended conversations stay.

* **Listen** tunes the listening channel to the conversation: upper sideband, 300 - 3000 Hz, dial = the voice's lower edge - 300 Hz, and follows its drift while it lasts. **Squelch** mutes the audio while only noise is there.
* **Auto listen**: *the loudest conversation* or *the newest conversation*; it switches when the current one has gone quiet, or (loudest) another is 6 dB louder for 3 s.
* **Record** writes a conversation to an 8 kHz WAV file in `<data folder>/qo100/` while it lasts.
* Speech to text: `Qo100Receiver::setSpeechToText()` takes a function that gets 8 s chunks of a recorded conversation's audio and returns text. Eye of ZorCom has no speech recognizer of its own yet, so nothing is wired to it; the callsign finder and the list are ready for it.

## Limits

* A signal is found when it stands 8 dB over the floor of the 75 Hz bins; weaker FT8 stations inside a dense block are decoded (the block's channel hears them) but not listed on their own.
* The class of FT8 / FT4 needs two or three slots (30 - 45 s) before a channel starts; the first slots are not decoded. Likewise PSK / CW / SSTV need 6 - 10 s. An SSTV picture that began before its channel did is lost.
* JS8Call and FT8 share the 15 s slot class: both decoders run on such signals when both are ticked.
* Tested on synthetic signals only (`tests/qo100/test_qo100_auto.cpp`); no real QO-100 recording was available.
