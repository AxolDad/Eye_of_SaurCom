# saurcom_same

SAME / EAS (47 CFR 11.31): the header parser (`ZCZC-ORG-EEE-PSSCCC...+TTTT-JJJHHMM-LLLLLLLL-`, `NNNN`), the originator / event / state / Canadian province tables,
partial-county digits, one location code in words, issue time (epoch and text), purge time text, `trimMessage` and the county-table CSV reader.

Modules: `parse` (header, trim), `locate` (tables, location text), `time`, `county` (CSV to records), `tables` (data), `text` (bounded writer). No allocation,
no panics, `unsafe` only in `ffi.rs`. The user's county table (state with a mutex) stays in `core/src/same/same_proto.cpp`; `same_proto_rs.cpp` is the Rust-backed
implementation of the rest of `dect2/same/same_proto.h`.

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/same.h`, symbols prefixed `saurcom_same_`. Golden-vector test: `tests/same/test_same_proto_rs.cpp` (about 2.3 M results hashed),
fuzz target `fuzz_same_proto`. Status and conventions: ../README.md.
