//! The one static library the C++ code links (`libzorcom_rs.a`). A single staticlib avoids duplicate Rust runtime/`core` symbols, which
//! several per-module staticlibs would each carry. Every module crate keeps its C ABI in its own `ffi.rs`; the `extern "C"` items are
//! `#[no_mangle]` and therefore end up in this archive, and the `use` lines below make sure each crate is linked in even when no Rust
//! code calls it. This crate also owns the panic handler (members are `no_std` rlibs and define none).
//!
//! Feature `bare` (what CMake builds with) makes the crate `no_std` and installs the panic handler, so the archive carries no Rust `std`.
//! Without it (plain `cargo test`, clippy) the crate is ordinary std code, because test builds unwind and `no_std` needs panic=abort.
#![cfg_attr(feature = "bare", no_std)]

pub use zorcom_acars as acars;
pub use zorcom_adsb as adsb;
pub use zorcom_ax25 as ax25;
pub use zorcom_common as common;
pub use zorcom_cot as cot;
pub use zorcom_wifisense as wifisense;
pub use zorcom_dsn as dsn;
pub use zorcom_dab as dab;
pub use zorcom_dmr as dmr;
pub use zorcom_drm as drm;
pub use zorcom_dstar as dstar;
pub use zorcom_dvbsi as dvbsi;
pub use zorcom_engine as engine;
pub use zorcom_fec as fec;
pub use zorcom_files as files;
pub use zorcom_hfdl as hfdl;
pub use zorcom_kaws as kaws;
pub use zorcom_kris as kris;
pub use zorcom_mesh as mesh;
pub use zorcom_net as net;
pub use zorcom_nxdn as nxdn;
pub use zorcom_p25 as p25;
pub use zorcom_pager as pager;
pub use zorcom_proto as proto;
pub use zorcom_ring as ring;
pub use zorcom_same as same;
pub use zorcom_tetra as tetra;
pub use zorcom_ts as ts;
pub use zorcom_uat as uat;
pub use zorcom_usb as usb;
pub use zorcom_vdl2 as vdl2;
pub use zorcom_wisun as wisun;

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
