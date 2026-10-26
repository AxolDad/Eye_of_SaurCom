//! The one static library the C++ code links (`libsaurcom_rs.a`). A single staticlib avoids duplicate Rust runtime/`core` symbols, which
//! several per-module staticlibs would each carry. Every module crate keeps its C ABI in its own `ffi.rs`; the `extern "C"` items are
//! `#[no_mangle]` and therefore end up in this archive, and the `use` lines below make sure each crate is linked in even when no Rust
//! code calls it. This crate also owns the panic handler (members are `no_std` rlibs and define none).
//!
//! Feature `bare` (what CMake builds with) makes the crate `no_std` and installs the panic handler, so the archive carries no Rust `std`.
//! Without it (plain `cargo test`, clippy) the crate is ordinary std code, because test builds unwind and `no_std` needs panic=abort.
#![cfg_attr(feature = "bare", no_std)]

pub use saurcom_acars as acars;
pub use saurcom_adsb as adsb;
pub use saurcom_ax25 as ax25;
pub use saurcom_common as common;
pub use saurcom_cot as cot;
pub use saurcom_wifisense as wifisense;
pub use saurcom_dsn as dsn;
pub use saurcom_dab as dab;
pub use saurcom_dmr as dmr;
pub use saurcom_drm as drm;
pub use saurcom_dstar as dstar;
pub use saurcom_dvbsi as dvbsi;
pub use saurcom_engine as engine;
pub use saurcom_fec as fec;
pub use saurcom_files as files;
pub use saurcom_hfdl as hfdl;
pub use saurcom_kaws as kaws;
pub use saurcom_kris as kris;
pub use saurcom_mesh as mesh;
pub use saurcom_net as net;
pub use saurcom_nxdn as nxdn;
pub use saurcom_p25 as p25;
pub use saurcom_pager as pager;
pub use saurcom_proto as proto;
pub use saurcom_ring as ring;
pub use saurcom_same as same;
pub use saurcom_tetra as tetra;
pub use saurcom_ts as ts;
pub use saurcom_uat as uat;
pub use saurcom_usb as usb;
pub use saurcom_vdl2 as vdl2;
pub use saurcom_wisun as wisun;

/// Panics must never unwind into C++: abort.
#[cfg(feature = "bare")]
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    extern "C" {
        fn abort() -> !;
    }
    unsafe { abort() }
}

// The prebuilt `core` still names the unwinding personality routine; with panic=abort nothing ever calls it.
#[cfg(feature = "bare")]
#[no_mangle]
pub extern "C" fn rust_eh_personality() {}
