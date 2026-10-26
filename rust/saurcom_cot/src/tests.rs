use crate::time::{parse_iso, write_iso};
use crate::*;

fn ev<'a>(uid: &'a str, callsign: &'a str, remarks: &'a str) -> Event<'a> {
    Event {
        uid,
        cot_type: Kind::Aircraft.cot_type(),
        callsign,
        remarks,
        lat: 54.687_157,
        lon: 25.279_652,
        hae: 1200.0,
        course_deg: 271.5,
        speed_mps: 120.25,
        time_ms: 1_792_758_896_789, // 2026-10-23T12:34:56.789Z
        stale_sec: 30,
    }
}

fn enc(e: &Event) -> std::string::String {
    let mut out = [0u8; 2048];
    let n = encode(e, &mut out).expect("fits");
    std::string::String::from_utf8(out[..n].to_vec()).unwrap()
}

#[test]
fn time_round_trip_and_known_values() {
    let mut b = [0u8; 32];
    let mut w = Buf::new(&mut b);
    write_iso(&mut w, 0).unwrap();
    let n = w.len;
    assert_eq!(&b[..n], b"1970-01-01T00:00:00.000Z");
    let mut b = [0u8; 32];
    let mut w = Buf::new(&mut b);
    write_iso(&mut w, 1_792_758_896_789).unwrap();
    let n = w.len;
    assert_eq!(&b[..n], b"2026-10-23T12:34:56.789Z");
    assert_eq!(parse_iso("2026-10-23T12:34:56.789Z"), Some(1_792_758_896_789));
    assert_eq!(parse_iso("2026-10-23T12:34:56Z"), Some(1_792_758_896_000));
    assert_eq!(parse_iso("2026-10-23T12:34:56.5Z"), Some(1_792_758_896_500));
    assert_eq!(parse_iso("2024-02-29T00:00:00.000Z"), Some(1_709_164_800_000)); // leap day
    for bad in ["", "2026-10-23T12:34:56", "2026-13-01T00:00:00Z", "2026-10-23 12:34:56Z", "x026-10-23T12:34:56Z", "2026-10-23T24:00:00Z"] {
        assert_eq!(parse_iso(bad), None, "{bad}");
    }
}

#[test]
fn encode_has_the_fields_atak_reads() {
    let s = enc(&ev("ICAO-4CA2B1", "RYR12AB", ""));
    assert!(s.starts_with("<?xml"));
    assert!(s.contains("uid=\"ICAO-4CA2B1\"") && s.contains("type=\"a-n-A-C-F\"") && s.contains("how=\"m-g\""));
    assert!(s.contains("time=\"2026-10-23T12:34:56.789Z\"") && s.contains("stale=\"2026-10-23T12:35:26.789Z\""));
    assert!(s.contains("lat=\"54.687157\" lon=\"25.279652\" hae=\"1200.0\""));
    assert!(s.contains("<contact callsign=\"RYR12AB\"/>") && s.contains("<track course=\"271.5\" speed=\"120.25\"/>"));
    assert!(!s.contains("<remarks>"), "no empty remarks element");
    assert!(s.ends_with("</detail></event>"));
}

#[test]
fn round_trip() {
    let s = enc(&ev("MMSI-277123456", "SAURCOM & <SONS>", "a \"quoted\" note"));
    let r = decode(s.as_bytes()).expect("decodes");
    assert_eq!(r.uid.as_str(), "MMSI-277123456");
    assert_eq!(r.cot_type.as_str(), "a-n-A-C-F");
    assert_eq!(r.callsign.as_str(), "SAURCOM & <SONS>");
    assert!((r.lat - 54.687_157).abs() < 1e-9 && (r.lon - 25.279_652).abs() < 1e-9 && (r.hae - 1200.0).abs() < 1e-9);
    assert_eq!(r.time_ms, 1_792_758_896_789);
    assert_eq!(r.stale_ms, 1_792_758_926_789);
}

#[test]
fn escaping_keeps_the_document_well_formed() {
    let s = enc(&ev("a\"b<c>&d'e", "x\u{1}y", "<script>"));
    assert!(s.contains("uid=\"a&quot;b&lt;c&gt;&amp;d&apos;e\""));
    assert!(s.contains("callsign=\"xy\""), "control characters dropped");
    assert!(s.contains("<remarks>&lt;script&gt;</remarks>"));
    assert_eq!(decode(s.as_bytes()).unwrap().uid.as_str(), "a\"b<c>&d'e");
}

#[test]
fn unknown_values_use_the_cot_conventions() {
    let mut e = ev("X", "", "");
    e.hae = f64::NAN;
    e.course_deg = -1.0;
    e.speed_mps = f64::NAN;
    let s = enc(&e);
    assert!(s.contains("hae=\"9999999.0\" ce=\"9999999.0\" le=\"9999999.0\""));
    assert!(!s.contains("<track") && !s.contains("<contact"));
    assert!(decode(s.as_bytes()).unwrap().hae.is_nan(), "unknown hae reads back as NaN");
}

#[test]
fn refuses_bad_input_without_panicking() {
    let mut small = [0u8; 64];
    assert_eq!(encode(&ev("X", "", ""), &mut small), None, "too small: nothing rather than a cut document");
    let mut out = [0u8; 2048];
    let mut e = ev("X", "", "");
    e.lat = 91.0;
    assert_eq!(encode(&e, &mut out), None);
    let mut e = ev("", "", "");
    e.lat = 1.0;
    assert_eq!(encode(&e, &mut out), None, "a uid is required");
    for bad in [&b""[..], b"<event", b"<event uid=\"a\" type=\"b\"/>", b"<event uid=\"a\" type=\"b\"><point lat=\"x\" lon=\"1\"/></event>",
                b"<event uid=\"a\" type=\"b\"><point lat=\"95\" lon=\"1\"/></event>", b"<events uid=\"a\" type=\"b\"><point lat=\"1\" lon=\"1\"/></events>", &[0xff, 0xfe]] {
        assert!(decode(bad).is_none());
    }
}

#[test]
fn hostile_years_are_rejected_without_overflow() {
    assert_eq!(parse_iso("2026-10-23T12:00:00Z"), Some(1_792_756_800_000));
    for bad in ["9999999999999999-01-01T00:00:00Z", "12345-01-01T00:00:00Z", "0000-01-01T00:00:00Z", "999-01-01T00:00:00Z"] {
        assert_eq!(parse_iso(bad), None, "{bad}");
    }
    let x = |stale: &str| format!("<event uid='a' type='b' time='2026-10-23T12:00:00Z' stale='{stale}'><point lat='1' lon='1'/></event>");
    assert_eq!(decode(x("2026-10-23T12:00:00Z").as_bytes()).map(|r| r.stale_ms), Some(1_792_756_800_000));
    for bad in ["9999999999999999-01-01T00:00:00Z", "12345-01-01T00:00:00Z"] {
        assert_eq!(decode(x(bad).as_bytes()).map(|r| r.stale_ms), Some(0), "{bad}"); // no stale time, no panic
    }
}

#[test]
fn decodes_an_atak_style_event() {
    let x = b"<?xml version='1.0' encoding='UTF-8' standalone='yes'?><event version='2.0' uid='ANDROID-589520ccfcd20f01' type='a-f-G-U-C' time='2026-10-23T12:00:00.123Z' start='2026-10-23T12:00:00.123Z' stale='2026-10-23T12:06:00.123Z' how='h-e'><point lat='38.8977' lon='-77.0365' hae='17.5' ce='9999999.0' le='9999999.0'/><detail><takv os='34' version='4.10' device='PIXEL' platform='ATAK-CIV'/><contact endpoint='*:-1:stcp' callsign='ZOR-1'/><__group role='Team Member' name='Cyan'/></detail></event>";
    let r = decode(x).expect("decodes");
    assert_eq!(r.uid.as_str(), "ANDROID-589520ccfcd20f01");
    assert_eq!(r.cot_type.as_str(), "a-f-G-U-C");
    assert_eq!(r.callsign.as_str(), "ZOR-1");
    assert!((r.lat - 38.8977).abs() < 1e-9 && (r.lon + 77.0365).abs() < 1e-9 && (r.hae - 17.5).abs() < 1e-9);
    assert!(r.stale_ms - r.time_ms == 360_000);
}

#[test]
fn every_kind_has_a_type() {
    for k in 0..5 {
        assert!(Kind::from_u32(k).unwrap().cot_type().starts_with("a-n-"));
    }
    assert!(Kind::from_u32(5).is_none());
}
