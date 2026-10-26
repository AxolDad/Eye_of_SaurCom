# KiwiSDR (network HF receiver)

A KiwiSDR is a software defined receiver that covers 0 to 30 MHz and is reached over the internet or a local network. Many owners make theirs public
(list: <https://kiwisdr.com/public/>). Eye of SaurCom uses one as a radio: **Network radios > Type KiwiSDR**, the receiver's address and port (default 8073),
the password if the owner set one, **Connect**. **Find KiwiSDR...** downloads the public list when you click it (never at startup) and adds a receiver
with one click. The password is kept in Eye of SaurCom's settings (the same file as the rest of the list) and is not shown in a name, a log line or a command.

## What you get

The Kiwi's "IQ" mode: 12 kHz of spectrum (the passband is 3, 5 or 5.9 kHz either side of the tuned frequency) around the frequency you set, with the
Kiwi's own AGC or a manual gain (the gain slider, 0 to 120 dB in the Kiwi's scale). The S-meter and an ADC overload mark show in the Network radios
window while it streams. The receiver also sends a GPS time stamp in every IQ frame (when the Kiwi has a GPS lock); Eye of SaurCom reads it
(`kiwiStatus()`, `kiwiGpsToUnix()`) but no clock in the app uses it yet.

Eye of SaurCom's receivers want at least 250 kHz of sample rate, so the 12 kHz stream is interpolated by 21 (252 kHz) inside the source (36-tap-per-phase
windowed sinc, image rejection more than 40 dB). The modes that put their channel off the radio's centre (HF digital, CW, PSK, JS8, ...) get the
Kiwi tuned to the dial and the offset applied by a mixer, so the receivers see the usual layout. The general receiver's 250 kHz offset needs more room:
that mode runs the stream at 84 times (1.008 MHz). So the sample rate is not a setting: the Kiwi runs at one rate.

## Which modes can work

By the code (not run against a live Kiwi): modes with a minimum rate up to 252 kHz and a channel of a few kHz: HF digital (FT8, FT4, WSPR, JT65/JT9, Q65,
FST4, MSK144 off the HF bands, RTTY, SSTV, PSK, Olivia and the other keyboard modes, FreeDV, HamDRM), CW, PSK, JS8, HFDL, ALE, the general receiver (AM, SSB, CW, NFM
in a window of 12 kHz) and the AM broadcast entry. A Kiwi's 12 kHz window cannot hold: HF beacons across a band (they need megahertz), wide modes, and
anything above 30 MHz. DRM: the Kiwi has its own DRM extension; Eye of SaurCom does not use it and its DRM mode wants a wider capture, so DRM from a Kiwi is not
offered. The spectrum and waterfall in Eye of SaurCom show the 12 kHz around the tuned frequency (the 0 to 30 MHz waterfall of the Kiwi is not shown; its frame
layout is parsed and tested, nothing connects to it).

## Public receivers are somebody's hobby

- One connection per receiver. Eye of SaurCom opens one stream and never a second one to the same receiver. The test when you press Connect opens the connection and
  gives the slot back at once.
- A busy receiver ("too busy"), a refusal (wrong password, one connection per address), a receiver that is down, and the time limit many public Kiwis put on
  a session (often about 30 minutes) are shown in plain words. A connection that fails the first time is not repeated.
- After a loss Eye of SaurCom waits before it connects again: 5 s, then 10, 20 ... up to 2 minutes, and at least a minute after "busy", a time limit, a kick or a
  receiver that is down. The wait is reset after a connection that held for 30 seconds.
- A keepalive goes every second (what the Kiwi's own page does), and the user name `Eye of SaurCom` is sent so the owner sees what is listening.
- Leave when you are done. If you listen a lot to one receiver, think of the owner: many accept a thank-you note.

## What is verified and what is assumed

Read in kiwiclient's sources (protocol facts only; no code was copied): the message tags, the MSG layout, the SND frame layout (flags, sequence, S-meter,
GPS block, big-endian 16-bit IQ), the commands (`SET auth`, `SET mod=... low_cut high_cut freq`, `SET agc`, `SET compression=0`, `SET AR OK`, `SET keepalive`,
`SET ident_user`), the error names (`too_busy`, `badp` 1 to 7, `down`, `redirect`), the W/F frame layout, and the sample_rate message.

Assumed (marked in `core/src/source/kiwisdr_proto.h`): the path prefix `/kiwi/<time>/SND`; `p=#` for no password; that in IQ mode the tuned frequency is the
centre of the 12 kHz; I first, Q second (the setting "Swap I and Q" mirrors it if a Kiwi disagrees); the names of the time-limit and kick messages
(`inactivity_timeout`, `kick_by_admin`); the list's field names and address (below). Eye of SaurCom was **not tried on a real KiwiSDR**: only against a fake server in
`tests/source/test_kiwisdr.cpp` (handshake, commands, keepalive, IQ with sequence gaps, short / odd / compressed / oversize frames, busy, bad password,
down, time limit, reconnect pauses, fuzzed parsers, the list parser, the interpolator).

## The public list

The Find KiwiSDR window reads a JSON array of receivers (fields name, url, loc, antenna, sdr_hw, gps, users, users_max, snr, bands, offline), also when it
sits in a JavaScript file. The default address is the file the public map uses, `http://rx.linkfanel.net/kiwisdr_com.js`; kiwisdr.com itself could not be
reached when this was written, so the format is from memory of that file, not checked. If the listing moves or changes, the window's parser skips what it
cannot read and shows an error instead of crashing; the address is in `app/ui/toolbar_rig.cpp` (`kKiwiListUrl`).

## Licences

kiwiclient (jks-prv): the repository page shows no licence file; only protocol facts were taken. KiwiSDR (jks-prv): a mix of GPL v2.1/3 and MIT per directory and
file (its `_LICENSE` files). Eye of SaurCom contains no code of either; both are credited in THIRD_PARTY.md and the in-app Credits.
