//! C ABI of `saurcom_engine` (core/include/dect2/rs/engine.h). Every export is `saurcom_engine_*`; pointers come as (pointer, length) pairs and the caller owns all buffers.

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn saurcom_engine_version() -> u32 {
    1
}
