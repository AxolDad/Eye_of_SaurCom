//! C ABI of `zorcom_cot` (core/include/dect2/rs/cot.h). Text comes as (pointer, length) UTF-8 pairs and the caller owns all buffers.
//! `zorcom_cot_encode` returns the length written or `usize::MAX`; strings handed out as `const char*` are static and NUL-terminated.
#![allow(clippy::missing_safety_doc)]

use crate::{decode, encode, Event, Kind};

const FAIL: usize = usize::MAX;

unsafe fn text<'a>(p: *const u8, n: usize) -> Option<&'a str> {
    if p.is_null() || n == 0 {
        return Some("");
    }
    core::str::from_utf8(core::slice::from_raw_parts(p, n)).ok()
}

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn zorcom_cot_version() -> u32 {
    1
}

/// sizeof(ZorcomCotEvent): the C++ side checks it against its own struct.
#[no_mangle]
pub extern "C" fn zorcom_cot_struct_size() -> usize {
    core::mem::size_of::<ZorcomCotEvent>()
}

/// The CoT type of a contact kind (0 aircraft, 1 vessel, 2 station, 3 balloon, 4 mesh node); "" for an unknown kind.
#[no_mangle]
pub extern "C" fn zorcom_cot_type(kind: u32) -> *const u8 {
    match Kind::from_u32(kind) {
        Some(Kind::Aircraft) => b"a-n-A-C-F\0".as_ptr(),
        Some(Kind::Vessel) => b"a-n-S-C\0".as_ptr(),
        Some(Kind::Station) => b"a-n-G\0".as_ptr(),
        Some(Kind::Balloon) => b"a-n-A-C-L\0".as_ptr(),
        Some(Kind::MeshNode) => b"a-n-G-U-C\0".as_ptr(),
        None => b"\0".as_ptr(),
    }
}

/// One event's XML into `out`. Unknown values: `hae` NaN, `course_deg` / `speed_mps` negative.
#[no_mangle]
pub unsafe extern "C" fn zorcom_cot_encode(
    uid: *const u8, uid_len: usize,
    cot_type: *const u8, type_len: usize,
    callsign: *const u8, callsign_len: usize,
    remarks: *const u8, remarks_len: usize,
    lat: f64, lon: f64, hae: f64, course_deg: f64, speed_mps: f64,
    time_ms: i64, stale_sec: i32,
    out: *mut u8, cap: usize,
) -> usize {
    let (Some(uid), Some(cot_type), Some(callsign), Some(remarks)) =
        (text(uid, uid_len), text(cot_type, type_len), text(callsign, callsign_len), text(remarks, remarks_len))
    else {
        return FAIL;
    };
    if out.is_null() || cap == 0 {
        return FAIL;
    }
    let buf = core::slice::from_raw_parts_mut(out, cap);
    let e = Event { uid, cot_type, callsign, remarks, lat, lon, hae, course_deg, speed_mps, time_ms, stale_sec };
    encode(&e, buf).unwrap_or(FAIL)
}

/// A received contact; the texts are NUL-terminated UTF-8. `hae` is NaN when unknown; times are ms since 1970 (0 when absent).
#[repr(C)]
pub struct ZorcomCotEvent {
    pub uid: [u8; 96],
    pub cot_type: [u8; 48],
    pub callsign: [u8; 64],
    pub lat: f64,
    pub lon: f64,
    pub hae: f64,
    pub time_ms: i64,
    pub stale_ms: i64,
}

/// Reads one event document; false (and `out` untouched) when it is not a CoT event with a uid, a type and a valid point.
#[no_mangle]
pub unsafe extern "C" fn zorcom_cot_decode(xml: *const u8, n: usize, out: *mut ZorcomCotEvent) -> bool {
    if xml.is_null() || n == 0 || out.is_null() {
        return false;
    }
    let Some(r) = decode(core::slice::from_raw_parts(xml, n)) else { return false };
    let o = &mut *out;
    o.uid = r.uid.bytes;
    o.cot_type = r.cot_type.bytes;
    o.callsign = r.callsign.bytes;
    o.lat = r.lat;
    o.lon = r.lon;
    o.hae = r.hae;
    o.time_ms = r.time_ms;
    o.stale_ms = r.stale_ms;
    true
}
