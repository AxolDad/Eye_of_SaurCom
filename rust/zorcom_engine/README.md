# zorcom_engine

Will hold: The engine threading core: lock-free ring buffer, worker threads, hand-off of sample blocks.
Status: skeleton (only `zorcom_engine_version`). Plan and conventions: ../README.md.
C ABI: `src/ffi.rs`, header `core/include/dect2/rs/engine.h`, symbols prefixed `zorcom_engine_`.
Shared CRC/bit/hex helpers come from `zorcom_common`; keep protocol logic in safe modules and `unsafe` in `ffi.rs` only.
