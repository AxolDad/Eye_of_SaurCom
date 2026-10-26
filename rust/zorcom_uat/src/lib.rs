//! Eye of ZorCom UAT (978 MHz): the message layer of RTCA DO-282B: ADS-B / TIS-B downlink messages (header, state vector, mode status, auxiliary state vector)
//! and the uplink frame (position, slot, application data split into information frames, FIS-B APDU headers). The Reed-Solomon FEC (third_party/uat_fec),
//! the demodulator, the track table and the UI glue stay in C / C++ (`core/src/uat/`). Port of dump978's `uat_decode.c`.
//! Safe protocol code goes in modules next to `ffi`; only `ffi` may use `unsafe` (see rust/README.md).
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

pub mod downlink;
pub mod uplink;

#[allow(unsafe_code)]
pub mod ffi;

#[cfg(test)]
mod tests;
