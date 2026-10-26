# Passive radar (View > Passive radar)

Detects aircraft from the echoes of a transmitter that already exists (an FM or DVB-T station), with no transmitter of your own. **Status: the processing is tested against a built-in simulator only. It has never been run on a real radio, and Eye of SaurCom makes no claim about what it will detect with real hardware.**

## What you need (honest hardware list)

- **Two phase-coherent receive channels on one clock**: a reference channel and a surveillance channel sampled in step. A KrakenSDR (5 coherent RTL-SDR channels), a USRP with two channels from one clock, a BladeRF 2.0 or an Airspy-style multi-channel setup would do. Two separate dongles with separate clocks do **not** work: the Doppler processing needs the two channels to share the same oscillator and sample clock.
- **Two antennas with direction**: the reference antenna aims at the transmitter; the surveillance antenna aims at the sky area to watch, and should be as deaf as possible toward the transmitter (a Yagi pointed away, or a null steered toward it). The transmitter's direct signal is typically 60 to 100 dB above an aircraft echo. The receiver's dynamic range and the antenna isolation decide the result, more than the software.
- A transmitter whose position you know. The receiver position is the one set for ADS-B.
- Range: with FM at 250 kS/s one range sample is 1.2 km of bistatic range; FM resolves roughly 2 to 3 km. Detection range depends on transmitter power, antenna gain and aircraft size, and Eye of SaurCom cannot predict it.

## What it does

1. **Clutter cancellation.** The surveillance channel contains the direct signal and static multipath. A least-squares filter (ECA, the default) or an NLMS filter removes everything that is a delayed copy of the reference over the first N range samples ("Cancellation taps"). Targets whose bistatic range is shorter than N samples are hidden by this.
2. **Cross-ambiguity function.** For each delay the cleaned surveillance signal is multiplied by the delayed reference, summed in blocks (this also decimates to a 1 kHz slow-time rate) and Fourier transformed: a map of bistatic range against Doppler.
3. **CFAR.** Two-dimensional cell-averaging CFAR with a set false-alarm probability, a zero-Doppler notch and the cancelled zone skipped.
4. **Tracking.** A Kalman filter on bistatic range and range rate (Doppler gives the rate: rate = -wavelength x Doppler), nearest-neighbour association with a chi-square gate, a track is confirmed after three hits.
5. **Display.** The range-Doppler map, a Doppler-time waterfall, the track table and a local map with the **bistatic range ellipse** of each track: the aircraft is somewhere on the ellipse with the transmitter and the receiver as its foci. One passive receiver gives a range and a speed, not a position. A position needs a second receiver with another illuminator, or the intersection with an ADS-B track.

Sources: the simulator (synthetic targets of chosen range, speed and level; FM or noise-like waveform), one two-channel interleaved cf32 file, or two single-channel cf32 files (reference, surveillance), all sample-synchronous. A source object for live coherent receivers is the same `MultiChannelSource` interface the direction finder uses, so a coherent-radio driver can feed this window later.

The illuminator (transmitter) is entered by hand: latitude, longitude, a name, saved in a list. There is no transmitter database in Eye of SaurCom.

## Verified in the tests (tests/radar/test_passive_radar.cpp)

Simulated, 250 kS/s, 100 MHz, 0.512 s coherent integration, clean reference (55 dB SNR), a direct path and three static multipath copies, thermal noise 65 dB below the direct path. Pfa is set to 1e-6 per cell:

| echo level vs direct path | detection probability, FM | noise-like (DVB-T-like) |
|---|---|---|
| -80 dB | 100 % (16 of 16) | 100 % |
| -90 dB | 94 % | 81 % |
| -100 dB and weaker | 0 % | 0 % |

Range error about 0.4 km rms, Doppler error 0.1 to 0.2 Hz rms (a bin is 2 Hz). No false alarm in 20 target-free runs (532 thousand cells). A target at 150 m/s is tracked over 10 integration periods within 0.1 km and 1 m/s. Cancellation depth is about 54 dB, set by the reference noise: a noisy reference passes its noise into the cleaned channel (real systems rebuild a clean copy of the DVB-T signal for this reason; that reconstruction is not implemented here). The range ellipses are checked against the sum of distances to the foci (error under 100 m at 34 km baseline).

## Not verified, not done

- Anything with real signals, real antennas, real clocks, or a real aircraft. The simulator has a perfect common clock, no antenna isolation limit, no ionospheric or tropospheric effects and no moving clutter (trees, cars, other aircraft at the same Doppler).
- DVB-T reference reconstruction (demodulate and re-modulate for a clean reference). FM and DVB-T are used raw.
- Range migration compensation: a target moving more than a range cell during one integration is smeared. At FM resolution and 0.5 s this only matters above about 2000 m/s.
- Multi-illuminator fusion and a map-server upload.

## Method sources

Written from the published descriptions, no code was copied: M. Malanowski, *Signal Processing for Passive Bistatic Radar* (Artech House 2019); F. Colone, D. O'Hagan, P. Lombardo, C. Baker, "A multistage processing algorithm for disturbance removal and target detection in passive bistatic radar", IEEE Trans. AES 45 (2009); P. Howland, D. Maksimiuk, G. Reitsma, "FM radio based bistatic radar", IEE Proc. Radar Sonar Navig. 152 (2005); H. Rohling, "Radar CFAR thresholding in clutter and multiple target situations", IEEE Trans. AES 19 (1983).
