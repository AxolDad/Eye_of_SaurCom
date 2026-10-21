# Direction finding (KrakenSDR / KerberosSDR style arrays)

Code: `core/include/dect2/df/` (`df.h` estimators, `source.h` sources, `group.h` coherent group, `rfcam.h`, `position.h`), UI in `app/df/`.

## What exists
- Estimators: Bartlett, Capon, MUSIC (grid scan over azimuth), root-MUSIC (ULA), interferometer; uniform circular / linear / custom arrays with configurable radius or spacing; MDL source count; forward-backward averaging, spatial smoothing.
- Calibration from a covariance recorded while the noise source is on (`estimateCalibration`).
- `CoherentGroup` (`group.h`): N independent receivers (one RTL-SDR per channel) as one `MultiChannelSource`. `calibrate()` finds the integer start offset of every device by cross-correlation against channel 0, drops the lead, and estimates the per-channel complex gain from the dominant eigenvector of the noise covariance; it refuses channels whose coherence is below 0.8. `read()` then returns aligned, gain-corrected frames.
- Sources: simulator, cf32/SigMF file, KrakenSDR Heimdall IQ header parser, DoA-server line importer.
- Tests: `tests/df/test_df.cpp` (accuracy vs SNR, two sources, multipath), `tests/df/test_df_group.cpp` (lags, gains and MUSIC error under 2 degrees with devices that start 0 to 11 samples apart and carry unknown phases and amplitudes; worst case 0.14 degrees on synthetic data).

## Not verified on real hardware
No KrakenSDR or KerberosSDR has been connected. Unverified: the RTL driver adapter that turns N opened RTL devices into `ChannelFeed`s and starts them together, switching the hardware noise source (GPIO on the Kraken, a vendor control request), clock drift between devices over minutes (only a fixed integer lag and a fixed gain are corrected; re-run the calibration when the coherence in `GroupCalibration` falls), the Heimdall header layout beyond what the public iq_header definition states, and SigMF files from other writers.

## Not done
- The RTL adapter and the UI button wiring the hardware noise source to `CoherentGroup::calibrate()` (the UI already has a calibration button for the simulator and file sources).
- Fractional-sample delay correction.
