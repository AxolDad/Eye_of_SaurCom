//! WiFi sensing for Eye of SaurCom: the CSI frames of RuView's ESP32 sensor nodes (ADR-018) turned into presence, motion, breathing rate and
//! heart rate. The vital-sign algorithms are a port of RuView's `wifi-densepose-vitals` (MIT License, Copyright (c) 2024 rUv); see vitals.rs.
//! `no_std` with `alloc`, no dependencies; only `ffi` and `mathf` use `unsafe`.
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

extern crate alloc;

#[allow(unsafe_code)]
pub mod ffi;
pub mod frame;
pub mod mathf;
pub mod sensor;
pub mod vitals;

#[cfg(test)]
mod tests;
