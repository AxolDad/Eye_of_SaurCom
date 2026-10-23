//! C ABI of `onair_wifisense` (core/include/dect2/rs/wifisense.h). A sensor is created per node and freed by the caller.
#![allow(clippy::missing_safety_doc)]

use crate::frame;
use crate::sensor::Sensor;
use alloc::boxed::Box;

/// ABI version of this module's C interface; bump it when an exported signature changes.
#[no_mangle]
pub extern "C" fn onair_wifisense_version() -> u32 {
    1
}

/// One node's state after a frame. Rates are 0 and their confidence 0 while unknown.
#[repr(C)]
pub struct OnairWifiSense {
    pub node_id: u8,
    pub presence: u8,
    pub n_antennas: u8,
    pub _pad: u8,
    pub n_subcarriers: u32,
    pub freq_mhz: u32,
    pub seq: u32,
    pub rssi: i32,
    pub noise: i32,
    pub motion: f64,
    pub breathing_bpm: f64,
    pub breathing_conf: f64,
    pub heart_bpm: f64,
    pub heart_conf: f64,
    pub frames: u64,
}

/// sizeof(OnairWifiSense): the C++ side checks it against its own struct.
#[no_mangle]
pub extern "C" fn onair_wifisense_struct_size() -> usize {
    core::mem::size_of::<OnairWifiSense>()
}

/// The node id of a CSI datagram, or -1 when it is not a whole ADR-018 CSI frame.
#[no_mangle]
pub unsafe extern "C" fn onair_wifisense_node(d: *const u8, n: usize) -> i32 {
    if d.is_null() || n == 0 {
        return -1;
    }
    frame::parse(core::slice::from_raw_parts(d, n)).map_or(-1, |f| i32::from(f.node_id))
}

/// A sensor for one node sending `frames_per_sec` frames (RuView firmware: 50).
#[no_mangle]
pub extern "C" fn onair_wifisense_new(frames_per_sec: f64) -> *mut Sensor {
    Box::into_raw(Box::new(Sensor::new(frames_per_sec)))
}

#[no_mangle]
pub unsafe extern "C" fn onair_wifisense_free(s: *mut Sensor) {
    if !s.is_null() {
        drop(Box::from_raw(s));
    }
}

/// Feeds one datagram to the node's sensor; false (out untouched) when it is not a CSI frame.
#[no_mangle]
pub unsafe extern "C" fn onair_wifisense_feed(s: *mut Sensor, d: *const u8, n: usize, out: *mut OnairWifiSense) -> bool {
    if s.is_null() || d.is_null() || n == 0 || out.is_null() {
        return false;
    }
    let Some(f) = frame::parse(core::slice::from_raw_parts(d, n)) else { return false };
    let r = (*s).feed(&f);
    let o = &mut *out;
    o.node_id = f.node_id;
    o.presence = u8::from(r.presence);
    o.n_antennas = f.n_antennas as u8;
    o._pad = 0;
    o.n_subcarriers = f.n_subcarriers as u32;
    o.freq_mhz = f.freq_mhz;
    o.seq = f.seq;
    o.rssi = i32::from(f.rssi);
    o.noise = i32::from(f.noise);
    o.motion = r.motion;
    o.breathing_bpm = r.breathing.map_or(0.0, |e| e.bpm);
    o.breathing_conf = r.breathing.map_or(0.0, |e| e.confidence);
    o.heart_bpm = r.heart.map_or(0.0, |e| e.bpm);
    o.heart_conf = r.heart.map_or(0.0, |e| e.confidence);
    o.frames = r.frames;
    true
}
