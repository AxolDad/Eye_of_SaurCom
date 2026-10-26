//! UAT uplink frame decoding, RTCA DO-282B section 2.1.2 and the FIS-B APDU header of RTCA DO-358B: ground station position, slot, TIS-B site, the 424 byte
//! application data split into information frames (length, type) and, for FIS-B frames, the APDU header (flags, product id, time stamp).
//! Port of `uat_decode_uplink_mdb` / `uat_decode_info_frame` from dump978 (`uat_decode.c`). Reads past the end of the frame give 0.

/// Bytes of application data in an uplink frame.
pub const APP_DATA: usize = 424;
/// Most information frames one uplink frame can hold (a frame is at least 6 bytes in the reference's sizing).
pub const MAX_INFO_FRAMES: usize = APP_DATA / 6;

/// FIS-B APDU header (DO-358B table 2-1), `repr(C)`; `data_off` is the offset of the product data inside the application data.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Fisb {
    /// APDU flags: a (application), g (geographic), p (product), s (segmentation).
    pub a_flag: u8,
    pub g_flag: u8,
    pub p_flag: u8,
    pub s_flag: u8,
    /// Month / day present, seconds present.
    pub monthday_valid: u8,
    pub seconds_valid: u8,
    pub month: u8,
    pub day: u8,
    pub hours: u8,
    pub minutes: u8,
    pub seconds: u8,
    /// 11 bit product id (0 METAR, 1 TAF, 2 NEXRAD, ...).
    pub product_id: u16,
    /// Length of the product data and its offset in the application data.
    pub length: u16,
    pub data_off: u16,
}

/// One information frame of the uplink application data, `repr(C)`; `data_off` is the offset of its payload in the application data.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Info {
    /// 1 when the frame is a FIS-B APDU and `fisb` is meaningful.
    pub is_fisb: u8,
    /// Frame type (low four bits of the second header byte).
    pub kind: u8,
    /// 9 bit payload length.
    pub length: u16,
    pub data_off: u16,
    pub fisb: Fisb,
}

/// A decoded uplink frame, `repr(C)`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Uplink {
    /// Flags: position valid, UTC coupled, application data valid.
    pub position_valid: u8,
    pub utc_coupled: u8,
    pub app_data_valid: u8,
    /// Slot id (5 bits) and TIS-B site id (4 bits).
    pub slot_id: u8,
    pub tisb_site_id: u8,
    /// Number of entries of `info` that are valid.
    pub num_info_frames: u8,
    /// 1 when the scan stopped at a frame it did not count (overrun or empty): `tail_length` / `tail_kind` are what the reference stored in that slot.
    pub tail_valid: u8,
    pub tail_kind: u8,
    pub tail_length: u16,
    /// Ground station position in degrees.
    pub lat: f64,
    pub lon: f64,
    pub info: [Info; MAX_INFO_FRAMES],
}

/// Byte `i` of the frame, 0 beyond its end.
fn b(f: &[u8], i: usize) -> u32 {
    f.get(i).copied().unwrap_or(0) as u32
}

/// Decodes the header of one information frame at `data` (offset of its first payload byte): a FIS-B APDU (type 0, at least 4 bytes) gets its flags,
/// product id and time stamp (DO-358B 2.2.2: t_opt picks hours / minutes, + seconds, + month / day, + both). Frames that are not FIS-B or too short come back with `is_fisb` 0.
fn decode_info(app: &[u8], mut info: Info) -> Info {
    info.is_fisb = 0;
    if info.kind != 0 || info.length < 4 {
        return info;
    }
    let d = info.data_off as usize;
    let by = |i: usize| b(app, d + i);
    let t_opt = ((by(1) & 1) << 1) | (by(2) >> 7);
    let len = info.length;
    let mut f = Fisb::default();
    match t_opt {
        0 => {
            f.hours = ((by(2) & 0x7c) >> 2) as u8;
            f.minutes = (((by(2) & 3) << 4) | (by(3) >> 4)) as u8;
            f.length = len - 4;
            f.data_off = (d + 4) as u16;
        }
        1 => {
            if len < 5 {
                return info;
            }
            f.seconds_valid = 1;
            f.hours = ((by(2) & 0x7c) >> 2) as u8;
            f.minutes = (((by(2) & 3) << 4) | (by(3) >> 4)) as u8;
            f.seconds = (((by(3) & 0x0f) << 2) | (by(4) >> 6)) as u8;
            f.length = len - 5;
            f.data_off = (d + 5) as u16;
        }
        2 => {
            if len < 5 {
                return info;
            }
            f.monthday_valid = 1;
            f.month = ((by(2) & 0x78) >> 3) as u8;
            f.day = (((by(2) & 7) << 2) | (by(3) >> 6)) as u8;
            f.hours = ((by(3) & 0x3e) >> 1) as u8;
            f.minutes = (((by(3) & 1) << 5) | (by(4) >> 3)) as u8;
            f.length = len - 5;
            f.data_off = (d + 5) as u16;
        }
        _ => {
            if len < 6 {
                return info;
            }
            f.monthday_valid = 1;
            f.seconds_valid = 1;
            f.month = ((by(2) & 0x78) >> 3) as u8;
            f.day = (((by(2) & 7) << 2) | (by(3) >> 6)) as u8;
            f.hours = ((by(3) & 0x3e) >> 1) as u8;
            f.minutes = (((by(3) & 1) << 5) | (by(4) >> 3)) as u8;
            f.seconds = (((by(4) & 3) << 3) | (by(5) >> 5)) as u8;
            f.length = len - 6;
            f.data_off = (d + 6) as u16;
        }
    }
    f.a_flag = (by(0) & 0x80 != 0) as u8;
    f.g_flag = (by(0) & 0x40 != 0) as u8;
    f.p_flag = (by(0) & 0x20 != 0) as u8;
    f.product_id = (((by(0) & 0x1f) << 6) | (by(1) >> 2)) as u16;
    f.s_flag = (by(1) & 2 != 0) as u8;
    info.is_fisb = 1;
    info.fisb = f;
    info
}

/// Decodes an uplink frame (432 data bytes: 8 header bytes, 424 application bytes). The information frames refer to offsets in the application data, which
/// is `frame[8..432]`; `app_data_valid` says whether they were scanned.
pub fn decode_uplink(f: &[u8]) -> Uplink {
    let mut m = Uplink {
        position_valid: (b(f, 5) & 1 != 0) as u8,
        utc_coupled: 0,
        app_data_valid: 0,
        slot_id: 0,
        tisb_site_id: 0,
        num_info_frames: 0,
        tail_valid: 0,
        tail_kind: 0,
        tail_length: 0,
        lat: 0.0,
        lon: 0.0,
        info: [Info::default(); MAX_INFO_FRAMES],
    };
    // Position is decoded even when the valid bit is clear (plausible data is there, per the reference).
    let raw_lat = (b(f, 0) << 15) | (b(f, 1) << 7) | (b(f, 2) >> 1);
    let raw_lon = ((b(f, 2) & 1) << 23) | (b(f, 3) << 15) | (b(f, 4) << 7) | (b(f, 5) >> 1);
    m.lat = raw_lat as f64 * 360.0 / 16777216.0;
    if m.lat > 90.0 {
        m.lat -= 180.0;
    }
    m.lon = raw_lon as f64 * 360.0 / 16777216.0;
    if m.lon > 180.0 {
        m.lon -= 360.0;
    }
    m.utc_coupled = (b(f, 6) & 0x80 != 0) as u8;
    m.app_data_valid = (b(f, 6) & 0x20 != 0) as u8;
    m.slot_id = (b(f, 6) & 0x1f) as u8;
    m.tisb_site_id = (b(f, 7) >> 4) as u8;
    if m.app_data_valid == 0 {
        return m;
    }
    let mut app = [0u8; APP_DATA];
    for (i, a) in app.iter_mut().enumerate() {
        *a = b(f, 8 + i) as u8;
    }
    let mut pos = 0usize;
    while (m.num_info_frames as usize) < MAX_INFO_FRAMES && pos + 2 <= APP_DATA {
        let length = ((app[pos] as u16) << 1) | (app[pos + 1] >> 7) as u16;
        let kind = app[pos + 1] & 0x0f;
        if pos + length as usize + 2 > APP_DATA {
            m.tail_valid = 1;
            m.tail_length = length;
            m.tail_kind = kind;
            break;
        }
        if length == 0 && kind == 0 {
            m.tail_valid = 1;
            m.tail_length = length;
            m.tail_kind = kind;
            break;
        }
        let info = Info { kind, length, data_off: (pos + 2) as u16, ..Info::default() };
        m.info[m.num_info_frames as usize] = decode_info(&app, info);
        pos += length as usize + 2;
        m.num_info_frames += 1;
    }
    m
}
