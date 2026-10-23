//! Math functions for the `no_std` crate: the C library in the library build, the std methods under `cargo test`.
#[cfg(not(test))]
#[allow(unsafe_code)]
mod c {
    extern "C" {
        pub fn cos(x: f64) -> f64;
        pub fn sqrt(x: f64) -> f64;
        pub fn exp(x: f64) -> f64;
        pub fn atan2(y: f64, x: f64) -> f64;
        pub fn floor(x: f64) -> f64;
        pub fn ceil(x: f64) -> f64;
    }
}
#[cfg(not(test))]
#[allow(unsafe_code)]
mod w {
    use super::c;
    // SAFETY: the C library math functions are pure and take plain values.
    pub fn cos(x: f64) -> f64 { unsafe { c::cos(x) } }
    pub fn sqrt(x: f64) -> f64 { unsafe { c::sqrt(x) } }
    pub fn exp(x: f64) -> f64 { unsafe { c::exp(x) } }
    pub fn atan2(y: f64, x: f64) -> f64 { unsafe { c::atan2(y, x) } }
    pub fn floor(x: f64) -> f64 { unsafe { c::floor(x) } }
    pub fn ceil(x: f64) -> f64 { unsafe { c::ceil(x) } }
}
#[cfg(test)]
mod w {
    pub fn cos(x: f64) -> f64 { x.cos() }
    pub fn sqrt(x: f64) -> f64 { x.sqrt() }
    pub fn exp(x: f64) -> f64 { x.exp() }
    pub fn atan2(y: f64, x: f64) -> f64 { y.atan2(x) }
    pub fn floor(x: f64) -> f64 { x.floor() }
    pub fn ceil(x: f64) -> f64 { x.ceil() }
}
pub use w::*;
pub const PI: f64 = core::f64::consts::PI;
pub fn round(x: f64) -> f64 { floor(x + 0.5) }
