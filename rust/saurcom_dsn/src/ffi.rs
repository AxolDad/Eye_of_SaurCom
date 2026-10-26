//! C ABI of `saurcom_dsn` (core/include/dect2/rs/dsn.h). The caller owns every buffer; the struct sizes are exported so that the C++ side
//! can check its copies of the layouts.
#![allow(clippy::missing_safety_doc)]

use crate::{dish_info, parse, parse_config, spacecraft_name, Config, Feed};

unsafe fn bytes<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if p.is_null() || n == 0 {
        &[]
    } else {
        core::slice::from_raw_parts(p, n)
    }
}

/// ABI version of this module's C interface; bump it when an exported signature or struct changes.
#[no_mangle]
pub extern "C" fn saurcom_dsn_version() -> u32 {
    1
}

/// sizeof(SaurcomDsnFeed)
#[no_mangle]
pub extern "C" fn saurcom_dsn_feed_size() -> usize {
    core::mem::size_of::<Feed>()
}

/// sizeof(SaurcomDsnConfig)
#[no_mangle]
pub extern "C" fn saurcom_dsn_config_size() -> usize {
    core::mem::size_of::<Config>()
}

/// Reads a dsn.xml report into `out` (cleared first). False when the text is not a DSN report.
#[no_mangle]
pub unsafe extern "C" fn saurcom_dsn_parse(xml: *const u8, n: usize, out: *mut Feed) -> bool {
    if out.is_null() {
        return false;
    }
    parse(bytes(xml, n), &mut *out)
}

/// Reads config.xml into `out` (cleared first). False when the text is not the DSN configuration.
#[no_mangle]
pub unsafe extern "C" fn saurcom_dsn_parse_config(xml: *const u8, n: usize, out: *mut Config) -> bool {
    if out.is_null() {
        return false;
    }
    parse_config(bytes(xml, n), &mut *out)
}

/// The full name of a spacecraft call sign into `out` (NUL-terminated, cut to `cap`); returns its length, 0 when the config lacks it.
#[no_mangle]
pub unsafe extern "C" fn saurcom_dsn_spacecraft_name(
    cfg: *const Config,
    call: *const u8,
    call_len: usize,
    out: *mut u8,
    cap: usize,
) -> usize {
    if cfg.is_null() || out.is_null() || cap == 0 {
        return 0;
    }
    let Ok(call) = core::str::from_utf8(bytes(call, call_len)) else { return 0 };
    let o = core::slice::from_raw_parts_mut(out, cap);
    match spacecraft_name(&*cfg, call) {
        Some(name) => {
            crate::put(o, name);
            crate::text(o).len()
        }
        None => {
            o[0] = 0;
            0
        }
    }
}

/// The index of a dish in the config's dish list (for its type and site), -1 when it is not listed.
#[no_mangle]
pub unsafe extern "C" fn saurcom_dsn_dish_index(cfg: *const Config, dish: *const u8, dish_len: usize) -> i32 {
    if cfg.is_null() {
        return -1;
    }
    let Ok(dish) = core::str::from_utf8(bytes(dish, dish_len)) else { return -1 };
    let cfg = &*cfg;
    match dish_info(cfg, dish) {
        Some(d) => cfg.dishes.iter().position(|x| core::ptr::eq(x, d)).map(|i| i as i32).unwrap_or(-1),
        None => -1,
    }
}
