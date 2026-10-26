//! C ABI of `zorcom_engine` (core/include/dect2/rs/engine.h). Every export is `zorcom_engine_*`; pointers come as (pointer, length) pairs and the caller owns all buffers.

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn zorcom_engine_version() -> u32 {
    1
}
