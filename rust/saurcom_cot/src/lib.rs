//! Cursor on Target (CoT), the position format of ATAK, WinTAK, iTAK and TAK servers: one `<event>` XML document per contact.
//! [`encode`] writes an event for a tracked contact (aircraft, vessel, station, balloon, mesh node); [`decode`] reads the uid, type,
//! position, times and callsign of a received event. No allocation and no panics: output goes into caller buffers. Only `ffi` uses `unsafe`.
#![cfg_attr(not(test), no_std)]
#![deny(unsafe_code)]

#[allow(unsafe_code)]
pub mod ffi;
pub mod time;

#[cfg(test)]
mod tests;

use core::fmt::Write;

/// CoT's value for "unknown" in `hae`, `ce` and `le`.
pub const UNKNOWN: f64 = 9_999_999.0;

/// What a contact is, for its CoT type. All are "n" (neutral): a receiver cannot tell friend from foe.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Aircraft,
    Vessel,
    Station,
    Balloon,
    MeshNode,
}

impl Kind {
    pub fn from_u32(k: u32) -> Option<Kind> {
        Some(match k {
            0 => Kind::Aircraft,
            1 => Kind::Vessel,
            2 => Kind::Station,
            3 => Kind::Balloon,
            4 => Kind::MeshNode,
            _ => return None,
        })
    }
    /// The 2525-style CoT type: atom, neutral, then the battle dimension (A air, S sea surface, G ground) and function.
    pub fn cot_type(self) -> &'static str {
        match self {
            Kind::Aircraft => "a-n-A-C-F",
            Kind::Vessel => "a-n-S-C",
            Kind::Station => "a-n-G",
            Kind::Balloon => "a-n-A-C-L",
            Kind::MeshNode => "a-n-G-U-C",
        }
    }
}

/// A contact to send. Unknown values: `hae` NaN, `course_deg` or `speed_mps` negative or NaN.
pub struct Event<'a> {
    pub uid: &'a str,
    pub cot_type: &'a str,
    pub callsign: &'a str,
    pub remarks: &'a str,
    pub lat: f64,
    pub lon: f64,
    pub hae: f64,
    pub course_deg: f64,
    pub speed_mps: f64,
    pub time_ms: i64,
    pub stale_sec: i32,
}

/// Writes into a byte slice; `fmt::Error` once it is full (the event is then not sent rather than sent cut).
pub struct Buf<'a> {
    out: &'a mut [u8],
    pub len: usize,
}

impl<'a> Buf<'a> {
    pub fn new(out: &'a mut [u8]) -> Self {
        Buf { out, len: 0 }
    }
}

impl Write for Buf<'_> {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let end = self.len.checked_add(s.len()).ok_or(core::fmt::Error)?;
        let dst = self.out.get_mut(self.len..end).ok_or(core::fmt::Error)?;
        dst.copy_from_slice(s.as_bytes());
        self.len = end;
        Ok(())
    }
}

/// XML attribute or text escaping of `s`; control characters (not allowed in XML 1.0) are dropped.
fn escape(w: &mut Buf, s: &str) -> core::fmt::Result {
    for c in s.chars() {
        match c {
            '&' => w.write_str("&amp;")?,
            '<' => w.write_str("&lt;")?,
            '>' => w.write_str("&gt;")?,
            '"' => w.write_str("&quot;")?,
            '\'' => w.write_str("&apos;")?,
            c if (c as u32) < 0x20 && c != '\t' && c != '\n' && c != '\r' => {}
            c => w.write_char(c)?,
        }
    }
    Ok(())
}

fn known(v: f64) -> bool {
    v.is_finite() && v >= 0.0
}

/// The event XML of `e` into `out`; the length written, or None when `out` is too small or a coordinate is out of range.
pub fn encode(e: &Event, out: &mut [u8]) -> Option<usize> {
    if !(e.lat.is_finite() && e.lon.is_finite() && (-90.0..=90.0).contains(&e.lat) && (-180.0..=180.0).contains(&e.lon)) || e.uid.is_empty() {
        return None;
    }
    let mut w = Buf::new(out);
    write_event(&mut w, e).ok()?;
    Some(w.len)
}

fn write_event(w: &mut Buf, e: &Event) -> core::fmt::Result {
    let stale_ms = e.time_ms.saturating_add(i64::from(e.stale_sec.max(1)) * 1000);
    w.write_str("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><event version=\"2.0\" uid=\"")?;
    escape(w, e.uid)?;
    w.write_str("\" type=\"")?;
    escape(w, e.cot_type)?;
    w.write_str("\" how=\"m-g\" time=\"")?;
    time::write_iso(w, e.time_ms)?;
    w.write_str("\" start=\"")?;
    time::write_iso(w, e.time_ms)?;
    w.write_str("\" stale=\"")?;
    time::write_iso(w, stale_ms)?;
    let hae = if e.hae.is_finite() { e.hae } else { UNKNOWN };
    write!(w, "\"><point lat=\"{:.6}\" lon=\"{:.6}\" hae=\"{:.1}\" ce=\"{:.1}\" le=\"{:.1}\"/><detail>", e.lat, e.lon, hae, UNKNOWN, UNKNOWN)?;
    if !e.callsign.is_empty() {
        w.write_str("<contact callsign=\"")?;
        escape(w, e.callsign)?;
        w.write_str("\"/>")?;
    }
    if known(e.course_deg) || known(e.speed_mps) {
        let course = if known(e.course_deg) { e.course_deg % 360.0 } else { 0.0 };
        let speed = if known(e.speed_mps) { e.speed_mps } else { 0.0 };
        write!(w, "<track course=\"{:.1}\" speed=\"{:.2}\"/>", course, speed)?;
    }
    if !e.remarks.is_empty() {
        w.write_str("<remarks>")?;
        escape(w, e.remarks)?;
        w.write_str("</remarks>")?;
    }
    w.write_str("</detail></event>")
}

/// A fixed-size UTF-8 text taken from a received event (cut at a character boundary when longer).
#[derive(Clone, Copy)]
pub struct Text<const N: usize> {
    pub bytes: [u8; N],
    pub len: usize,
}

impl<const N: usize> Default for Text<N> {
    fn default() -> Self {
        Text { bytes: [0; N], len: 0 }
    }
}

impl<const N: usize> Text<N> {
    pub fn as_str(&self) -> &str {
        core::str::from_utf8(self.bytes.get(..self.len).unwrap_or(&[])).unwrap_or("")
    }
    fn push(&mut self, c: char) {
        let mut tmp = [0u8; 4];
        let s = c.encode_utf8(&mut tmp).as_bytes();
        if self.len + s.len() < N {   // keeps one byte for a C terminator
            if let Some(d) = self.bytes.get_mut(self.len..self.len + s.len()) {
                d.copy_from_slice(s);
                self.len += s.len();
            }
        }
    }
}

/// What [`decode`] reads from a received event.
#[derive(Clone, Copy, Default)]
pub struct Received {
    pub uid: Text<96>,
    pub cot_type: Text<48>,
    pub callsign: Text<64>,
    pub lat: f64,
    pub lon: f64,
    pub hae: f64,
    pub time_ms: i64,
    pub stale_ms: i64,
}

/// The start of the tag `<name` followed by whitespace, `/` or `>` (so `<event` does not match `<events`).
fn find_tag<'a>(doc: &'a str, name: &str) -> Option<&'a str> {
    let mut from = 0;
    while let Some(i) = doc.get(from..)?.find('<') {
        let at = from + i + 1;
        let rest = doc.get(at..)?;
        if let Some(after) = rest.strip_prefix(name) {
            if after.starts_with(|c: char| c.is_ascii_whitespace() || c == '/' || c == '>') {
                let end = after.find('>').map(|e| e + 1).unwrap_or(after.len());
                return after.get(..end);
            }
        }
        from = at;
    }
    None
}

/// The raw (still escaped) value of attribute `name` in the attribute text of one tag.
fn attr<'a>(tag: &'a str, name: &str) -> Option<&'a str> {
    let mut from = 0;
    while let Some(i) = tag.get(from..)?.find(name) {
        let start = from + i;
        let before_ok = start == 0 || tag.as_bytes().get(start - 1).is_some_and(|b| b.is_ascii_whitespace());
        let rest = tag.get(start + name.len()..)?.trim_start();
        if before_ok {
            if let Some(v) = rest.strip_prefix('=') {
                let v = v.trim_start();
                let q = v.chars().next()?;
                if q == '"' || q == '\'' {
                    let body = v.get(1..)?;
                    return body.find(q).and_then(|e| body.get(..e));
                }
            }
        }
        from = start + name.len();
    }
    None
}

fn unescape_into<const N: usize>(s: &str, out: &mut Text<N>) {
    let mut rest = s;
    while let Some(c) = rest.chars().next() {
        if c == '&' {
            let ent = [("&amp;", '&'), ("&lt;", '<'), ("&gt;", '>'), ("&quot;", '"'), ("&apos;", '\'')];
            if let Some((e, ch)) = ent.iter().find(|(e, _)| rest.starts_with(e)) {
                out.push(*ch);
                rest = rest.get(e.len()..).unwrap_or("");
                continue;
            }
        }
        out.push(c);
        rest = rest.get(c.len_utf8()..).unwrap_or("");
    }
}

fn num(tag: &str, name: &str) -> Option<f64> {
    attr(tag, name)?.trim().parse::<f64>().ok().filter(|v| v.is_finite())
}

/// The contact in a received CoT event: None unless it has a uid, a type and a point with a valid latitude and longitude.
pub fn decode(xml: &[u8]) -> Option<Received> {
    let doc = core::str::from_utf8(xml).ok()?;
    let ev = find_tag(doc, "event")?;
    let pt = find_tag(doc, "point")?;
    let mut r = Received::default();
    unescape_into(attr(ev, "uid")?, &mut r.uid);
    unescape_into(attr(ev, "type")?, &mut r.cot_type);
    if r.uid.len == 0 || r.cot_type.len == 0 {
        return None;
    }
    r.lat = num(pt, "lat")?;
    r.lon = num(pt, "lon")?;
    if !(-90.0..=90.0).contains(&r.lat) || !(-180.0..=180.0).contains(&r.lon) {
        return None;
    }
    r.hae = num(pt, "hae").filter(|h| *h < UNKNOWN).unwrap_or(f64::NAN);
    r.time_ms = attr(ev, "time").and_then(time::parse_iso).unwrap_or(0);
    r.stale_ms = attr(ev, "stale").and_then(time::parse_iso).unwrap_or(0);
    if let Some(cs) = find_tag(doc, "contact").and_then(|t| attr(t, "callsign")) {
        unescape_into(cs, &mut r.callsign);
    }
    Some(r)
}
