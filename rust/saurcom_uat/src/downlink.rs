//! UAT downlink (ADS-B / TIS-B) message decoding, RTCA DO-282B section 2.2.4: the 18 byte short / 34 byte long frame payload becomes one flat record.
//! Port of `uat_decode_adsb_mdb` from dump978 (`uat_decode.c`). Reads past the end of the frame give 0.

/// Address qualifier values (DO-282B table 2-11), kept as plain numbers for the C ABI.
pub const ALT_INVALID: u8 = 0;
/// Barometric altitude.
pub const ALT_BARO: u8 = 1;
/// Geometric altitude.
pub const ALT_GEO: u8 = 2;
/// Air / ground state: subsonic.
pub const AG_SUBSONIC: u8 = 0;
/// Air / ground state: supersonic.
pub const AG_SUPERSONIC: u8 = 1;
/// Air / ground state: on the ground.
pub const AG_GROUND: u8 = 2;
/// Track type: none.
pub const TT_INVALID: u8 = 0;
/// Track angle.
pub const TT_TRACK: u8 = 1;
/// Magnetic heading.
pub const TT_MAG_HEADING: u8 = 2;
/// True heading.
pub const TT_TRUE_HEADING: u8 = 3;
/// Heading type: not valid.
pub const HT_INVALID: u8 = 0;
/// Heading reference magnetic.
pub const HT_MAGNETIC: u8 = 1;
/// Heading reference true.
pub const HT_TRUE: u8 = 2;
/// Call sign field not valid.
pub const CS_INVALID: u8 = 0;
/// Call sign field holds a call sign.
pub const CS_CALLSIGN: u8 = 1;
/// Call sign field holds a squawk.
pub const CS_SQUAWK: u8 = 2;

/// One decoded downlink message; every field is what the C struct `uat_adsb_mdb` holds, flags are 0 / 1 bytes. Layout is `repr(C)` (see rs/uat.h).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Adsb {
    /// Flags: state vector, mode status, auxiliary state vector present.
    pub has_sv: u8,
    pub has_ms: u8,
    pub has_auxsv: u8,
    /// Flags: position valid, N/S velocity valid, E/W velocity valid, speed valid, dimensions valid, position offset applied, UTC coupled.
    pub position_valid: u8,
    pub ns_vel_valid: u8,
    pub ew_vel_valid: u8,
    pub speed_valid: u8,
    pub dimensions_valid: u8,
    pub position_offset: u8,
    pub utc_coupled: u8,
    /// Capability and mode flags of the mode status element.
    pub has_cdti: u8,
    pub has_acas: u8,
    pub acas_ra_active: u8,
    pub ident_active: u8,
    pub atc_services: u8,
    /// Message type 0..31 and address qualifier 0..7.
    pub mdb_type: u8,
    pub address_qualifier: u8,
    pub nic: u8,
    pub altitude_type: u8,
    pub airground_state: u8,
    pub track_type: u8,
    pub vert_rate_source: u8,
    pub tisb_site_id: u8,
    pub emitter_category: u8,
    pub callsign_type: u8,
    pub emergency_status: u8,
    pub uat_version: u8,
    pub sil: u8,
    pub transmit_mso: u8,
    pub nac_p: u8,
    pub nac_v: u8,
    pub nic_baro: u8,
    pub heading_type: u8,
    pub sec_altitude_type: u8,
    /// NUL terminated call sign (8 characters, trailing spaces removed).
    pub callsign: [u8; 9],
    /// 24 bit ICAO / other address.
    pub address: u32,
    pub altitude: i32,
    pub sec_altitude: i32,
    pub lat: f64,
    pub lon: f64,
    pub length: f64,
    pub width: f64,
    pub ns_vel: i16,
    pub ew_vel: i16,
    pub vert_rate: i16,
    pub track: u16,
    pub speed: u16,
}

impl Adsb {
    /// An all-zero record (what the C++ reference starts from).
    pub const fn zero() -> Adsb {
        Adsb {
            has_sv: 0,
            has_ms: 0,
            has_auxsv: 0,
            position_valid: 0,
            ns_vel_valid: 0,
            ew_vel_valid: 0,
            speed_valid: 0,
            dimensions_valid: 0,
            position_offset: 0,
            utc_coupled: 0,
            has_cdti: 0,
            has_acas: 0,
            acas_ra_active: 0,
            ident_active: 0,
            atc_services: 0,
            mdb_type: 0,
            address_qualifier: 0,
            nic: 0,
            altitude_type: 0,
            airground_state: 0,
            track_type: 0,
            vert_rate_source: 0,
            tisb_site_id: 0,
            emitter_category: 0,
            callsign_type: 0,
            emergency_status: 0,
            uat_version: 0,
            sil: 0,
            transmit_mso: 0,
            nac_p: 0,
            nac_v: 0,
            nic_baro: 0,
            heading_type: 0,
            sec_altitude_type: 0,
            callsign: [0; 9],
            address: 0,
            altitude: 0,
            sec_altitude: 0,
            lat: 0.0,
            lon: 0.0,
            length: 0.0,
            width: 0.0,
            ns_vel: 0,
            ew_vel: 0,
            vert_rate: 0,
            track: 0,
            speed: 0,
        }
    }
}

/// Math the decoder needs from the C library (`atan2`, `sqrt`), passed in so this module stays free of `unsafe`.
pub struct Math {
    /// `atan2(y, x)` of the C library.
    pub atan2: fn(f64, f64) -> f64,
    /// `sqrt(x)` of the C library.
    pub sqrt: fn(f64) -> f64,
}

/// Dimension table: aircraft width in metres by the length / width code of a ground state vector (DO-282B table 2-14).
const WIDTHS: [f64; 16] = [11.5, 23.0, 28.5, 34.0, 33.0, 38.0, 39.5, 45.0, 45.0, 52.0, 59.5, 67.0, 72.5, 80.0, 80.0, 90.0];
/// Base-40 alphabet of call signs (DO-282B section 2.2.4.5.6).
const BASE40: &[u8; 40] = b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ  ..";

/// Byte `i` of the frame, 0 beyond its end.
fn b(f: &[u8], i: usize) -> u32 {
    f.get(i).copied().unwrap_or(0) as u32
}

/// State vector element (DO-282B 2.2.4.5.4): NIC, position (24 bit lat / lon), altitude, velocity or ground speed / track, TIS-B site, UTC coupling.
fn decode_sv(f: &[u8], m: &mut Adsb, math: &Math) {
    m.has_sv = 1;
    m.nic = (b(f, 11) & 15) as u8;
    let raw_lat = (b(f, 4) << 15) | (b(f, 5) << 7) | (b(f, 6) >> 1);
    let raw_lon = ((b(f, 6) & 1) << 23) | (b(f, 7) << 15) | (b(f, 8) << 7) | (b(f, 9) >> 1);
    if m.nic != 0 || raw_lat != 0 || raw_lon != 0 {
        m.position_valid = 1;
        m.lat = raw_lat as f64 * 360.0 / 16777216.0;
        if m.lat > 90.0 {
            m.lat -= 180.0;
        }
        m.lon = raw_lon as f64 * 360.0 / 16777216.0;
        if m.lon > 180.0 {
            m.lon -= 360.0;
        }
    }
    let raw_alt = (b(f, 10) << 4) | ((b(f, 11) & 0xf0) >> 4);
    if raw_alt != 0 {
        m.altitude_type = if b(f, 9) & 1 != 0 { ALT_GEO } else { ALT_BARO };
        m.altitude = (raw_alt as i32 - 1) * 25 - 1000;
    }
    m.airground_state = ((b(f, 12) >> 6) & 3) as u8;
    match m.airground_state {
        AG_SUBSONIC | AG_SUPERSONIC => {
            let scale: i32 = if m.airground_state == AG_SUPERSONIC { 4 } else { 1 };
            let raw_ns = ((b(f, 12) & 0x1f) << 6) | ((b(f, 13) & 0xfc) >> 2);
            if raw_ns & 0x3ff != 0 {
                m.ns_vel_valid = 1;
                let mut v = (raw_ns & 0x3ff) as i32 - 1;
                if raw_ns & 0x400 != 0 {
                    v = -v;
                }
                m.ns_vel = (v * scale) as i16;
            }
            let raw_ew = ((b(f, 13) & 3) << 9) | (b(f, 14) << 1) | ((b(f, 15) & 0x80) >> 7);
            if raw_ew & 0x3ff != 0 {
                m.ew_vel_valid = 1;
                let mut v = (raw_ew & 0x3ff) as i32 - 1;
                if raw_ew & 0x400 != 0 {
                    v = -v;
                }
                m.ew_vel = (v * scale) as i16;
            }
            if m.ns_vel_valid != 0 && m.ew_vel_valid != 0 {
                if m.ns_vel != 0 || m.ew_vel != 0 {
                    m.track_type = TT_TRACK;
                    let a = (math.atan2)(m.ns_vel as f64, m.ew_vel as f64) * 180.0 / core::f64::consts::PI;
                    m.track = ((360.0 + 90.0 - a) as u16) % 360;
                }
                m.speed_valid = 1;
                let (n, e) = (m.ns_vel as i32, m.ew_vel as i32);
                m.speed = (math.sqrt)((n * n + e * e) as f64) as i32 as u16;
            }
            let raw_vvel = ((b(f, 15) & 0x7f) << 4) | ((b(f, 16) & 0xf0) >> 4);
            if raw_vvel & 0x1ff != 0 {
                m.vert_rate_source = if raw_vvel & 0x400 != 0 { ALT_BARO } else { ALT_GEO };
                let mut v = ((raw_vvel & 0x1ff) as i32 - 1) * 64;
                if raw_vvel & 0x200 != 0 {
                    v = -v;
                }
                m.vert_rate = v as i16;
            }
        }
        AG_GROUND => {
            let raw_gs = ((b(f, 12) & 0x1f) << 6) | ((b(f, 13) & 0xfc) >> 2);
            if raw_gs != 0 {
                m.speed_valid = 1;
                m.speed = ((raw_gs & 0x3ff) as i32 - 1) as u16;
            }
            let raw_track = ((b(f, 13) & 3) << 9) | (b(f, 14) << 1) | ((b(f, 15) & 0x80) >> 7);
            match (raw_track & 0x0600) >> 9 {
                1 => m.track_type = TT_TRACK,
                2 => m.track_type = TT_MAG_HEADING,
                3 => m.track_type = TT_TRUE_HEADING,
                _ => {}
            }
            if m.track_type != TT_INVALID {
                m.track = ((raw_track & 0x1ff) * 360 / 512) as u16;
            }
            m.dimensions_valid = 1;
            m.length = (15 + 10 * ((b(f, 15) & 0x38) >> 3)) as f64;
            m.width = WIDTHS[((b(f, 15) & 0x78) >> 3) as usize];
            m.position_offset = (b(f, 15) & 0x04 != 0) as u8;
        }
        _ => {}
    }
    let q = b(f, 0) & 7;
    if q == 2 || q == 3 {
        m.utc_coupled = 0;
        m.tisb_site_id = (b(f, 16) & 0x0f) as u8;
    } else {
        m.utc_coupled = (b(f, 16) & 0x08 != 0) as u8;
        m.tisb_site_id = 0;
    }
}

/// Mode status element (DO-282B 2.2.4.5.6): emitter category, call sign / squawk, emergency, version, SIL, MSO, NACp / NACv, NICbaro, capabilities.
fn decode_ms(f: &[u8], m: &mut Adsb) {
    m.has_ms = 1;
    let mut v = (b(f, 17) << 8) | b(f, 18);
    m.emitter_category = ((v / 1600) % 40) as u8;
    m.callsign[0] = BASE40[((v / 40) % 40) as usize];
    m.callsign[1] = BASE40[(v % 40) as usize];
    v = (b(f, 19) << 8) | b(f, 20);
    m.callsign[2] = BASE40[((v / 1600) % 40) as usize];
    m.callsign[3] = BASE40[((v / 40) % 40) as usize];
    m.callsign[4] = BASE40[(v % 40) as usize];
    v = (b(f, 21) << 8) | b(f, 22);
    m.callsign[5] = BASE40[((v / 1600) % 40) as usize];
    m.callsign[6] = BASE40[((v / 40) % 40) as usize];
    m.callsign[7] = BASE40[(v % 40) as usize];
    m.callsign[8] = 0;
    for i in (0..8).rev() {
        if m.callsign[i] == b' ' {
            m.callsign[i] = 0;
        } else {
            break;
        }
    }
    m.emergency_status = ((b(f, 23) >> 5) & 7) as u8;
    m.uat_version = ((b(f, 23) >> 2) & 7) as u8;
    m.sil = (b(f, 23) & 3) as u8;
    m.transmit_mso = ((b(f, 24) >> 2) & 0x3f) as u8;
    m.nac_p = ((b(f, 25) >> 4) & 15) as u8;
    m.nac_v = ((b(f, 25) >> 1) & 7) as u8;
    m.nic_baro = (b(f, 25) & 1) as u8;
    m.has_cdti = (b(f, 26) & 0x80 != 0) as u8;
    m.has_acas = (b(f, 26) & 0x40 != 0) as u8;
    m.acas_ra_active = (b(f, 26) & 0x20 != 0) as u8;
    m.ident_active = (b(f, 26) & 0x10 != 0) as u8;
    m.atc_services = (b(f, 26) & 0x08 != 0) as u8;
    m.heading_type = if b(f, 26) & 0x04 != 0 { HT_MAGNETIC } else { HT_TRUE };
    if m.callsign[0] != 0 {
        m.callsign_type = if b(f, 26) & 0x02 != 0 { CS_CALLSIGN } else { CS_SQUAWK };
    }
}

/// Auxiliary state vector element (DO-282B 2.2.4.5.7): the secondary altitude (the other kind than the state vector's).
fn decode_auxsv(f: &[u8], m: &mut Adsb) {
    let raw_alt = (b(f, 29) << 4) | ((b(f, 30) & 0xf0) >> 4);
    if raw_alt != 0 {
        m.sec_altitude = (raw_alt as i32 - 1) * 25 - 1000;
        m.sec_altitude_type = if b(f, 9) & 1 != 0 { ALT_BARO } else { ALT_GEO };
    } else {
        m.sec_altitude_type = ALT_INVALID;
    }
    m.has_auxsv = 1;
}

/// Decodes one downlink payload (DO-282B 2.2.4): the header (type, qualifier, address) selects which elements follow (table 2-10).
pub fn decode_adsb(f: &[u8], math: &Math) -> Adsb {
    let mut m = Adsb::zero();
    m.mdb_type = ((b(f, 0) >> 3) & 0x1f) as u8;
    m.address_qualifier = (b(f, 0) & 7) as u8;
    m.address = (b(f, 1) << 16) | (b(f, 2) << 8) | b(f, 3);
    match m.mdb_type {
        0 | 4 | 7..=10 => decode_sv(f, &mut m, math),
        1 => {
            decode_sv(f, &mut m, math);
            decode_ms(f, &mut m);
            decode_auxsv(f, &mut m);
        }
        2 | 5 | 6 => {
            decode_sv(f, &mut m, math);
            decode_auxsv(f, &mut m);
        }
        3 => {
            decode_sv(f, &mut m, math);
            decode_ms(f, &mut m);
        }
        _ => {}
    }
    m
}
