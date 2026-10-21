# Propagation (Tools > Propagation)

Where can a transmitter be received? Place it on the map (or use your location), enter power, antenna gain (or the EIRP), frequency, bandwidth, the
SNR your receiver needs and the UTC date and time. The map shows the predicted SNR as a heat map (the white line marks the SNR you asked for), a
white ring at the range where it is just met, and clicking a point (set "a map click sets" to "a probe point") gives distance, model, path loss,
received level, field strength, noise, SNR and margin. The inputs are saved. The grid is computed on a worker thread; the resolution setting
(40 / 80 / 140 cells per side) keeps it cheap on a phone or a Raspberry Pi. Nothing is downloaded and **there is no terrain**: hills, buildings and
trees are not modelled, so real coverage is usually shorter than shown. Code: `core/include/dect2/prop/prop.h`, `core/src/prop/prop.cpp`
(pure functions, tested by `tests/prop/test_prop_model.cpp`); screen: `app/prop/prop_ui.cpp`.

All formulas are written from the public ITU-R texts (no code copied). The model is picked by frequency and named in the window.

## Link budget

Received power `Pr = EIRP - L + Gr` (dBm). EIRP = power (W, dBW or dBm) + antenna gain - feeder loss, or the EIRP typed directly.
SNR = `Pr - N`, margin = SNR minus the SNR needed. Field strength (P.525): `E[dBuV/m] = Pr[dBm] - Gr + 20 log f[MHz] + 77.2`.

## Noise

`N = kTB + 10 log(Fa + Fr - 1)` with kTB = -174 dBm/Hz + 10 log B, Fr the receiver noise figure, and Fa the external noise factor
(P.372, simplified): `Fa = c - d log10 f[MHz]` dB for man-made noise (city 76.8 / 27.7, residential 72.5 / 27.7, rural 67.2 / 27.7, quiet rural
53.6 / 28.6) or galactic noise (52.0 / 23.0), whichever is larger, never below 0 dB. Limits: the table is a simplified fit of the P.372 median curves
(valid 0.3 to a few hundred MHz); it ignores atmospheric (lightning) noise, which dominates at night on the lower HF bands, and the antenna's own
noise temperature.

## 30 MHz and up: line of sight and diffraction

Loss `32.44 + 20 log f + 20 log d` (P.525 free space) inside the radio horizon `4.12 (sqrt h1 + sqrt h2)` km (4/3 earth). Beyond it the smooth-earth
diffraction loss of P.526 (`F(X) + G(Y1) + G(Y2)`, beta = 1) is added, floored at 0. Antenna heights are optional (defaults 10 m / 2 m).
Limits: no terrain, buildings, foliage or knife edges; no two-ray ground reflection (the real field inside the horizon fluctuates around free space);
no ducting, tropospheric scatter, rain or gas absorption (matters above ~6 GHz; the window warns); **sporadic E is not predicted**; polarisation
is ignored. Treat the range as an optimistic figure in towns and hills.

## Below 30 MHz: ground wave and skywave (an estimate)

- Ground wave: free space loss plus the Norton attenuation factor in Terman's form `A = (2 + 0.3p) / (2 + p + 0.6p^2)`, `p = pi d / (lambda x)`,
  `x = 1.8e4 sigma / f[MHz]`, sigma = 5 (sea), 0.01 (wet), 0.005 (average), 0.001 (dry) S/m. Vertical polarisation over a uniform ground; no land/sea
  mixing, no antenna ground system, no terrain.
- Skywave: one F2 layer at 300 km. `foF2` = night + (day - night) * sqrt(cos zenith-angle term), day = (5.5 + 0.045 R12) * (1 - 0.35 sin^2 lat) with
  R12 from the solar flux you enter (SFI = 63.7 + 0.728 R + 0.00089 R^2), night = 0.45 day; the sun position from the date, UTC time and the path
  midpoint. MUF = foF2 * sec(incidence angle) with curved-earth geometry for hops of at most 4000 km. A skywave exists only when f is under the MUF
  (so there is a skip zone near the transmitter, and none at all when the band is closed). D-layer absorption per hop after P.533:
  `La = 677.2 Ir sec(phi) / ((f + 1)^1.98 + 10.2)`, `Ir = (1 + 0.0037 R) * max(cos(0.881 chi)^1.3, 0.1)`. Loss = free space over the slant
  path + absorption + 2 dB per ground reflection. The result is the power sum of ground wave and skywave.
- Limits: this is a deliberately simple model, good for "is the band open, roughly how far", not for a circuit plan. foF2 can be off by a few MHz
  (no monthly maps, no winter anomaly, no equatorial crests, no geomagnetic latitude, no storms or auroral/polar-cap absorption), no E-layer or
  sporadic-E propagation, no multipath or fading margin, no Es-hop, a fixed gyrofrequency term, and the antenna pattern and take-off angle are
  ignored. Flux values are typed by you; the tool never fetches space weather.

## Licence note

Formulas only (ITU-R P.525, P.526, P.372, P.368 background, P.533 absorption term). No ITU data tables or software are included.
