//! One ESP32 node's pipeline: CSI frame -> static removal -> breathing and heart rate (vitals.rs), plus a presence / motion score.
//! The score is this crate's own simple heuristic (RuView keeps its presence classifier in its server): the mean absolute residual over the
//! subcarriers relative to the mean amplitude, smoothed over about one second. People moving disturb the channel far more than an empty room.

use crate::frame::Frame;
use crate::vitals::{Breathing, Estimate, HeartRate, Preprocessor};

/// Motion score above which the room counts as occupied (an empty room with RuView's ESP32 nodes stays well under it).
pub const PRESENCE_MOTION: f64 = 0.02;

pub struct Sensor {
    pre: Preprocessor,
    breathing: Breathing,
    heart: HeartRate,
    n_subcarriers: usize,
    fs: f64,
    motion: f64,
    last_breathing: Option<(Estimate, u64)>,   // the estimate and the frame it came with
    last_heart: Option<(Estimate, u64)>,
    frames: u64,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct Reading {
    pub motion: f64,
    pub presence: bool,
    pub breathing: Option<Estimate>,
    pub heart: Option<Estimate>,
    pub frames: u64,
}

impl Sensor {
    /// fs: frames per second the node sends (RuView firmware: 50 by default, 8 - 50)
    pub fn new(fs: f64) -> Self {
        let fs = if fs.is_finite() && fs >= 1.0 { fs } else { 50.0 };
        Sensor {
            pre: Preprocessor::new(0, 0.05),
            breathing: Breathing::new(fs, 30.0),
            heart: HeartRate::new(fs, 15.0),
            n_subcarriers: 0,
            fs,
            motion: 0.0,
            last_breathing: None,
            last_heart: None,
            frames: 0,
        }
    }

    pub fn feed(&mut self, f: &Frame) -> Reading {
        if f.n_subcarriers != self.n_subcarriers {   // a new layout (another bandwidth): start again
            *self = Sensor::new(self.fs);
            self.n_subcarriers = f.n_subcarriers;
            self.pre = Preprocessor::new(f.n_subcarriers, 0.05);
        }
        self.frames += 1;
        let residuals = self.pre.process(&f.amplitudes);
        let mean_amp = f.amplitudes.iter().sum::<f64>() / f.amplitudes.len().max(1) as f64;
        let mean_res = residuals.iter().map(|r| if *r < 0.0 { -r } else { *r }).sum::<f64>() / residuals.len().max(1) as f64;
        let rel = if mean_amp > 1e-9 { mean_res / mean_amp } else { 0.0 };
        let k = 1.0 / self.fs;   // about one second of smoothing
        if rel.is_finite() {
            self.motion += k * (rel - self.motion);
        }
        if let Some(e) = self.breathing.extract(&residuals) { self.last_breathing = Some((e, self.frames)); }
        if let Some(e) = self.heart.extract(&residuals, &f.phases) { self.last_heart = Some((e, self.frames)); }
        // an estimate is kept for 5 s; without a newer one it is gone (the person left, or the rhythm was lost)
        let fresh = |v: Option<(Estimate, u64)>| v.filter(|(_, at)| ((self.frames - at) as f64) < 5.0 * self.fs).map(|(e, _)| e);
        let (breathing, heart) = (fresh(self.last_breathing), fresh(self.last_heart));
        Reading {
            motion: self.motion,
            presence: self.motion > PRESENCE_MOTION || breathing.is_some_and(|e| e.confidence >= 0.4),
            breathing,
            heart,
            frames: self.frames,
        }
    }
}
