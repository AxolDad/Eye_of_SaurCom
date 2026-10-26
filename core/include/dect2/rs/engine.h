// C ABI of the Rust crate rust/saurcom_engine (engine core). Written by hand; the Rust test rust/saurcom/tests/headers.rs checks it against ffi.rs.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ABI version of this module (1 for the skeleton).
uint32_t saurcom_engine_version(void);

#ifdef __cplusplus
}
#endif
