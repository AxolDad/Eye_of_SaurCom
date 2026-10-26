use crate::xml::{attr, Tags};
use crate::*;

// a report and the configuration as eyes.nasa.gov served them on 2026-10-23 (all frequencies were published as 0 that day)
const SAMPLE: &[u8] = include_bytes!("../testdata/dsn_sample.xml");
const CONFIG: &[u8] = include_bytes!("../testdata/config_sample.xml");

fn feed(xml: &[u8]) -> Box<Feed> {
    let mut f = Box::new(Feed::empty());
    assert!(parse(xml, &mut f));
    f
}

#[test]
fn xml_tags_and_attributes() {
    let s = "<?xml version='1.0'?><!-- a > comment --><a x=\"1>2\" y='q'><b/></a><t>123</t>";
    let v: Vec<_> = Tags::new(s).map(|t| (t.name, t.closing, t.self_closing)).collect();
    assert_eq!(v, vec![("a", false, false), ("b", false, true), ("a", true, false), ("t", false, false), ("t", true, false)]);
    let a = Tags::new(s).next().unwrap();
    assert_eq!(attr(a.attrs, "x"), Some("1>2"));
    assert_eq!(attr(a.attrs, "y"), Some("q"));
    assert_eq!(attr(a.attrs, "z"), None);
    assert_eq!(Tags::new(s).find(|t| t.name == "t").unwrap().text, "123");
    assert_eq!(attr(" flag name=\"DSS14\"", "name"), Some("DSS14"));
}

#[test]
fn recorded_report() {
    let f = feed(SAMPLE);
    assert_eq!(f.n_stations, 3);
    assert_eq!(text(&f.stations[0].name), "gdscc");
    assert_eq!(text(&f.stations[1].friendly), "Madrid");
    assert_eq!(text(&f.stations[2].friendly), "Canberra");
    assert_eq!(f.stations[0].tz_offset_ms, -25_200_000);
    assert_eq!(f.timestamp_ms, 1_792_799_506_000);
    assert_eq!(f.n_dishes, 14);
    assert_eq!(f.n_targets, 15);
    assert_eq!(f.n_signals, 28);
    // DSS14 is under upgrade and points at the placeholder target DSN with unknown ranges
    let d = &f.dishes[0];
    assert_eq!(text(&d.name), "DSS14");
    assert_eq!(text(&d.activity), "Engineering Upgrades");
    assert_eq!((d.azimuth_deg, d.elevation_deg, d.station), (0.0, 90.0, 0));
    assert!(d.wind_kmh.is_nan(), "empty windSpeed");
    let t = &f.targets[0];
    assert_eq!((text(&t.name), t.dish, t.id), ("DSN", 0, 99));
    assert!(t.upleg_km.is_nan() && t.downleg_km.is_nan() && t.rtlt_s.is_nan());
    // OSIRIS-APEX on DSS23: known range, unknown light time
    let orx = f.targets.iter().take(f.n_targets as usize).find(|t| text(&t.name) == "ORX").unwrap();
    assert_eq!(orx.upleg_km, 91_400_000.0);
    assert!(orx.rtlt_s.is_nan());
    // every dish after the Madrid station line belongs to Madrid, and so on
    for d in f.dishes.iter().take(f.n_dishes as usize) {
        let st = text(&f.stations[d.station as usize].name);
        let n: u32 = text(&d.name)[3..].parse().unwrap();
        let want = match n {
            10..=29 => "gdscc",
            50..=69 => "mdscc",
            30..=49 => "cdscc",
            _ => unreachable!(),
        };
        // DSS14 sits in 10..29 (Goldstone), DSS43 in 30..49 (Canberra)
        assert_eq!(st, want, "{}", text(&d.name));
    }
    // signals: New Horizons downlink at 49.99 bit/s on X band
    let nh = f.signals.iter().take(f.n_signals as usize).find(|s| text(&s.spacecraft) == "NHPC" && s.up == 0).unwrap();
    assert_eq!((nh.active, nh.kind, nh.band, nh.data_rate_bps), (1, 2, b'X', 49.99));
    assert_eq!((nh.power, nh.power_flagged, nh.frequency_hz), (-160.0, 0, 0.0));
    let lro = f.signals.iter().take(f.n_signals as usize).find(|s| text(&s.spacecraft) == "LRO" && s.up == 0).unwrap();
    assert_eq!((lro.band, lro.data_rate_bps, lro.spacecraft_id), (b'S', 146_400.0, -85));
    // an inactive downlink with the -480 dBm placeholder: flagged and corrected to -180
    let ace = f.signals.iter().take(f.n_signals as usize).find(|s| text(&s.spacecraft) == "ACE" && s.up == 0).unwrap();
    assert_eq!((ace.active, ace.kind, ace.power, ace.power_flagged), (0, 0, -180.0, 1));
    let up = f.signals.iter().take(f.n_signals as usize).find(|s| text(&s.spacecraft) == "MMS3" && s.up == 1).unwrap();
    assert_eq!((up.power, up.band), (0.2, b'S'));
}

#[test]
fn units_and_flags() {
    assert_eq!(frequency_hz(0.0), 0.0);
    assert_eq!(frequency_hz(-1.0), 0.0);
    assert_eq!(frequency_hz(8.4), 8.4e9);
    assert_eq!(frequency_hz(2290.5), 2.2905e9);
    assert_eq!(frequency_hz(8_420_432_000.0), 8_420_432_000.0);
    assert_eq!(downlink_dbm(-410.5), (-110.5, true));
    assert_eq!(downlink_dbm(-110.5), (-110.5, false));
    assert_eq!(signal_kind(Some("carrier")), 1);
    assert_eq!(signal_kind(Some("data")), 2);
    assert_eq!(signal_kind(Some("none")), 0);
    assert_eq!(signal_kind(Some("ranging")), 3);
    assert!((light_time_s(KM_PER_AU) - 499.004_783_8).abs() < 1e-3);
}

#[test]
fn signal_details() {
    let x = br#"<dsn><station name="mdscc" friendlyName="Madrid &amp; Robledo" timeUTC="1" timeZoneOffset="2"/>
        <dish name="DSS63" azimuthAngle="101.5" elevationAngle="22.25" windSpeed="12.5" isMSPA="true" isArray="true" isDDOR="false" activity="Tracking &lt;VGR1&gt;">
        <downSignal active="true" signalType="carrier" dataRate="" frequency="8420432097.5" band="X" power="-153.2" spacecraft="VGR1" spacecraftID="-31"/>
        <upSignal active="true" signalType="data" dataRate="16" frequency="7174.12" band="X" power="18.4" spacecraft="VGR1" spacecraftID="-31"/>
        <target name="VGR1" id="31" uplegRange="25300000000" downlegRange="25300000000" rtlt="168800"/>
        </dish><dish name="DSS65" activity=""/><target name="LOOSE" id="1" uplegRange="1" downlegRange="1" rtlt="1"/>
        <timestamp>1792800000000</timestamp></dsn>"#;
    let f = feed(x);
    assert_eq!(text(&f.stations[0].friendly), "Madrid & Robledo");
    let d = &f.dishes[0];
    assert_eq!(text(&d.activity), "Tracking <VGR1>");
    assert_eq!((d.mspa, d.array, d.ddor, d.wind_kmh), (1, 1, 0, 12.5));
    assert_eq!((d.azimuth_deg, d.elevation_deg), (101.5, 22.25));
    let down = &f.signals[0];
    assert_eq!((down.up, down.kind, down.frequency_hz, down.data_rate_bps), (0, 1, 8_420_432_097.5, 0.0));
    let up = &f.signals[1];
    assert_eq!((up.up, up.kind, up.data_rate_bps, up.power), (1, 2, 16.0, 18.4));
    assert!((up.frequency_hz - 7.17412e9).abs() < 1.0);
    let t = &f.targets[0];
    assert_eq!((t.upleg_km, t.rtlt_s), (25_300_000_000.0, 168_800.0));
    // a target outside any dish is not kept; a self-closing dish takes no children
    assert_eq!(f.n_targets, 1);
    assert_eq!(f.n_dishes, 2);
    assert_eq!(f.timestamp_ms, 1_792_800_000_000);
}

#[test]
fn not_a_report() {
    let mut f = Box::new(Feed::empty());
    assert!(!parse(b"<html><body>DSN Now</body></html>", &mut f));
    assert!(!parse(&[0xff, 0xfe, 0x00], &mut f));
    assert!(!parse(b"", &mut f));
    let mut c = Box::new(Config::empty());
    assert!(!parse_config(SAMPLE, &mut c));
}

#[test]
fn full_tables_stop_cleanly() {
    let mut s = String::from("<dsn><station name=\"g\"/>");
    for i in 0..40 {
        s.push_str(&format!("<dish name=\"DSS{i}\">"));
        for _ in 0..6 {
            s.push_str("<upSignal active=\"true\" spacecraft=\"X\"/><target name=\"X\" id=\"1\"/>");
        }
        s.push_str("</dish>");
    }
    s.push_str("</dsn>");
    let f = feed(s.as_bytes());
    assert_eq!(f.n_dishes as usize, MAX_DISHES);
    assert_eq!(f.n_signals as usize, MAX_SIGNALS);
    assert_eq!(f.n_targets as usize, MAX_TARGETS);
}

#[test]
fn recorded_config() {
    let mut c = Box::new(Config::empty());
    assert!(parse_config(CONFIG, &mut c));
    assert_eq!(c.n_sites, 3);
    assert_eq!(text(&c.sites[0].friendly), "Madrid");
    assert!((c.sites[1].latitude - 35.2443523).abs() < 1e-9 && (c.sites[1].longitude + 116.8895382).abs() < 1e-9);
    assert_eq!(c.n_dishes, 15);
    let d = dish_info(&c, "dss43").unwrap();
    assert_eq!((text(&d.kind), text(&c.sites[d.site as usize].friendly)), ("70M", "Canberra"));
    assert!(c.n_spacecraft >= 150);
    assert_eq!(spacecraft_name(&c, "ACE"), Some("Advanced Composition Explorer"));
    assert_eq!(spacecraft_name(&c, "nope"), None);
}

#[test]
fn ffi_round_trip() {
    use crate::ffi::*;
    assert_eq!(zorcom_dsn_version(), 1);
    assert_eq!(zorcom_dsn_feed_size(), core::mem::size_of::<Feed>());
    let mut f = Box::new(Feed::empty());
    let mut c = Box::new(Config::empty());
    #[allow(unsafe_code)]
    unsafe {
        assert!(zorcom_dsn_parse(SAMPLE.as_ptr(), SAMPLE.len(), &mut *f));
        assert!(zorcom_dsn_parse_config(CONFIG.as_ptr(), CONFIG.len(), &mut *c));
        assert!(!zorcom_dsn_parse(core::ptr::null(), 0, &mut *f));
        let mut name = [0u8; 8];
        let n = zorcom_dsn_spacecraft_name(&*c, b"ace".as_ptr(), 3, name.as_mut_ptr(), name.len());
        assert_eq!((n, text(&name)), (7, "Advance"));
        assert_eq!(zorcom_dsn_dish_index(&*c, b"DSS63".as_ptr(), 5), 0);
        assert_eq!(zorcom_dsn_dish_index(&*c, b"DSS99".as_ptr(), 5), -1);
    }
}
