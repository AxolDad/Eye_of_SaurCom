# DECT bands

Eye of ZorCom listens to DECT base-station beacons (identity, system information, bursts). Pick the band in the **Band** list of the tuner, then a carrier, or use **Scan band** to step through the carriers (1.5 s each, it stays on one while bursts are heard). Europe is the default. The radio must reach the band: a RTL-SDR (R820T, about 24-1700 MHz) cannot tune 1.9 GHz and the tuner then says so, using the radio's own range (and the converter, if one is set). HackRF, PlutoSDR, LimeSDR and BladeRF reach it.

All DECT carriers lie on one raster 1.728 MHz apart, 1.152 Mbit/s GFSK (ETSI EN 300 175-2, clause 5.2 and annex F, V2.7.1).

| Band in Eye of ZorCom | Allocation | Carriers | Centre frequencies |
|---|---|---|---|
| Europe | 1880-1900 MHz | 10 | Fc = 1897.344 - c x 1.728 MHz, c = 0..9 (1897.344 ... 1881.792) |
| US / Canada (DECT 6.0) | 1920-1930 MHz | 5 | 1921.536 + n x 1.728 MHz, n = 0..4 (1921.536 ... 1928.448); ETSI c = 23..27, Fc = 1881.792 + c x 1.728 |
| Brazil | 1910-1920 MHz | 5 | 1911.168 ... 1918.080 (ETSI c = 17..21) |
| Latin America | 1910-1930 MHz | 11 | 1911.168 ... 1928.448 (ETSI c = 17..27) |
| US ISM 900 | 902-928 MHz | 14 | 903.744 + n x 1.728 MHz, n = 0..13 (RF band 01000, annex F.7.1) |

A band lists the carriers whose whole 1.728 MHz channel lies inside the allocation. The Brazil and Latin America plans are derived that way from the annex F raster; the counts in other sources (10 for Latin America) differ by the edge carriers, so check your national table. A base station tells its phones which carriers it may use, so a real system can use a subset.

Not added: Korea (1786-1792 MHz), Taiwan (1880-1895 MHz), Japan (J-DECT, 1893-1906 MHz) and China (1900-1920 MHz, until 2003) are listed in secondary sources only; EN 300 175-2 gives no carrier plan for them. Japan's PHS is not DECT.

## 900 MHz and 2.4 GHz cordless phones

EN 300 175-2 annex F.7 defines DECT carriers for the US ISM bands 902-928 MHz (above) and 2400-2483.5 MHz, but almost no product uses them. What is sold as a "900 MHz" or "2.4 GHz" cordless phone is analogue FM, FHSS or a vendor-specific digital system, **not DECT**: Eye of ZorCom does not decode those. "DECT 6.0" is the 1920-1930 MHz band, not 6 GHz and not 900 MHz. The 902-928 MHz entry is for the rare standard-DECT-in-ISM case.
