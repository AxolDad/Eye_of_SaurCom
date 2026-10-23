use crate::frame::{parse, MAGIC};
use crate::sensor::Sensor;
use std::vec::Vec;

const FS: f64 = 50.0;
const NSUB: usize = 52;

/// One ADR-018 frame: per subcarrier a complex value of amplitude `amp(k)` (int8 I/Q), phase drifting slowly across the band.
fn frame(seq: u32, amp: impl Fn(usize) -> f64) -> Vec<u8> {
    let mut b = Vec::new();
    b.extend_from_slice(&MAGIC.to_le_bytes());
    b.push(7); // node
    b.push(1); // antennas
    b.extend_from_slice(&(NSUB as u16).to_le_bytes());
    b.extend_from_slice(&2437u32.to_le_bytes());
    b.extend_from_slice(&seq.to_le_bytes());
    b.push((-45i8) as u8);
    b.push((-92i8) as u8);
    b.push(0);
    b.push(0);
    for k in 0..NSUB {
        let ph = 0.3 + 0.05 * k as f64;
        let a = amp(k);
        b.push((a * ph.cos()).round() as i8 as u8);
        b.push((a * ph.sin()).round() as i8 as u8);
    }
    b
}

/// A cheap reproducible noise source in -1..1
struct Lcg(u64);
impl Lcg {
    fn next(&mut self) -> f64 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        ((self.0 >> 11) as f64 / (1u64 << 53) as f64) * 2.0 - 1.0
    }
}

#[test]
fn parses_the_firmware_frame() {
    let b = frame(42, |_| 60.0);
    let f = parse(&b).expect("parses");
    assert_eq!((f.node_id, f.n_antennas, f.n_subcarriers, f.freq_mhz, f.seq, f.rssi, f.noise), (7, 1, NSUB, 2437, 42, -45, -92));
    assert!(f.amplitudes.iter().all(|a| (a - 60.0).abs() < 1.0), "amplitudes ~60");
    assert!((f.phases[0] - 0.3).abs() < 0.05);
}

#[test]
fn refuses_malformed_frames() {
    let good = frame(1, |_| 60.0);
    assert!(parse(&good[..good.len() - 1]).is_none(), "truncated I/Q");
    let mut bad = good.clone();
    bad[0] ^= 1;
    assert!(parse(&bad).is_none(), "magic");
    let mut zero = good.clone();
    zero[6] = 0;
    zero[7] = 0;
    assert!(parse(&zero).is_none(), "no subcarriers");
    let mut many = good.clone();
    many[5] = 9;
    assert!(parse(&many).is_none(), "too many antennas");
    assert!(parse(&[]).is_none() && parse(&good[..10]).is_none());
}

#[test]
fn breathing_and_heart_rate_of_a_still_person() {
    // 18 breaths and 72 beats a minute modulate the channel a little (int8 I/Q, a little noise), 40 s at 50 frames per second
    let mut s = Sensor::new(FS);
    let mut rng = Lcg(1);
    let mut last = None;
    for i in 0..(40.0 * FS) as u32 {
        let t = f64::from(i) / FS;
        let b = 6.0 * (2.0 * std::f64::consts::PI * 0.30 * t).sin();
        let h = 1.5 * (2.0 * std::f64::consts::PI * 1.20 * t).sin();
        let noise: Vec<f64> = (0..NSUB).map(|_| 0.3 * rng.next()).collect();
        last = Some(s.feed(&parse(&frame(i, |k| 60.0 + b + h + noise[k])).unwrap()));
    }
    let r = last.unwrap();
    let br = r.breathing.expect("breathing found");
    assert!((br.bpm - 18.0).abs() < 2.0, "breathing {:.1} bpm", br.bpm);
    let hr = r.heart.expect("heart rate found");
    assert!((hr.bpm - 72.0).abs() < 6.0, "heart {:.1} bpm", hr.bpm);
    assert!(r.presence, "a breathing person is present");
}

#[test]
fn an_empty_room_is_empty_and_movement_shows() {
    let mut s = Sensor::new(FS);
    let mut rng = Lcg(2);
    let mut r = None;
    for i in 0..(30.0 * FS) as u32 {
        let noise: Vec<f64> = (0..NSUB).map(|_| 0.3 * rng.next()).collect();
        r = Some(s.feed(&parse(&frame(i, |k| 60.0 + noise[k])).unwrap()));
    }
    let r = r.unwrap();
    assert!(!r.presence && r.motion < crate::sensor::PRESENCE_MOTION, "empty: motion {:.4}", r.motion);
    assert!(r.breathing.map_or(true, |e| e.confidence < 0.4), "no confident breathing in an empty room");
    // someone walks through: large, irregular changes on every subcarrier for 3 s
    let mut w = None;
    for i in 0..(3.0 * FS) as u32 {
        let jump: Vec<f64> = (0..NSUB).map(|_| 15.0 * rng.next()).collect();
        w = Some(s.feed(&parse(&frame(5000 + i, |k| 60.0 + jump[k])).unwrap()));
    }
    let w = w.unwrap();
    assert!(w.presence && w.motion > crate::sensor::PRESENCE_MOTION, "walking: motion {:.4}", w.motion);
}

#[test]
fn a_perfectly_still_channel_has_no_rhythm() {
    // identical frames: the residuals are only rounding noise, which must not read as breathing
    let mut s = Sensor::new(FS);
    let mut r = None;
    for i in 0..(40.0 * FS) as u32 {
        r = Some(s.feed(&parse(&frame(i, |_| 60.0)).unwrap()));
    }
    let r = r.unwrap();
    assert!(r.breathing.is_none() && r.heart.is_none() && !r.presence, "{:?}", r);
}
