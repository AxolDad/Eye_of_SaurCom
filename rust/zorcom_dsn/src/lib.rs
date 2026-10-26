//! Deep Space Network Now for Eye of ZorCom: NASA's live XML of what the DSN dishes at Goldstone, Madrid and Canberra are doing
//! (https://eyes.nasa.gov/dsn/data/dsn.xml, refreshed about every 5 s) and its configuration (https://eyes.nasa.gov/dsn/config.xml: sites,
//! dish sizes, spacecraft names). The feed is not documented by NASA; the units follow the field analysis on Space Exploration Stack
//! Exchange (question 32479): upSignal power in kW and frequency in MHz, downSignal power in dBm (300 dB below the real value as a flag)
//! and frequency in Hz, ranges in km and round-trip light time in s (-1 when unknown). Values are range-checked instead of trusted.
//! `no_std`, no allocation, no dependencies; only `ffi` uses `unsafe`.
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

#[allow(unsafe_code)]
pub mod ffi;
pub mod xml;

#[cfg(test)]
mod tests;

use xml::{attr, Tags};

pub const MAX_STATIONS: usize = 4;
pub const MAX_DISHES: usize = 32;
pub const MAX_TARGETS: usize = 64;
pub const MAX_SIGNALS: usize = 128;
pub const MAX_SITES: usize = 4;
pub const MAX_SPACECRAFT: usize = 320;

pub const KM_PER_AU: f64 = 149_597_870.7;
pub const C_KM_S: f64 = 299_792.458;

/// A Deep Space Communications Complex and its clock.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Station {
    pub name: [u8; 8],      // gdscc, mdscc, cdscc
    pub friendly: [u8; 24], // Goldstone, Madrid, Canberra
    pub time_utc_ms: i64,
    pub tz_offset_ms: i64, // add to time_utc_ms for the local time
}

/// One antenna. `station` indexes the stations of the same report (the dish follows its station in the XML), -1 when none came first.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Dish {
    pub name: [u8; 8], // DSS14 ...
    pub activity: [u8; 72],
    pub azimuth_deg: f64,
    pub elevation_deg: f64,
    pub wind_kmh: f64, // NaN when not given
    pub station: i32,
    pub mspa: u8,  // multiple spacecraft per aperture
    pub array: u8, // part of an array of dishes
    pub ddor: u8,  // delta differential one-way ranging
    pub _pad: u8,
}

/// A spacecraft a dish points at. Ranges in km and the round-trip light time in s are NaN when the feed has -1.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Target {
    pub name: [u8; 16], // call sign: VGR1, JWST, MSL ...
    pub dish: i32,
    pub id: i32,
    pub upleg_km: f64,
    pub downleg_km: f64,
    pub rtlt_s: f64,
}

/// An uplink (dish to spacecraft) or a downlink.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Signal {
    pub spacecraft: [u8; 16],
    pub dish: i32,
    pub spacecraft_id: i32,
    pub up: u8, // 1 uplink, 0 downlink
    pub active: u8,
    pub kind: u8,          // 0 none (setup, teardown, tracking), 1 carrier only, 2 data, 3 other
    pub band: u8,          // 'S', 'X', 'K' (Ka) ... 0 when not given
    pub power_flagged: u8, // downlink: the feed had the power 300 dB low (corrected in power)
    pub _pad: [u8; 3],
    pub data_rate_bps: f64,
    pub frequency_hz: f64, // 0 when not published
    pub power: f64,        // uplink kW, downlink dBm
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Feed {
    pub timestamp_ms: i64,
    pub n_stations: u32,
    pub n_dishes: u32,
    pub n_targets: u32,
    pub n_signals: u32,
    pub stations: [Station; MAX_STATIONS],
    pub dishes: [Dish; MAX_DISHES],
    pub targets: [Target; MAX_TARGETS],
    pub signals: [Signal; MAX_SIGNALS],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Site {
    pub name: [u8; 8],
    pub friendly: [u8; 24],
    pub latitude: f64,
    pub longitude: f64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct DishInfo {
    pub name: [u8; 8],
    pub kind: [u8; 12], // 70M, 34M, 34MHEF ...
    pub site: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Spacecraft {
    pub name: [u8; 16], // lower case call sign as in the config
    pub friendly: [u8; 64],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Config {
    pub n_sites: u32,
    pub n_dishes: u32,
    pub n_spacecraft: u32,
    pub _pad: u32,
    pub sites: [Site; MAX_SITES],
    pub dishes: [DishInfo; MAX_DISHES],
    pub spacecraft: [Spacecraft; MAX_SPACECRAFT],
}

impl Feed {
    // every member is a number or a byte array, for which all zero bytes is a valid value
    #[allow(unsafe_code)]
    pub fn empty() -> Feed {
        unsafe { core::mem::zeroed() }
    }
}

impl Config {
    #[allow(unsafe_code)]
    pub fn empty() -> Config {
        unsafe { core::mem::zeroed() }
    }
}

/// Copies text into a fixed NUL-terminated buffer, decoding the five XML entities and cutting at a character boundary.
pub fn put(dst: &mut [u8], src: &str) {
    let mut n = 0;
    let cap = dst.len().saturating_sub(1);
    let mut rest = src;
    while let Some(c) = rest.chars().next() {
        let (ch, used) = if c == '&' {
            match rest {
                r if r.starts_with("&amp;") => ('&', 5),
                r if r.starts_with("&lt;") => ('<', 4),
                r if r.starts_with("&gt;") => ('>', 4),
                r if r.starts_with("&quot;") => ('"', 6),
                r if r.starts_with("&apos;") => ('\'', 6),
                _ => ('&', 1),
            }
        } else {
            (c, c.len_utf8())
        };
        let mut b = [0u8; 4];
        let e = ch.encode_utf8(&mut b).as_bytes();
        if n + e.len() > cap {
            break;
        }
        dst[n..n + e.len()].copy_from_slice(e);
        n += e.len();
        rest = &rest[used..];
    }
    for x in dst[n..].iter_mut() {
        *x = 0;
    }
}

/// The text of a fixed buffer, up to its NUL.
pub fn text(b: &[u8]) -> &str {
    let n = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    core::str::from_utf8(&b[..n]).unwrap_or("")
}

fn num(s: Option<&str>) -> f64 {
    s.and_then(|v| v.trim().parse::<f64>().ok()).filter(|v| v.is_finite()).unwrap_or(f64::NAN)
}

fn int(s: Option<&str>) -> i64 {
    s.and_then(|v| v.trim().parse::<f64>().ok()).filter(|v| v.is_finite()).map(|v| v as i64).unwrap_or(0)
}

fn yes(s: Option<&str>) -> u8 {
    matches!(s, Some("true") | Some("True") | Some("1")) as u8
}

/// -1 (and other negative values) mean unknown.
fn known(v: f64) -> f64 {
    if v.is_finite() && v >= 0.0 {
        v
    } else {
        f64::NAN
    }
}

/// A frequency to Hz whatever unit the feed used: below 40 it can only be GHz, below 40 000 MHz, else Hz. 0 or nonsense is 0 (not given).
pub fn frequency_hz(v: f64) -> f64 {
    if !v.is_finite() || v <= 0.0 {
        0.0
    } else if v < 40.0 {
        v * 1e9
    } else if v < 40_000.0 {
        v * 1e6
    } else {
        v
    }
}

/// A downlink power to dBm: values below -300 dBm carry a flag of -300 dB.
pub fn downlink_dbm(v: f64) -> (f64, bool) {
    if v.is_finite() && v < -300.0 {
        (v + 300.0, true)
    } else {
        (v, false)
    }
}

pub fn signal_kind(s: Option<&str>) -> u8 {
    match s {
        Some("none") | None => 0,
        Some("carrier") => 1,
        Some("data") => 2,
        _ => 3,
    }
}

fn band(s: Option<&str>) -> u8 {
    match s.map(|v| v.trim()) {
        Some("Ka") | Some("KA") | Some("ka") => b'K',
        Some(v) if !v.is_empty() => v.as_bytes()[0].to_ascii_uppercase(),
        _ => 0,
    }
}

/// Reads a dsn.xml report. False when the text holds no `<dsn>` element; what does not fit the fixed tables is left out.
pub fn parse(xml: &[u8], out: &mut Feed) -> bool {
    *out = Feed::empty();
    let Ok(s) = core::str::from_utf8(xml) else { return false };
    let mut seen_root = false;
    let mut cur_dish: i32 = -1;
    for t in Tags::new(s) {
        if t.closing {
            if t.name == "dish" {
                cur_dish = -1;
            }
            continue;
        }
        let a = t.attrs;
        match t.name {
            "dsn" => seen_root = true,
            "station" => {
                if (out.n_stations as usize) < MAX_STATIONS {
                    let st = &mut out.stations[out.n_stations as usize];
                    put(&mut st.name, attr(a, "name").unwrap_or(""));
                    put(&mut st.friendly, attr(a, "friendlyName").unwrap_or(""));
                    st.time_utc_ms = int(attr(a, "timeUTC"));
                    st.tz_offset_ms = int(attr(a, "timeZoneOffset"));
                    out.n_stations += 1;
                }
            }
            "dish" => {
                cur_dish = -1;
                if (out.n_dishes as usize) < MAX_DISHES {
                    let d = &mut out.dishes[out.n_dishes as usize];
                    put(&mut d.name, attr(a, "name").unwrap_or(""));
                    put(&mut d.activity, attr(a, "activity").unwrap_or(""));
                    d.azimuth_deg = num(attr(a, "azimuthAngle"));
                    d.elevation_deg = num(attr(a, "elevationAngle"));
                    d.wind_kmh = num(attr(a, "windSpeed"));
                    d.station = out.n_stations as i32 - 1;
                    d.mspa = yes(attr(a, "isMSPA"));
                    d.array = yes(attr(a, "isArray"));
                    d.ddor = yes(attr(a, "isDDOR"));
                    cur_dish = out.n_dishes as i32;
                    out.n_dishes += 1;
                }
                if t.self_closing {
                    cur_dish = -1;
                }
            }
            "target" if cur_dish >= 0 && (out.n_targets as usize) < MAX_TARGETS => {
                let g = &mut out.targets[out.n_targets as usize];
                put(&mut g.name, attr(a, "name").unwrap_or(""));
                g.dish = cur_dish;
                g.id = int(attr(a, "id")) as i32;
                g.upleg_km = known(num(attr(a, "uplegRange")));
                g.downleg_km = known(num(attr(a, "downlegRange")));
                g.rtlt_s = known(num(attr(a, "rtlt")));
                out.n_targets += 1;
            }
            "upSignal" | "downSignal" if cur_dish >= 0 && (out.n_signals as usize) < MAX_SIGNALS => {
                let up = t.name == "upSignal";
                let g = &mut out.signals[out.n_signals as usize];
                put(&mut g.spacecraft, attr(a, "spacecraft").unwrap_or(""));
                g.dish = cur_dish;
                g.spacecraft_id = int(attr(a, "spacecraftID")) as i32;
                g.up = up as u8;
                g.active = yes(attr(a, "active"));
                g.kind = signal_kind(attr(a, "signalType"));
                g.band = band(attr(a, "band"));
                let r = num(attr(a, "dataRate"));
                g.data_rate_bps = if r.is_finite() && r > 0.0 { r } else { 0.0 };
                g.frequency_hz = frequency_hz(num(attr(a, "frequency")));
                let p = num(attr(a, "power"));
                if up {
                    g.power = p;
                } else {
                    let (dbm, flagged) = downlink_dbm(p);
                    g.power = dbm;
                    g.power_flagged = flagged as u8;
                }
                out.n_signals += 1;
            }
            "timestamp" => out.timestamp_ms = int(Some(t.text)),
            _ => {}
        }
    }
    if out.timestamp_ms == 0 && out.n_stations > 0 {
        out.timestamp_ms = out.stations[0].time_utc_ms;
    }
    seen_root
}

/// Reads config.xml. False when it holds no `<config>` element.
pub fn parse_config(xml: &[u8], out: &mut Config) -> bool {
    *out = Config::empty();
    let Ok(s) = core::str::from_utf8(xml) else { return false };
    let mut seen_root = false;
    let mut cur_site: i32 = -1;
    for t in Tags::new(s) {
        if t.closing {
            if t.name == "site" {
                cur_site = -1;
            }
            continue;
        }
        let a = t.attrs;
        match t.name {
            "config" => seen_root = true,
            "site" if (out.n_sites as usize) < MAX_SITES => {
                let st = &mut out.sites[out.n_sites as usize];
                put(&mut st.name, attr(a, "name").unwrap_or(""));
                put(&mut st.friendly, attr(a, "friendlyName").unwrap_or(""));
                st.latitude = num(attr(a, "latitude"));
                st.longitude = num(attr(a, "longitude"));
                cur_site = out.n_sites as i32;
                out.n_sites += 1;
            }
            "dish" if (out.n_dishes as usize) < MAX_DISHES => {
                let d = &mut out.dishes[out.n_dishes as usize];
                put(&mut d.name, attr(a, "name").unwrap_or(""));
                put(&mut d.kind, attr(a, "type").unwrap_or(""));
                d.site = cur_site;
                out.n_dishes += 1;
            }
            "spacecraft" if (out.n_spacecraft as usize) < MAX_SPACECRAFT => {
                let c = &mut out.spacecraft[out.n_spacecraft as usize];
                put(&mut c.name, attr(a, "name").unwrap_or(""));
                put(&mut c.friendly, attr(a, "friendlyName").unwrap_or(""));
                out.n_spacecraft += 1;
            }
            _ => {}
        }
    }
    seen_root
}

/// The full name of a spacecraft call sign (case does not matter), if the config lists it.
pub fn spacecraft_name<'a>(cfg: &'a Config, call: &str) -> Option<&'a str> {
    let call = call.trim();
    cfg.spacecraft[..(cfg.n_spacecraft as usize).min(MAX_SPACECRAFT)]
        .iter()
        .find(|c| text(&c.name).eq_ignore_ascii_case(call))
        .map(|c| text(&c.friendly))
        .filter(|s| !s.is_empty())
}

/// The dish type (70M, 34M ...) and site index of a dish name.
pub fn dish_info<'a>(cfg: &'a Config, dish: &str) -> Option<&'a DishInfo> {
    cfg.dishes[..(cfg.n_dishes as usize).min(MAX_DISHES)].iter().find(|d| text(&d.name).eq_ignore_ascii_case(dish.trim()))
}

/// One-way light time in seconds for a range in km.
pub fn light_time_s(km: f64) -> f64 {
    km / C_KM_S
}
