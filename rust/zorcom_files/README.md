# zorcom_files

The pure parsing and writing layers of the files the program reads and writes (`core/include/dect2/files/files_proto.h`, namespace `dect2::filesp`):

* **CSV** (`csv.rs`): the channel list files (`scripts/ingest.py`, RFC 4180 style: quoted fields with `,` `"` and line breaks, `""`, blank lines skipped), the field quoting of the
  writer, and the trimmed `A|B|C` services list. A two-pass API without allocation (`measure`, then `parse` into caller buffers).
* **SigMF** (`json.rs`, `sigmf.rs`): the small JSON reader (validates the whole document, then looks members up with "the last duplicate key wins"; no tree, no allocation), the
  meta reader (data type, rate, centre frequency, sample start, header / trailing bytes, annotation count, description, data set name) and both meta writers (the recorder's and the
  test tool's). `%g` / `%.1f` come from the C library (`ffi::cfn`, `snprintf`) so the text is the one the C++ gave.
* **WAV** (`wav.rs`): the `fmt ` chunk (also WAVE_FORMAT_EXTENSIBLE), the sample format choice for two channel IQ files, the end of the `data` chunk, the centre frequency in a file name.
* **TLE** (`tle.rs`): NORAD two-line elements: columns, checksum, the implied-decimal exponent fields, the epoch Julian date, name handling ("0 " of 3LE), and the scan of a whole text.
* **SQLite pages** (`sqlite.rs`): what the MBTiles reader needs from the file format: database header, varints, records, overflow payload sizes, page headers, the four b-tree cell kinds,
  the MBTiles row rule (TMS), PNG / JPEG detection, the `tiles` schema checks.
* **Results container** (`container.rs`): the `OARS` header, FNV-1a and the signature hash of `core/include/dect2/results/results_io.h`.

No allocation: callers pass (pointer, length) buffers, text fields come back as byte ranges. `unsafe` only in `ffi.rs` (the C ABI and the three C library calls `pow`, `floor`, `snprintf`).

## Behaviour fixed on the way (identically in the C++ reference `files_proto.cpp` and in Rust)

* Numbers read from text (SigMF meta, TLE fields) are decimal tokens only (`[+-]? (digits [. digits*] | . digits) ([eE] [+-]? digits)?`); `strtod` also took hex floats, `inf` and `nan`.
* The TLE exponent field is read with a strict sign-and-digits check (`strtol` stopped at a NUL byte and accepted blanks).
* The SigMF writers do not cut descriptions at 1023 bytes (a `snprintf` buffer), write a non-finite rate or frequency as 0 (was `nan` / `inf`, not JSON), and the recorder leaves out a centre
  frequency of 1e18 or more (its `%.1f` text was cut at 95 digits).
* Doubles that do not fit a 64 bit unsigned (`sample_start`, `header_bytes`) saturate (the cast was undefined).
* A data type that Eye of ZorCom cannot play no longer leaves a half-set endianness in the result; the b-tree code refuses a usable page size under 36 bytes and checks that a page is long enough for its header.

## Stays in C++

The file system work (finding the data file next to a `.sigmf-meta`, file sizes, `fopen`, atomic writes), the WAV chunk walk (seeking), the MBTiles b-tree walks and overflow page chains
(`mbtiles.h` keeps the loops and the page cache), the struct-walking encoder of the results (`pack` / `unpack` templates), the channel list rows (`Row`, merge, GitHub issue text), SGP4 and
pass prediction, the `settings` / ImGui ini files of the app (they are written and read by the GUI library and the app preference code, not by `core`), the channel list `fmtFreq` formatting.

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/files.h`, symbols prefixed `zorcom_files_`. C++ side: `core/src/files/files_proto_rs.cpp` (record layouts are checked at start-up; the C++ it replaced is deleted). Golden-vector test `tests/files/test_files_proto_rs.cpp` (100000
inputs per family, plus the C ABI with null pointers and short arrays), fuzz target `fuzz_files_proto`. Status and conventions: ../README.md.
