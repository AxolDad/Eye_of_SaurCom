# saurcom_dvbsi

The DVB service information layer: broadcast character sets and the data decoders of the DVB tuner side.

* **Text** (`charset`, `tables`): EBU Latin (ETSI TS 101 756 Annex C), ISO/IEC 6937 (ETSI EN 300 468 Annex A, with the accent + letter pairs), UCS-2 big endian with surrogate pairs, the DAB charset selector
  (ETSI EN 300 401: EBU Latin, ISO 8859-1, UCS-2, UTF-8), `clean` (valid UTF-8, no control or invisible characters, runs of spaces folded, in place), and the DVB text field with its character table
  prefix (`dvb_text`: default table, GB 2312 detection for DTMB, line feed / C1 clean-up). The character sets the system provides (ISO 8859-5..15, GB 2312, Big5, EUC-KR, iconv's UCS-2) are reached through
  a callback the C++ side registers (`saurcom_dvbsi_set_convert`, its iconv / Windows code page wrapper `dvbsi::convertCharset`); without one the bytes are kept. The tables are generated from the C++ ones.
* **Teletext** (`teletext`, `ttxrender`; ETSI EN 300 706 / EN 300 472): Hamming 8/4 and 24/18, bit reversal, the Latin G0 national option subsets, the PES data unit walk, the decoding of one 44 byte data unit into a
  header / row / Fastext (X/27/0) record, and the page renderer (colours, alpha and separated mosaics, flash, conceal, hold mosaic, double height, boxes) with the UTF-8 row text.
* **DVB subtitles** (`subs`, `subpix`; ETSI EN 300 743): the PES header with its PTS, the segment walk, the page / region / CLUT / object / display definition parsers, YCrCbT to RGBA, the default colour
  tables, the 2-, 4- and 8-bit run-length pixel strings with their map tables, index to RGBA conversion, region blending and the 33-bit PTS distance.

`unsafe` only in `ffi.rs`; no allocation (bounded writers into caller buffers, plain `repr(C)` records into caller arrays).

Stays in C++: the page store of `TeletextDecoder` (which page a row belongs to, serial transmission, completion, the 4096 page bound), the region / CLUT / object state and display queue of `DvbSubDecoder`,
PES reassembly from transport packets, and the iconv wrapper. The decoder classes call the layer through `teletext.h` / `dvbsub.h` (`ttxDecodeUnit`, `ttxPesUnits`, `dvbSubParse*`, ...); the Rust-backed implementations are
`teletext_rs.cpp`, `dvbsub_rs.cpp`, `text_charset_rs.cpp`, `dvb_text_rs.cpp` (the C++ copies are deleted; `teletext.cpp`, `dvbsub.cpp`, `text_charset.cpp`, `dvb_text.cpp` keep the decoder classes, `appendUtf8` and the iconv wrapper).

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/dvbsi.h`, symbols prefixed `saurcom_dvbsi_`. Golden-vector test: `tests/dvbsi/test_dvbsi_proto_rs.cpp` (about 6 M results hashed at the default 200000 iterations per
family; the whole `TeletextDecoder` and `DvbSubDecoder` classes are compared end to end on random PES streams), fuzz target `fuzz_dvbsi_proto`. `dvbsub.cpp` is compiled with `-ffp-contract=off`
(the YCrCb conversion). Status and conventions: ../README.md.
