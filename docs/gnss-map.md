# GNSS satellites on the map

Where the satellites are comes from the navigation message the receiver decodes (parity/CRC checked, complete and consistent sets only), computed at the
current signal time with the ICD orbit algorithm (IS-GPS-200 Table 20-IV Kepler solution; GLONASS ICD state vector integrated). The sub-satellite point
(WGS-84 latitude, longitude, height) is drawn with its RINEX name (G12, E05, C21, R07, J02), coloured by constellation.

- App: GNSS mode, Map view. Markers are dimmed when the satellite is not tracked; the tooltip says tracked, in the fix, almanac or ephemeris, and the
  ephemeris age. An almanac position is marked as such.
- Covered: GPS, QZSS, Galileo (E1B I/NAV), BeiDou (D1/D2), GLONASS - whatever the receiver has decoded an ephemeris for. SBAS has no orbit here.
- Not verified: with real signals (no hardware); checked with the simulator's ephemerides and closed-form orbits only. Time scales: GPST, GST, BDT
  and the GLONASS day are taken from the receiver's existing conversions.
- No third-party code or tables were copied; the algorithms are implemented from the ICDs.
