//! The raw CSI frame of RuView's ESP32 firmware (ADR-018, firmware/esp32-csi-node/main/csi_collector.c), one UDP datagram per frame:
//! `[0..4]` magic 0xC5110001 LE, `[4]` node id, `[5]` antennas, `[6..8]` subcarriers LE, `[8..12]` frequency MHz LE, `[12..16]` sequence LE,
//! `[16]` RSSI (i8), `[17]` noise floor (i8), `[18]` PPDU type, `[19]` flags, then int8 I,Q pairs, antenna-major.

use crate::mathf::{atan2, sqrt};
use alloc::vec::Vec;

pub const MAGIC: u32 = 0xC511_0001;
pub const HEADER: usize = 20;
pub const MAX_ANTENNAS: usize = 4;
pub const MAX_SUBCARRIERS: usize = 256;

#[derive(Clone, Debug, Default)]
pub struct Frame {
    pub node_id: u8,
    pub n_antennas: usize,
    pub n_subcarriers: usize,
    pub freq_mhz: u32,
    pub seq: u32,
    pub rssi: i8,
    pub noise: i8,
    /// per subcarrier: |H| averaged over the antennas
    pub amplitudes: Vec<f64>,
    /// per subcarrier: the phase of the first antenna (radians)
    pub phases: Vec<f64>,
}

fn u16le(b: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes([*b.get(at)?, *b.get(at + 1)?]))
}
fn u32le(b: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes([*b.get(at)?, *b.get(at + 1)?, *b.get(at + 2)?, *b.get(at + 3)?]))
}

/// One datagram as a frame; None unless it is a whole, well-formed ADR-018 CSI frame.
pub fn parse(b: &[u8]) -> Option<Frame> {
    if u32le(b, 0)? != MAGIC {
        return None;
    }
    let n_ant = usize::from(*b.get(5)?);
    let n_sub = usize::from(u16le(b, 6)?);
    if n_ant == 0 || n_ant > MAX_ANTENNAS || n_sub == 0 || n_sub > MAX_SUBCARRIERS {
        return None;
    }
    let need = HEADER + n_ant * n_sub * 2;
    let iq = b.get(HEADER..need)?;
    let mut f = Frame {
        node_id: b[4],
        n_antennas: n_ant,
        n_subcarriers: n_sub,
        freq_mhz: u32le(b, 8)?,
        seq: u32le(b, 12)?,
        rssi: b[16] as i8,
        noise: b[17] as i8,
        amplitudes: Vec::with_capacity(n_sub),
        phases: Vec::with_capacity(n_sub),
    };
    for k in 0..n_sub {
        let mut amp = 0.0;
        for a in 0..n_ant {
            let at = (a * n_sub + k) * 2;
            let (i, q) = (f64::from(iq[at] as i8), f64::from(iq[at + 1] as i8));
            amp += sqrt(i * i + q * q);
            if a == 0 {
                f.phases.push(atan2(q, i));
            }
        }
        f.amplitudes.push(amp / n_ant as f64);
    }
    Some(f)
}
