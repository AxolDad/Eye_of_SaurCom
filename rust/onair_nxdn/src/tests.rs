//! Unit tests for the NXDN layer: LICH round trip, CRC sensitivity, channel encode / decode with bit errors, message round trips.
use crate::chan::{self, Ch};
use crate::lich::{self, Lich};
use crate::{crc, msg};

#[test]
fn lich_round_trip() {
    for v in 0..128 {
        let l = Lich { rfct: (v >> 5) & 3, fct: (v >> 3) & 3, opt: (v >> 1) & 3, dir: v & 1 };
        let e = lich::encode(&l);
        assert_eq!(lich::decode(e), Some(l));
        assert_eq!(lich::decode(e ^ 0x80), None);
    }
}

#[test]
fn pn9_starts_with_seed() {
    let mut p = [0u8; 8];
    lich::pn9(&mut p);
    assert_eq!(p, [0, 0, 1, 0, 0, 1, 1, 1]);
}

#[test]
fn crc_catches_bit_error() {
    let mut b = [0u8; 40];
    for (i, x) in b.iter_mut().enumerate() {
        *x = ((i * 7 + 3) % 5 < 2) as u8;
    }
    let (a, c, d) = (crc::crc6(&b), crc::crc12(&b), crc::crc16(&b));
    b[13] ^= 1;
    assert!(crc::crc6(&b) != a && crc::crc12(&b) != c && crc::crc16(&b) != d);
}

#[test]
fn channels_round_trip_and_fix_errors() {
    for (c, fix) in [(Ch::Sacch, 3usize), (Ch::Facch1, 3), (Ch::Cac, 10)] {
        let ni = chan::info_bits(c);
        let na = chan::air_bits(c);
        let mut info = [0u8; 155];
        for (i, x) in info.iter_mut().enumerate() {
            *x = ((i * 13 + 5) % 7 < 3) as u8;
        }
        let mut air = [0u8; 300];
        chan::encode(c, &info[..ni], &mut air[..na]).unwrap();
        let mut soft = [0f32; 300];
        for k in 0..na {
            soft[k] = if air[k] != 0 { 1.0 } else { -1.0 };
        }
        for e in 0..fix {
            let k = (e * 977 + 11) % na;
            soft[k] = -soft[k];
        }
        let mut out = [0u8; 155];
        let d = chan::decode(c, &soft[..na], &mut out[..ni]).unwrap();
        assert!(d.ok);
        assert_eq!(&out[..ni], &info[..ni]);
        assert_eq!(d.errs as usize, fix);
    }
}

#[test]
fn message_round_trips() {
    let mut m = msg::Msg { typ: msg::VCALL, call_type: 4, option: 9, src: 1234, dst: 4321, cipher: 2, key_id: 17, ..Default::default() };
    let mut bits = [0u8; 72];
    assert_eq!(msg::build(&m, &mut bits), 56);
    let mut p = msg::Msg::default();
    assert!(msg::parse(&bits[..56], &mut p));
    assert_eq!(p, m);
    m = msg::Msg { typ: msg::SD_DATA, block: 3, text: *b"HELLO\0\0\0", text_len: 5, ..Default::default() };
    assert_eq!(msg::build(&m, &mut bits), 80);
    assert!(!msg::parse(&bits[..70], &mut p));
    assert_eq!(p.typ, msg::SD_DATA);
}

#[test]
fn voice_channel_assignment_layout() {
    // msg::VCALL_ASSGN written out by hand (TS 1-D): type 0x04, CC option 0x80 (emergency), group call 1 with option 0, source 1234,
    // talkgroup 20, call timer 5, channel 321
    let fields: [(u32, u32); 8] = [(0x04, 8), (0x80, 8), (1, 3), (0, 5), (1234, 16), (20, 16), (5, 6), (321, 10)];
    let mut bits = std::vec::Vec::new();
    for (v, w) in fields {
        for i in (0..w).rev() {
            bits.push(((v >> i) & 1) as u8);
        }
    }
    let mut m = msg::Msg::default();
    assert!(msg::parse(&bits, &mut m));
    assert_eq!((m.typ, m.cc_option, m.call_type, m.src, m.dst, m.timer, m.channel), (msg::VCALL_ASSGN, 0x80, 1, 1234, 20, 5, 321));
    let mut out = [0u8; 80];
    assert_eq!(msg::build(&m, &mut out), 72);
    assert_eq!(&out[..72], &bits[..]);
    assert!(!msg::parse(&bits[..71], &mut m), "short body refused");
    assert_eq!(msg::msg_name(msg::VCALL_ASSGN), "VCALL_ASSGN\0");
}
